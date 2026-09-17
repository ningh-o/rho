#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// arm64 (Apple Silicon) emitter — spill-everything, mirrors emit_amd64.
// Scratch registers are free between instructions; every vreg lives in a
// stack slot addressed off the frame pointer (x29).

typedef struct Emitter64 {
  Target tgt;
  SB *out;
  IRFn *fn;
  Vec slot_off;
  int64_t frame;
  IRBlock *cur;
} Emitter64;

static const char *X(int64_t off) {
  static char buf[4][16];
  static int r = 0;
  r = (r + 1) % 4;
  (void)off;
  return "x8"; // unused shim; real addressing goes through slot operands
}

static int64_t slot_off(Emitter64 *e, IRSlot *s) {
  if (s->id < (int)e->slot_off.n)
    return (long)e->slot_off.items[s->id];
  int64_t off = e->frame;
  int64_t al = s->align > 8 ? 8 : s->align;
  off = (off + al - 1) / al * al;
  off += s->size;
  e->frame = off;
  while ((int)e->slot_off.n <= s->id)
    vec_push(&e->slot_off, (void *)0);
  e->slot_off.items[s->id] = (void *)(long)(-off);
  return -off;
}

static int64_t vreg_off(Emitter64 *e, IRVreg *v) {
  size_t idx = e->fn->slots.n + (size_t)v->id;
  while (e->slot_off.n <= idx)
    vec_push(&e->slot_off, (void *)0);
  return (long)e->slot_off.items[idx];
}

static void layout_frame64(Emitter64 *e) {
  e->slot_off = (Vec){0};
  e->frame = 0;
  for (size_t i = 0; i < e->fn->slots.n; i++)
    slot_off(e, e->fn->slots.items[i]);
  for (int v = 0; v < e->fn->next_vreg; v++) {
    e->frame += 8;
    vec_push(&e->slot_off, (void *)(long)(-e->frame));
  }
  e->frame = (e->frame + 15) / 16 * 16 + 16;
}

// x12 is the dedicated address scratch for slot access
// imm12 caps at 4095; for big frames walk the offset in 4080-sized steps,
// chaining through x12 (x29 stays intact — the epilogue restores sp from it)
static void addr_into(Emitter64 *e, int64_t off) {
  (void)e;
  int64_t left = off < 0 ? -off : off;
  const char *op = off < 0 ? "sub" : "add";
  const char *base = "x29";
  while (left > 4095) {
    sb_printf(e->out, "  %s x12, %s, #4080\n", op, base);
    base = "x12";
    left -= 4080;
  }
  sb_printf(e->out, "  %s x12, %s, #%lld\n", op, base, (long long)left);
}

static void ld(Emitter64 *e, int64_t off, int64_t size, bool flt, const char *reg) {
  addr_into(e, off);
  if (flt) {
    sb_printf(e->out, "  ldr %s, [x12]\n", reg);
    return;
  }
  if (size == 1)
    sb_printf(e->out, "  ldrsb %s, [x12]\n", reg);
  else if (size == 2)
    sb_printf(e->out, "  ldrsh %s, [x12]\n", reg);
  else if (size == 4)
    sb_printf(e->out, "  ldrsw %s, [x12]\n", reg);
  else
    sb_printf(e->out, "  ldr %s, [x12]\n", reg);
}

static void st_w4(Emitter64 *e, const char *reg, int64_t off) {
  if (reg[0] == 'x') {
    char wname[8];
    wname[0] = 'w';
    wname[1] = reg[1];
    wname[2] = 0;
    sb_printf(e->out, "  str %s, [x12]\n", wname);
  } else {
    sb_printf(e->out, "  str %s, [x12]\n", reg);
  }
}

static void st_(Emitter64 *e, int64_t off, int64_t size, bool flt, const char *reg) {
  addr_into(e, off);
  if (flt) {
    sb_printf(e->out, "  str %s, [x12]\n", reg);
    return;
  }
  if (size == 1 || size == 2) {
    char wname[8];
    wname[0] = reg[0] == 'x' ? 'w' : reg[0];
    wname[1] = reg[1];
    wname[2] = 0;
    sb_printf(e->out, size == 1 ? "  strb %s, [x12]\n" : "  strh %s, [x12]\n", wname);
  }
  else if (size == 4)
    st_w4(e, reg, off);
  else
    sb_printf(e->out, "  str %s, [x12]\n", reg);
}

static int g_label64 = 0;
static Vec g_fconsts64 = {0}; // FConstNote reuses amd64's struct? no — local

typedef struct FC64 {
  int lbl;
  double v;
} FC64;

static void arm64_note_fconst(int lbl, double v) {
  FC64 *n = arena_alloc(sizeof(FC64));
  n->lbl = lbl;
  n->v = v;
  vec_push(&g_fconsts64, n);
}

static void emit_cmp64(Emitter64 *e, IRIns *i) {
  int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
  if (!i->is_float) {
    bool u = !i->signed_ops;
    const char *cc = i->cc == CC_EQ ? "eq" : i->cc == CC_NE ? "ne"
                                  : i->cc == CC_LT          ? (u ? "lo" : "lt")
                                  : i->cc == CC_LE          ? (u ? "ls" : "le")
                                  : i->cc == CC_GT          ? (u ? "hi" : "gt")
                                                            : (u ? "hs" : "ge");
    ld(e, ao, 8, false, "x8");
    ld(e, bo, 8, false, "x9");
    sb_printf(e->out, "  cmp x8, x9\n  cset x8, %s\n", cc);
    addr_into(e, dof);
    // full-width store: cmp results flow through 8-byte spill moves and
    // returns, and stale high bytes would poison them
    sb_printf(e->out, "  str x8, [x12]\n");
    return;
  }
  bool s64 = i->size == 8;
  const char *a = s64 ? "d0" : "s0", *b = s64 ? "d1" : "s1";
  ld(e, ao, s64 ? 8 : 4, true, a);
  ld(e, bo, s64 ? 8 : 4, true, b);
  sb_printf(e->out, "  fcmp %s, %s\n", a, b);
  switch (i->cc) {
  case CC_EQ: // eq & !vs
    sb_printf(e->out, "  cset x8, eq\n  cset x9, vs\n  eor x8, x8, x9\n");
    break;
  case CC_NE: // ne | vs
    sb_printf(e->out, "  cset x8, ne\n  cset x9, vs\n  orr x8, x8, x9\n");
    break;
  case CC_LT: // mi is NaN-false on unordered
    sb_printf(e->out, "  cset x8, mi\n");
    break;
  case CC_LE: // le & !vs
    sb_printf(e->out, "  cset x8, le\n  cset x9, vs\n  eor x8, x8, x9\n");
    break;
  case CC_GT: // gt is NaN-false
    sb_printf(e->out, "  cset x8, gt\n");
    break;
  case CC_GE: // ge is NaN-false
    sb_printf(e->out, "  cset x8, ge\n");
    break;
  }
  addr_into(e, dof);
  sb_printf(e->out, "  strb w8, [x12]\n");
}

static void emit_divmod64(Emitter64 *e, IRIns *i, bool is_mod) {
  int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
  if (i->is_float) {
    ld(e, ao, 8, true, "d0");
    ld(e, bo, 8, true, "d1");
    sb_printf(e->out, "  fdiv d0, d0, d1\n");
    st_(e, dof, 8, true, "d0");
    return;
  }
  int lbl = g_label64++;
  ld(e, bo, 8, false, "x9");
  sb_printf(e->out, "  cbnz x9, Ldz%d\n", lbl);
  sb_printf(e->out, "  bl _%s\n", prelude_symbol("__panic_div"));
  sb_printf(e->out, "Ldz%d:\n", lbl);
  if (i->signed_ops) {
    sb_printf(e->out, "  cmp x9, #-1\n  b.ne Lmin%d\n", lbl);
    ld(e, ao, 8, false, "x8");
    sb_printf(e->out, "  neg x8, x8\n  mov x9, #0\n  b Ldone%d\n", lbl);
    sb_printf(e->out, "Lmin%d:\n", lbl);
  }
  ld(e, ao, 8, false, "x10");
  sb_printf(e->out, "  %s x8, x10, x9\n", i->signed_ops ? "sdiv" : "udiv");
  if (is_mod)
    sb_printf(e->out, "  msub x8, x8, x9, x10\n");
  sb_printf(e->out, "Ldone%d:\n", lbl);
  st_(e, dof, 8, false, "x8");
}

static const char *sym64(const char *s) {
  return arena_printf("_%s", s);
}

static void emit_call64(Emitter64 *e, IRIns *i) {
  const char *regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
  const char *fregs[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
  int ri = 0, fi = 0;
  if (i->callee_vreg) { // indirect target in x17, outside the arg regs
    addr_into(e, vreg_off(e, i->callee_vreg));
    sb_printf(e->out, "  ldr x17, [x12]\n");
  }
  for (size_t k = 0; k < i->args.n; k++) {
    IRArg *a = i->args.items[k];
    bool isf = a->ty && (a->ty->kind == TY_F32 || a->ty->kind == TY_F64);
    int64_t off = vreg_off(e, a->vreg);
    if (isf && fi < 8) {
      addr_into(e, off);
      sb_printf(e->out, "  ldr %s, [x12]\n", fregs[fi++]);
    } else if (!isf && ri < 8) {
      addr_into(e, off);
      sb_printf(e->out, "  ldr %s, [x12]\n", regs[ri++]);
    } else {
      addr_into(e, off);
      sb_printf(e->out, "  ldr x10, [x12]\n  str x10, [sp, #%d]\n", ri * 8);
    }
  }
  if (i->callee_vreg) {
    sb_printf(e->out, "  blr x17\n");
  } else {
    // debugging aid: trap at the exact call site when an rc helper receives
    // a small non-null value (a discriminant or clobbered slot, never a
    // pointer). Removed by leaving RHO_RC_GUARD unset at emitter build time.
    if (getenv("RHO_RC_GUARD") && i->callee &&
        (!strncmp(i->callee, "rho__rc_dec", 11) ||
         !strncmp(i->callee, "rho__rc_inc", 11))) {
      int gl = g_label64++;
      sb_printf(e->out, "  cbz x0, Lgok%d\n", gl);
      sb_printf(e->out, "  cmp x0, #65536\n");
      sb_printf(e->out, "  b.hs Lgok%d\n", gl);
      sb_printf(e->out, "  brk #3\n");
      sb_printf(e->out, "Lgok%d:\n", gl);
    }
    sb_printf(e->out, "  bl _%s\n", i->callee);
  }
  if (i->dst) {
    bool f = ir_is_float(i->dst->ty);
    if (f)
      st_(e, vreg_off(e, i->dst), i->dst->ty == IT_F32 ? 4 : 8, true, f && i->dst->ty == IT_F32 ? "s0" : "d0");
    else
      st_(e, vreg_off(e, i->dst), 8, false, "x0");
  }
}

static void emit_ins64(Emitter64 *e, IRIns *i) {
  switch (i->op) {
  case IR_CONST: {
    int64_t dof = vreg_off(e, i->dst);
    uint64_t v = (uint64_t)i->imm;
    if (v <= 0xFFFF) {
      sb_printf(e->out, "  mov x8, #%llu\n", (unsigned long long)v);
    } else {
      sb_printf(e->out, "  movz x8, #%llu\n", (unsigned long long)(v & 0xFFFF));
      sb_printf(e->out, "  movk x8, #%llu, lsl 16\n", (unsigned long long)((v >> 16) & 0xFFFF));
      sb_printf(e->out, "  movk x8, #%llu, lsl 32\n", (unsigned long long)((v >> 32) & 0xFFFF));
      sb_printf(e->out, "  movk x8, #%llu, lsl 48\n", (unsigned long long)((v >> 48) & 0xFFFF));
    }
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_FCONST: {
    // materialize the IEEE-754 bit pattern in x8, then move to d0
    uint64_t bits;
    __builtin_memcpy(&bits, &i->fimm, 8);
    sb_printf(e->out, "  movz x8, #%llu\n", (unsigned long long)(bits & 0xFFFF));
    sb_printf(e->out, "  movk x8, #%llu, lsl 16\n", (unsigned long long)((bits >> 16) & 0xFFFF));
    sb_printf(e->out, "  movk x8, #%llu, lsl 32\n", (unsigned long long)((bits >> 32) & 0xFFFF));
    sb_printf(e->out, "  movk x8, #%llu, lsl 48\n", (unsigned long long)((bits >> 48) & 0xFFFF));
    sb_printf(e->out, "  fmov d0, x8\n");
    st_(e, vreg_off(e, i->dst), 8, true, "d0");
    break;
  }
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_AND:
  case IR_OR:
  case IR_XOR: {
    int64_t dof = vreg_off(e, i->dst);
    if (i->is_float) {
      const char *mn = i->op == IR_ADD ? "fadd" : i->op == IR_SUB ? "fsub" : "fmul";
      ld(e, vreg_off(e, i->a), 8, true, "d0");
      ld(e, vreg_off(e, i->b), 8, true, "d1");
      sb_printf(e->out, "  %s d0, d0, d1\n", mn);
      st_(e, dof, 8, true, "d0");
      break;
    }
    const char *mn = i->op == IR_ADD ? "add" : i->op == IR_SUB ? "sub" : i->op == IR_MUL ? "mul"
                                                                     : i->op == IR_AND   ? "and"
                                                                     : i->op == IR_OR    ? "orr"
                                                                                         : "eor";
    ld(e, vreg_off(e, i->a), 8, false, "x8");
    ld(e, vreg_off(e, i->b), 8, false, "x9");
    sb_printf(e->out, "  %s x8, x8, x9\n", mn);
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_DIV:
  case IR_MOD:
    emit_divmod64(e, i, i->op == IR_MOD);
    break;
  case IR_SHL:
  case IR_SHR: {
    int64_t dof = vreg_off(e, i->dst);
    ld(e, vreg_off(e, i->a), 8, false, "x8");
    ld(e, vreg_off(e, i->b), 8, false, "x9");
    const char *mn = i->op == IR_SHL ? "lslv" : i->signed_ops ? "asrv" : "lsrv";
    sb_printf(e->out, "  %s x8, x8, x9\n", mn);
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_CMP:
    emit_cmp64(e, i);
    break;
  case IR_LOAD: {
    int64_t dof = vreg_off(e, i->dst);
    ld(e, vreg_off(e, i->addr), 8, false, "x8");
    if (i->is_float) {
      sb_printf(e->out, "  ldr %s, [x8]\n", i->size == 8 ? "d0" : "s0");
      st_(e, dof, i->size, true, "d0");
    } else {
      const char *op = i->size == 1 ? "ldrsb x8, [x8]"
                       : i->size == 2 ? "ldrsh x8, [x8]"
                       : i->size == 4 ? "ldrsw x8, [x8]"
                                      : "ldr x8, [x8]";
      sb_printf(e->out, "  %s\n", op);
      st_(e, dof, 8, false, "x8");
    }
    break;
  }
  case IR_STORE: {
    ld(e, vreg_off(e, i->addr), 8, false, "x8");
    ld(e, vreg_off(e, i->a), i->size, i->is_float, i->is_float ? "d0" : "x9");
    if (i->is_float)
      sb_printf(e->out, "  str d0, [x8]\n");
    else if (i->size == 1)
      sb_printf(e->out, "  strb w9, [x8]\n");
    else if (i->size == 2)
      sb_printf(e->out, "  strh w9, [x8]\n");
    else if (i->size == 4)
      sb_printf(e->out, "  str w9, [x8]\n");
    else
      sb_printf(e->out, "  str x9, [x8]\n");
    break;
  }
  case IR_ADDRC: {
    int64_t dof = vreg_off(e, i->dst);
    if (i->lit == -1)
      sb_printf(e->out, "  adrp x8, _%s@PAGE\n  add x8, x8, _%s@PAGEOFF\n", i->callee,
                i->callee);
    else {
      addr_into(e, slot_off(e, i->slot));
      sb_printf(e->out, "  mov x8, x12\n");
    }
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_ADDI: {
    int64_t dof = vreg_off(e, i->dst);
    ld(e, vreg_off(e, i->a), 8, false, "x8");
    sb_printf(e->out, "  add x8, x8, #%lld\n", (long long)i->imm);
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_CAST: {
    int64_t sof = vreg_off(e, i->a), dof = vreg_off(e, i->dst);
    switch (i->cast) {
    case CAST_TRUNC:
      ld(e, sof, ir_size_of(i->cast_from), false, "x8");
      st_(e, dof, ir_size_of(i->cast_to) < 4 ? ir_size_of(i->cast_to) : 4, false, "x8");
      break;
    case CAST_SEXT:
      ld(e, sof, ir_size_of(i->cast_from), false, "x8");
      st_(e, dof, 8, false, "x8");
      break;
    case CAST_ZEXT:
      ld(e, sof, ir_size_of(i->cast_from), false, "x8"); // ldrsb/ldrsw? zero: use ldr w
      sb_printf(e->out, "  uxtw x8, w8\n");
      st_(e, dof, 8, false, "x8");
      break;
    case CAST_I2F:
      ld(e, sof, 8, false, "x8");
      sb_printf(e->out, "  scvtf %s, x8\n", i->cast_to == IT_F32 ? "s0" : "d0");
      st_(e, dof, i->cast_to == IT_F32 ? 4 : 8, true, "d0");
      break;
    case CAST_F2I:
      ld(e, sof, i->size, true, "d0");
      sb_printf(e->out, "  fcvtzs x8, %s\n", i->cast_from == IT_F32 ? "s0" : "d0");
      st_(e, dof, 8, false, "x8");
      break;
    case CAST_F32_F64:
      ld(e, sof, 4, true, "s0");
      sb_printf(e->out, "  fcvt d0, s0\n");
      st_(e, dof, 8, true, "d0");
      break;
    case CAST_F64_F32:
      ld(e, sof, 8, true, "d0");
      sb_printf(e->out, "  fcvt s0, d0\n");
      st_(e, dof, 4, true, "s0");
      break;
    case CAST_REINTERP:
      // fmov gpr, fpr — exact bit move (f32_bits/f64_bits)
      if (i->cast_from == IT_F32) {
        ld(e, sof, 4, true, "s0");
        sb_printf(e->out, "  fmov w8, s0\n");
      } else {
        ld(e, sof, 8, true, "d0");
        sb_printf(e->out, "  fmov x8, d0\n");
      }
      st_(e, dof, i->cast_from == IT_F32 ? 4 : 8, false, "x8");
      break;
    case CAST_BITCOPY:
      ld(e, sof, 8, false, "x8");
      st_(e, dof, 8, false, "x8");
      break;
    }
    break;
  }
  case IR_COPYMEM: {
    ld(e, vreg_off(e, i->addr), 8, false, "x8");
    ld(e, vreg_off(e, i->a), 8, false, "x9");
    if (i->size < 0) {
      ld(e, vreg_off(e, i->b), 8, false, "x10"); // runtime byte count
      if (getenv("RHO_RC_GUARD")) {
        // a runtime copy longer than 1 MiB means the length came from a
        // corrupted slice header — trap here instead of copying for hours
        int gl2 = g_label64++;
        sb_printf(e->out, "  cmp x10, #0x100000\n");
        sb_printf(e->out, "  b.ls Lcpyok%d\n", gl2);
        sb_printf(e->out, "  brk #5\n");
        sb_printf(e->out, "Lcpyok%d:\n", gl2);
      }
    } else
      sb_printf(e->out, "  mov x10, #%lld\n", (long long)i->size);
    int lbl = g_label64++;
    // count==0 must not enter the loop: subs-first would wrap to -1 and
    // copy for 2^64 iterations (empty slice_string is the everyday case)
    sb_printf(e->out, "  cbz x10, Lcpyd%d\n", lbl);
    sb_printf(e->out, "Lcpy%d:\n  ldrb w11, [x9]\n  strb w11, [x8]\n"
                      "  add x8, x8, #1\n  add x9, x9, #1\n"
                      "  subs x10, x10, #1\n  b.ne Lcpy%d\n", lbl, lbl);
    sb_printf(e->out, "Lcpyd%d:\n", lbl);
    break;
  }
  case IR_ZERO: {
    ld(e, vreg_off(e, i->addr), 8, false, "x8");
    sb_printf(e->out, "  mov x10, #%lld\n  mov w11, #0\n", (long long)i->size);
    int lbl = g_label64++;
    sb_printf(e->out, "Lzro%d:\n  strb w11, [x8]\n  add x8, x8, #1\n"
                      "  subs x10, x10, #1\n  b.ne Lzro%d\n", lbl, lbl);
    break;
  }
  case IR_LITADDR: {
    int64_t dof = vreg_off(e, i->dst);
    IRLiteral *l = e->fn->literals.items[i->lit];
    sb_printf(e->out, "  adrp x8, L%s@PAGE\n  add x8, x8, L%s@PAGEOFF\n", l->label, l->label);
    st_(e, dof, 8, false, "x8");
    break;
  }
  case IR_CALL:
    emit_call64(e, i);
    break;
  default:
    break;
  }
}

static void emit_edge64(Emitter64 *e, IRBlock *succ) {
  for (size_t p = 0; p < succ->phis.n; p++) {
    IRPhi *phi = succ->phis.items[p];
    bool found = false;
    for (size_t k = 0; k < phi->preds.n; k++) {
      if (phi->preds.items[k] == e->cur) {
        found = true;
        IRVreg *val = phi->args.items[k];
        if (!val || !phi->dst)
          continue; // never-returning arm; control cannot reach the join here
        int64_t d = vreg_off(e, phi->dst), s = vreg_off(e, val);
        ld(e, s, 8, false, "x8");
        st_(e, d, 8, false, "x8");
        break;
      }
    }
    if (!found && getenv("RHO_PHI_DEBUG"))
      fprintf(stderr, "PHI EDGE MISSING: fn %s phi dst v%d pred block %d\n",
              e->fn->symbol, phi->dst ? phi->dst->id : -1, e->cur->id);
  }
}

static void emit_term64(Emitter64 *e, IRIns *t) {
  if (t->op == (IROp)OP_BR) {
    IRBlock *to = (IRBlock *)t->dst;
    emit_edge64(e, to);
    sb_printf(e->out, "  b L%d_%d\n", e->fn->uid, to->id);
  } else if (t->op == (IROp)OP_CBR) {
    IRBlock *tt = (IRBlock *)t->dst;
    IRBlock *ff = (IRBlock *)t->b;
    int64_t cof = vreg_off(e, t->a);
    int lbl = g_label64++;
    ld(e, cof, 1, false, "x8");
    sb_printf(e->out, "  cbz x8, Lcbf%d\n", lbl);
    // phi edge copies match preds against e->cur, which must stay the
    // branching block for both arms (mirrors emit_amd64)
    emit_edge64(e, tt);
    sb_printf(e->out, "  b L%d_%d\n", e->fn->uid, tt->id);
    sb_printf(e->out, "Lcbf%d:\n", lbl);
    emit_edge64(e, ff);
    sb_printf(e->out, "  b L%d_%d\n", e->fn->uid, ff->id);
  } else if (t->op == (IROp)OP_RET) {
    if (t->a) {
      bool f = ir_is_float(t->a->ty);
      ld(e, vreg_off(e, t->a), 8, f, f ? "d0" : "x0");
    }
    sb_printf(e->out, "  mov sp, x29\n  ldp x29, x30, [sp], #16\n  ret\n");
  }
}

static int g_uid64 = 0;

static void emit_fn64(Emitter64 *e, IRFn *fn) {
  e->fn = fn;
  e->cur = NULL;
  fn->uid = g_uid64++;
  layout_frame64(e);
  sb_printf(e->out, "\n  .globl _%s\n_%s:\n", fn->symbol, fn->symbol);
  sb_printf(e->out, "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n");
  if (e->frame) {
    // the sub sp immediate is 12 bits; the epilogue restores sp from x29,
    // so big frames just reserve in chunks
    int64_t left = e->frame;
    while (left > 0) {
      int64_t chunk = left > 4080 ? 4080 : left;
      sb_printf(e->out, "  sub sp, sp, #%lld\n", (long long)chunk);
      left -= chunk;
    }
  }

  const char *regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
  const char *fregs[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
  int ri = 0, fi = 0;
  if (fn->returns_aggregate) {
    ri++;
    addr_into(e, slot_off(e, fn->out_slot));
    sb_printf(e->out, "  str x0, [x12]\n");
  }
  for (size_t p = 0; p < fn->params.n; p++) {
    IRSlot *slot = fn->params.items[p];
    Type *pt = fn->param_types.items[p];
    bool isf = pt && (pt->kind == TY_F32 || pt->kind == TY_F64);
    if (isf && fi < 8) {
      addr_into(e, slot_off(e, slot));
      sb_printf(e->out, "  str %s, [x12]\n", fregs[fi++]);
    } else if (!isf && ri < 8) {
      addr_into(e, slot_off(e, slot));
      sb_printf(e->out, "  str %s, [x12]\n", regs[ri++]);
    }
  }

  for (size_t bi = 0; bi < fn->blocks.n; bi++) {
    IRBlock *b = fn->blocks.items[bi];
    e->cur = b;
    sb_printf(e->out, "L%d_%d:\n", fn->uid, b->id);
    for (size_t ii = 0; ii < b->ins.n; ii++)
      emit_ins64(e, b->ins.items[ii]);
    if (b->term)
      emit_term64(e, b->term);
    else
      sb_printf(e->out, "  mov sp, x29\n  ldp x29, x30, [sp], #16\n  ret\n");
  }
}

void emit_arm64(Target target, SB *out) {
  Emitter64 e = {0};
  e.tgt = target;
  e.out = out;
  sb_append_c(out, ".section __TEXT,__text,regular,pure_instructions\n");
  sb_append_c(out, ".build_version macos, 13, 0\n");

  for (size_t i = 0; i < g_ir_fns.n; i++)
    emit_fn64(&e, g_ir_fns.items[i]);

  // rc-headed literals must live in a non-merging section: ld64 folds and
  // reorders __cstring literals, which would break the buf/+24 pairing
  sb_append_c(out, ".section __TEXT,__rhostr,regular\n");
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    for (size_t j = 0; j < fn->literals.n; j++) {
      IRLiteral *l = fn->literals.items[j];
      sb_printf(out, "  .p2align 3\nL%s:\n  .quad 0x8000000000000000\n  .quad 0\n  .quad 0\nL%s_b:\n  .ascii \"",
                l->label, l->label);
      for (size_t k = 0; k < l->bytes.n; k++) {
        unsigned char ch = (unsigned char)(long)l->bytes.items[k];
        if (ch >= 0x20 && ch < 0x7F && ch != '"' && ch != '\\')
          sb_push(out, (char)ch);
        else
          sb_printf(out, "\\%03o", ch);
      }
      sb_append_c(out, "\"\n");
    }
  }
  for (size_t i = 0; i < g_fconsts64.n; i++) {
    FC64 *n = g_fconsts64.items[i];
    uint64_t bits;
    __builtin_memcpy(&bits, &n->v, 8);
    sb_printf(out, "  .p2align 3\nLf64_%d:\n  .quad %llu\n", n->lbl, (unsigned long long)bits);
  }

  for (size_t i = 0; i < g_ir_globals.n; i++) {
    IRGlobal *g = g_ir_globals.items[i];
    sb_printf(out, "\n  .globl _%s\n", g->symbol);
    sb_append_c(out, ".section __DATA,__data\n");
    sb_printf(out, "  .p2align %d\n_%s:\n",
              g->align >= 8 ? 3 : g->align >= 4 ? 2 : g->align >= 2 ? 1 : 0, g->symbol);
    size_t word = 0;
    if (g->relocs) {
      for (int w = 0; w < 3; w++) {
        if (g->relocs[w])
          sb_printf(out, "  .quad L%s\n", g->relocs[w]);
        else {
          uint64_t v = 0;
          for (int b = 0; b < 8; b++)
            v |= (unsigned char)(long)g->init.items[word * 8 + b] << (8 * b);
          sb_printf(out, "  .quad %llu\n", (unsigned long long)v);
        }
        word++;
      }
    } else if (g->init.n) {
      for (size_t b = 0; b < g->init.n; b++)
        sb_printf(out, "  .byte %d\n", (int)(unsigned char)(long)g->init.items[b]);
      if ((size_t)g->size > g->init.n)
        sb_printf(out, "  .zero %lld\n", (long long)(g->size - (int64_t)g->init.n));
    } else {
      sb_printf(out, "  .zero %lld\n", (long long)g->size);
    }
  }

  if (g_main_symbol) {
    sb_append_c(out, ".section __TEXT,__text,regular,pure_instructions\n");
    sb_printf(out, "\n  .globl _main\n_main:\n");
    sb_printf(out, "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n");
    sb_printf(out, "  bl _%s\n", g_main_symbol);
    sb_printf(out, "  ldp x29, x30, [sp], #16\n  ret\n");
  }
}
