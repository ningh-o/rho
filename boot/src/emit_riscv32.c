// emit_riscv32.c — esp32c3 backend (spec §11.2): RV32IM text assembly plus
// a two-pass assembler that produces the flat load image the in-tree
// simulator (rv32sim.c) runs. Spill-everything, mirroring emit_arm64:
//
//   fp = s0 (x8); slots at negative s0 offsets; t0 (x5) is the dedicated
//   address scratch; t1/t2 (x6/x7) hold the first value; a0-a7 are argument
//   and call-clobber registers, free as temps between instructions.
//
// Widths follow the wasm precedent: pointers, usize and isize are 32-bit
// (slot footprint stays 8 bytes, the low half carries the value); explicit
// i64/u64 occupy register pairs and route to the fixed runtime stubs the
// emitter appends (__div64, __shl64, ...). Soft-float lands in 0.2.1 —
// float ops trap with ebreak for now.

#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_label32 = 0;
static int g_uid32 = 0;

typedef struct Emitter32 {
  SB *out;
  IRFn *fn;
  Vec slot_off;
  int64_t frame;
  IRBlock *cur;
} Emitter32;

static int rv_is64(IRType t) {
  return t == IT_I64 || t == IT_U64;
}

static int rv_size_of(IRType t) {
  if (t == IT_PTR || t == IT_USIZE)
    return 4;
  return (int)ir_size_of(t);
}

static int64_t slot_off32(Emitter32 *e, IRSlot *s) {
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

static int64_t vreg_off32(Emitter32 *e, IRVreg *v) {
  size_t idx = e->fn->slots.n + (size_t)v->id;
  while (e->slot_off.n <= idx)
    vec_push(&e->slot_off, (void *)0);
  return (long)e->slot_off.items[idx];
}

static void layout_frame32(Emitter32 *e) {
  e->slot_off = (Vec){0};
  // the top 16 bytes of the frame are the save band (ra at s0-4, caller
  // s0 at s0-8 — see prologue/epilogue); slots and vregs live below it,
  // so the slot cursor starts at 16, not 0
  e->frame = 16;
  for (size_t i = 0; i < e->fn->slots.n; i++)
    slot_off32(e, e->fn->slots.items[i]);
  for (int v = 0; v < e->fn->next_vreg; v++) {
    e->frame += 8;
    vec_push(&e->slot_off, (void *)(long)(-e->frame));
  }
  e->frame = (e->frame + 15) / 16 * 16;
}

// t0 (x5) is the slot-address scratch; offsets beyond the 12-bit signed
// immediate go through a lui+addi absolute in t0 itself
static void addr_into32(Emitter32 *e, int64_t off) {
  (void)e;
  if (off >= -2048 && off <= 2047) {
    sb_printf(e->out, "  addi t0, s0, %lld\n", (long long)off);
    return;
  }
  uint32_t u = (uint32_t)off;
  uint32_t hi = (u + 0x800u) >> 12;
  uint32_t lo = u - (hi << 12);
  int32_t slo = (int32_t)lo;
  if (slo >= 2048)
    slo -= 4096;
  sb_printf(e->out, "  lui t0, %llu\n", (unsigned long long)hi);
  if (slo)
    sb_printf(e->out, "  addi t0, t0, %d\n", slo);
  sb_printf(e->out, "  add t0, s0, t0\n");
}

static const char *ld_op(int64_t size, bool sgn) {
  if (size == 1)
    return sgn ? "lb" : "lbu";
  if (size == 2)
    return sgn ? "lh" : "lhu";
  return "lw";
}

static void ld32(Emitter32 *e, int64_t off, int64_t size, bool sgn, const char *reg) {
  addr_into32(e, off);
  sb_printf(e->out, "  %s %s, 0(t0)\n", ld_op(size, sgn), reg);
}

// load reg from a slot WITHOUT disturbing t0 — the mirror of st_keep_t0,
// for ops that hold a pointer in t0 across the load (IR_STORE/COPYMEM
// fetch their data value after computing the store target into t0)
static void ld_keep_t0(Emitter32 *e, int64_t off, int64_t size, bool sgn,
                       const char *reg) {
  if (off >= -2048 && off <= 2047) {
    sb_printf(e->out, "  %s %s, %lld(s0)\n", ld_op(size, sgn), reg, (long long)off);
    return;
  }
  uint32_t u = (uint32_t)off;
  uint32_t hi = (u + 0x800u) >> 12;
  uint32_t lo = u - (hi << 12);
  int32_t slo = (int32_t)lo;
  if (slo >= 2048)
    slo -= 4096;
  sb_printf(e->out, "  lui t6, %llu\n", (unsigned long long)hi);
  if (slo)
    sb_printf(e->out, "  addi t6, t6, %d\n", slo);
  sb_printf(e->out, "  add t6, s0, t6\n");
  sb_printf(e->out, "  %s %s, 0(t6)\n", ld_op(size, sgn), reg);
}

static void st32(Emitter32 *e, int64_t off, int64_t size, const char *reg) {
  addr_into32(e, off);
  const char *op = size == 1 ? "sb" : size == 2 ? "sh" : "sw";
  sb_printf(e->out, "  %s %s, 0(t0)\n", op, reg);
}

// store reg into a slot WITHOUT disturbing t0 — for the ops that keep a
// computed value (an address, a compare bit) in t0 as data. s0-relative
// immediates cover most slots; beyond that the address is built in t6,
// which nothing holds across instructions except an indirect IR_CALL.
static void st_keep_t0(Emitter32 *e, int64_t off, int64_t size, const char *reg) {
  const char *op = size == 1 ? "sb" : size == 2 ? "sh" : "sw";
  if (off >= -2048 && off <= 2047) {
    sb_printf(e->out, "  %s %s, %lld(s0)\n", op, reg, (long long)off);
    return;
  }
  uint32_t u = (uint32_t)off;
  uint32_t hi = (u + 0x800u) >> 12;
  uint32_t lo = u - (hi << 12);
  int32_t slo = (int32_t)lo;
  if (slo >= 2048)
    slo -= 4096;
  sb_printf(e->out, "  lui t6, %llu\n", (unsigned long long)hi);
  if (slo)
    sb_printf(e->out, "  addi t6, t6, %d\n", slo);
  sb_printf(e->out, "  add t6, s0, t6\n");
  sb_printf(e->out, "  %s %s, 0(t6)\n", op, reg);
}

static void ld_pair(Emitter32 *e, int64_t off, const char *lo, const char *hi) {
  addr_into32(e, off);
  sb_printf(e->out, "  lw %s, 0(t0)\n", lo);
  sb_printf(e->out, "  lw %s, 4(t0)\n", hi);
}

static void st_pair(Emitter32 *e, int64_t off, const char *lo, const char *hi) {
  addr_into32(e, off);
  sb_printf(e->out, "  sw %s, 0(t0)\n", lo);
  sb_printf(e->out, "  sw %s, 4(t0)\n", hi);
}

static void li32(Emitter32 *e, const char *reg, uint32_t v) {
  if (v <= 2047) {
    sb_printf(e->out, "  addi %s, zero, %u\n", reg, v);
    return;
  }
  uint32_t hi = (v + 0x800u) >> 12;
  uint32_t lo = v - (hi << 12);
  int32_t slo = (int32_t)lo;
  if (slo >= 2048)
    slo -= 4096;
  sb_printf(e->out, "  lui %s, %llu\n", reg, (unsigned long long)hi);
  if (slo)
    sb_printf(e->out, "  addi %s, %s, %d\n", reg, reg, slo);
}

// canonicalize a 32-bit narrow result to its own width (spec: wraparound)
static void canon32(Emitter32 *e, const char *reg, IRType ty) {
  int64_t w = rv_size_of(ty);
  if (w >= 4)
    return;
  bool sgn = ty_is_signed_int(ty);
  int sh = (int)(32 - w * 8);
  sb_printf(e->out, "  slli %s, %s, %d\n", reg, reg, sh);
  sb_printf(e->out, "  %s %s, %s, %d\n", sgn ? "srai" : "srli", reg, reg, sh);
}

static void call_helper(Emitter32 *e, const char *name, int64_t alo, int64_t ahi,
                        int64_t blo, int64_t bhi, int64_t dlo, int64_t dhi) {
  ld32(e, alo, 4, false, "a0");
  ld32(e, ahi, 4, false, "a1");
  ld32(e, blo, 4, false, "a2");
  ld32(e, bhi, 4, false, "a3");
  sb_printf(e->out, "  auipc ra, %%hi(%s)\n", name);
  sb_printf(e->out, "  jalr ra, ra, %%lo(%s)\n", name);
  st_pair(e, dlo, "a0", "a1");
  (void)dhi;
}

static void emit_ins32(Emitter32 *e, IRIns *i) {
  switch (i->op) {
  case IR_CONST: {
    int64_t dof = vreg_off32(e, i->dst);
    uint64_t v = (uint64_t)i->imm;
    if (rv_is64(i->dst->ty)) {
      li32(e, "t1", (uint32_t)v);
      li32(e, "t2", (uint32_t)(v >> 32));
      st_pair(e, dof, "t1", "t2");
    } else {
      li32(e, "t1", (uint32_t)v);
      st32(e, dof, 4, "t1");
    }
    break;
  }
  case IR_FCONST:
    // soft-float lands in 0.2.1
    sb_printf(e->out, "  ebreak #900\n");
    break;
  case IR_ADD:
  case IR_SUB:
  case IR_AND:
  case IR_OR:
  case IR_XOR: {
    int64_t dof = vreg_off32(e, i->dst);
    if (rv_is64(i->dst->ty)) {
      int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b);
      ld_pair(e, ao, "t1", "t2");
      ld_pair(e, bo, "t3", "t4");
      const char *op = i->op == IR_ADD ? "add" : i->op == IR_SUB ? "sub"
                                    : i->op == IR_AND  ? "and"
                                    : i->op == IR_OR   ? "or"
                                                       : "xor";
      if (i->op == IR_ADD) {
        sb_printf(e->out, "  add t1, t1, t3\n");
        sb_printf(e->out, "  sltu t5, t1, t3\n");
        sb_printf(e->out, "  add t2, t2, t4\n");
        sb_printf(e->out, "  add t2, t2, t5\n");
      } else if (i->op == IR_SUB) {
        sb_printf(e->out, "  sltu t5, t1, t3\n");
        sb_printf(e->out, "  sub t1, t1, t3\n");
        sb_printf(e->out, "  sub t2, t2, t4\n");
        sb_printf(e->out, "  sub t2, t2, t5\n");
      } else {
        sb_printf(e->out, "  %s t1, t1, t3\n", op);
        sb_printf(e->out, "  %s t2, t2, t4\n", op);
      }
      st_pair(e, dof, "t1", "t2");
      break;
    }
    const char *op = i->op == IR_ADD ? "add" : i->op == IR_SUB ? "sub"
                                  : i->op == IR_AND  ? "and"
                                  : i->op == IR_OR   ? "or"
                                                     : "xor";
    ld32(e, vreg_off32(e, i->a), rv_size_of(i->a->ty), ty_is_signed_int(i->a->ty), "t1");
    ld32(e, vreg_off32(e, i->b), rv_size_of(i->b->ty), ty_is_signed_int(i->b->ty), "t2");
    sb_printf(e->out, "  %s t1, t1, t2\n", op);
    canon32(e, "t1", i->a->ty);
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_MUL: {
    int64_t dof = vreg_off32(e, i->dst);
    if (rv_is64(i->dst->ty)) {
      // lower 64 bits of the product: lo = alo*blo,
      // hi = alo*bhi + ahi*blo + hi(alo*blo) — mod 2^64 correct signed too
      int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b);
      ld_pair(e, ao, "t1", "t2");
      ld_pair(e, bo, "t3", "t4");
      sb_printf(e->out, "  mul t5, t1, t3\n");
      sb_printf(e->out, "  mulhu t6, t1, t3\n");
      sb_printf(e->out, "  mul t2, t1, t4\n");
      sb_printf(e->out, "  mul t3, t2, t3\n");
      sb_printf(e->out, "  add t2, t2, t3\n");
      sb_printf(e->out, "  add t2, t2, t6\n");
      sb_printf(e->out, "  mv t1, t5\n");
      st_pair(e, dof, "t1", "t2");
      break;
    }
    ld32(e, vreg_off32(e, i->a), rv_size_of(i->a->ty), true, "t1");
    ld32(e, vreg_off32(e, i->b), rv_size_of(i->b->ty), true, "t2");
    sb_printf(e->out, "  mul t1, t1, t2\n");
    canon32(e, "t1", i->a->ty);
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_DIV:
  case IR_MOD: {
    int64_t dof = vreg_off32(e, i->dst);
    bool is64 = rv_is64(i->dst->ty);
    bool sgn = ty_is_signed_int(i->a->ty);
    // RISC-V division defines x/0 = -1 and overflow as wraparound, but the
    // language panics on /0 — guard before the hardware op or the stub
    const char *psym = prelude_symbol("__panic_div");
    int lbl = g_label32++;
    if (is64) {
      int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b);
      ld32(e, bo, 4, false, "t2");
      ld32(e, bo + 4, 4, false, "t3");
      sb_printf(e->out, "  or t2, t2, t3\n  bne t2, zero, Ldz%d\n", lbl);
      sb_printf(e->out, "  auipc ra, %%hi(%s)\n", psym);
      sb_printf(e->out, "  jalr ra, ra, %%lo(%s)\n", psym);
      sb_printf(e->out, "Ldz%d:\n", lbl);
      if (sgn)
        call_helper(e, i->op == IR_DIV ? "__div64" : "__rem64", ao, ao + 4, bo, bo + 4, dof,
                    dof + 4);
      else
        call_helper(e, i->op == IR_DIV ? "__divu64" : "__remu64", ao, ao + 4, bo, bo + 4, dof,
                    dof + 4);
      break;
    }
    ld32(e, vreg_off32(e, i->a), 4, sgn, "t1");
    ld32(e, vreg_off32(e, i->b), 4, sgn, "t2");
    sb_printf(e->out, "  bne t2, zero, Ldz%d\n", lbl);
    sb_printf(e->out, "  auipc ra, %%hi(%s)\n", psym);
    sb_printf(e->out, "  jalr ra, ra, %%lo(%s)\n", psym);
    sb_printf(e->out, "Ldz%d:\n", lbl);
    const char *op = sgn ? (i->op == IR_DIV ? "div" : "rem") : (i->op == IR_DIV ? "divu" : "remu");
    sb_printf(e->out, "  %s t1, t1, t2\n", op);
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_SHL:
  case IR_SHR: {
    int64_t dof = vreg_off32(e, i->dst);
    if (rv_is64(i->dst->ty)) {
      int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b);
      const char *h = i->op == IR_SHL ? "__shl64" : i->signed_ops ? "__sar64" : "__srl64";
      call_helper(e, h, ao, ao + 4, bo, bo + 4, dof, dof + 4);
      break;
    }
    ld32(e, vreg_off32(e, i->a), rv_size_of(i->a->ty), ty_is_signed_int(i->a->ty), "t1");
    ld32(e, vreg_off32(e, i->b), rv_size_of(i->b->ty), false, "t2");
    const char *op = i->op == IR_SHL ? "sll" : i->signed_ops ? "sra" : "srl";
    sb_printf(e->out, "  %s t1, t1, t2\n", op);
    if (i->op == IR_SHL)
      canon32(e, "t1", i->a->ty);
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_CMP: {
    if (rv_is64(i->a->ty)) {
      // pair compare, branchless: less = hi_less | (hi_eq & lo_less)
      int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b), dof = vreg_off32(e, i->dst);
      bool sgn = ty_is_signed_int(i->a->ty);
      const char *cmp = sgn ? "slt" : "sltu";
      ld_pair(e, ao, "t1", "t2");
      ld_pair(e, bo, "t3", "t4");
      sb_printf(e->out, "  %s t5, t2, t4\n", cmp);
      sb_printf(e->out, "  %s t6, t4, t2\n", cmp);
      sb_printf(e->out, "  sltu t0, t1, t3\n");
      sb_printf(e->out, "  sub t6, zero, t6\n");
      sb_printf(e->out, "  not t6, t6\n");
      sb_printf(e->out, "  and t0, t0, t6\n");
      sb_printf(e->out, "  or t0, t5, t0\n");
      sb_printf(e->out, "  xor t1, t1, t3\n");
      sb_printf(e->out, "  xor t2, t2, t4\n");
      sb_printf(e->out, "  or t1, t1, t2\n");
      sb_printf(e->out, "  seqz t2, t1\n");
      // t0 = less, t2 = equal — pick per condition code
      if (i->cc == CC_LT)
        sb_printf(e->out, "  mv t0, t0\n");
      else if (i->cc == CC_LE)
        sb_printf(e->out, "  or t0, t0, t2\n");
      else if (i->cc == CC_GT)
        sb_printf(e->out, "  or t0, t0, t2\n  xori t0, t0, 1\n");
      else if (i->cc == CC_GE)
        sb_printf(e->out, "  xori t0, t0, 1\n");
      else if (i->cc == CC_EQ)
        sb_printf(e->out, "  mv t0, t2\n");
      else
        sb_printf(e->out, "  xori t2, t2, 1\n  mv t0, t2\n");
      st_keep_t0(e, dof, 1, "t0");
      break;
    }
    // 32-bit compare
    int64_t ao = vreg_off32(e, i->a), bo = vreg_off32(e, i->b), dof = vreg_off32(e, i->dst);
    bool sgn = ty_is_signed_int(i->a->ty);
    const char *lt = sgn ? "slt" : "sltu";
    ld32(e, ao, rv_size_of(i->a->ty), sgn, "t1");
    ld32(e, bo, rv_size_of(i->b->ty), sgn, "t2");
    if (i->cc == CC_EQ) {
      sb_printf(e->out, "  xor t1, t1, t2\n  seqz t1, t1\n");
    } else if (i->cc == CC_NE) {
      sb_printf(e->out, "  xor t1, t1, t2\n  snez t1, t1\n");
    } else if (i->cc == CC_LT) {
      sb_printf(e->out, "  %s t1, t1, t2\n", lt);
    } else if (i->cc == CC_GT) {
      sb_printf(e->out, "  %s t1, t2, t1\n", lt);
    } else if (i->cc == CC_LE) {
      sb_printf(e->out, "  %s t1, t2, t1\n  xori t1, t1, 1\n", lt);
    } else {
      sb_printf(e->out, "  %s t1, t1, t2\n  xori t1, t1, 1\n", lt);
    }
    st32(e, dof, 1, "t1");
    break;
  }
  case IR_LOAD: {
    int64_t dof = vreg_off32(e, i->dst);
    ld32(e, vreg_off32(e, i->addr), 4, false, "t0");
    if (i->size == 8 && rv_is64(i->dst->ty)) {
      sb_printf(e->out, "  lw t1, 0(t0)\n");
      sb_printf(e->out, "  lw t2, 4(t0)\n");
      st_pair(e, dof, "t1", "t2");
      break;
    }
    bool sgn = ty_is_signed_int(i->dst->ty);
    sb_printf(e->out, "  %s t1, 0(t0)\n", ld_op(i->size, sgn));
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_STORE: {
    ld32(e, vreg_off32(e, i->addr), 4, false, "t0");
    if (i->size == 8 && i->a->ty && rv_is64(i->a->ty)) {
      ld_keep_t0(e, vreg_off32(e, i->a), 4, false, "t1");
      ld_keep_t0(e, vreg_off32(e, i->a) + 4, 4, false, "t2");
      sb_printf(e->out, "  sw t1, 0(t0)\n");
      sb_printf(e->out, "  sw t2, 4(t0)\n");
      break;
    }
    ld_keep_t0(e, vreg_off32(e, i->a), i->size, false, "t1");
    const char *op = i->size == 1 ? "sb" : i->size == 2 ? "sh" : "sw";
    sb_printf(e->out, "  %s t1, 0(t0)\n", op);
    break;
  }
  case IR_ADDRC: {
    int64_t dof = vreg_off32(e, i->dst);
    if (i->lit == -1) {
      sb_printf(e->out, "  auipc t0, %%hi(%s)\n", i->callee);
      sb_printf(e->out, "  addi t0, t0, %%lo(%s)\n", i->callee);
    } else {
      addr_into32(e, slot_off32(e, i->slot));
    }
    st_keep_t0(e, dof, 4, "t0");
    break;
  }
  case IR_ADDI: {
    int64_t dof = vreg_off32(e, i->dst);
    ld32(e, vreg_off32(e, i->a), 4, false, "t1");
    if (i->imm >= -2048 && i->imm <= 2047) {
      sb_printf(e->out, "  addi t1, t1, %lld\n", (long long)i->imm);
    } else {
      li32(e, "t2", (uint32_t)i->imm);
      sb_printf(e->out, "  add t1, t1, t2\n");
    }
    st32(e, dof, 4, "t1");
    break;
  }
  case IR_CAST: {
    int64_t sof = vreg_off32(e, i->a), dof = vreg_off32(e, i->dst);
    switch (i->cast) {
    case CAST_TRUNC:
      // i64 -> i32: the low half IS the truncated value
      ld32(e, sof, rv_is64(i->a->ty) ? 4 : rv_size_of(i->cast_from), false, "t1");
      st32(e, dof, 4, "t1");
      break;
    case CAST_SEXT:
      if (rv_is64(i->dst->ty) && !rv_is64(i->a->ty)) {
        ld32(e, sof, 4, true, "t1");
        sb_printf(e->out, "  srai t2, t1, 31\n");
        st_pair(e, dof, "t1", "t2");
      } else if (rv_is64(i->dst->ty)) {
        ld_pair(e, sof, "t1", "t2");
        st_pair(e, dof, "t1", "t2");
      } else {
        ld32(e, sof, rv_size_of(i->cast_from), true, "t1");
        st32(e, dof, 4, "t1");
      }
      break;
    case CAST_ZEXT:
      if (rv_is64(i->dst->ty) && !rv_is64(i->a->ty)) {
        ld32(e, sof, 4, false, "t1");
        li32(e, "t2", 0);
        st_pair(e, dof, "t1", "t2");
      } else {
        ld32(e, sof, rv_size_of(i->cast_from), false, "t1");
        st32(e, dof, 4, "t1");
      }
      break;
    case CAST_BITCOPY:
    case CAST_REINTERP:
      // same-size bit moves (usize<->ptr, f32/f64 bit pattern extraction) —
      // no FP arithmetic involved, so these work without soft-float
      if (rv_is64(i->dst->ty)) {
        ld_pair(e, sof, "t1", "t2");
        st_pair(e, dof, "t1", "t2");
      } else {
        ld32(e, sof, rv_size_of(i->cast_from), false, "t1");
        st32(e, dof, 4, "t1");
      }
      break;
    default:
      // I2F / F2I / F32_F64 / F64_F32: soft-float lands in 0.2.1
      sb_printf(e->out, "  ebreak #901\n");
      break;
    }
    break;
  }
  case IR_COPYMEM: {
    ld32(e, vreg_off32(e, i->addr), 4, false, "t0");
    ld_keep_t0(e, vreg_off32(e, i->a), 4, false, "t1");
    if (i->size < 0) {
      ld_keep_t0(e, vreg_off32(e, i->b), 4, false, "t2");
    } else {
      li32(e, "t2", (uint32_t)i->size);
    }
    int lbl = g_label32++;
    // count==0 must not enter the loop (empty slice_string is everyday)
    sb_printf(e->out, "  beq t2, zero, Lcpyd%d\n", lbl);
    sb_printf(e->out, "Lcpy%d:\n", lbl);
    sb_printf(e->out, "  lb t3, 0(t1)\n");
    sb_printf(e->out, "  sb t3, 0(t0)\n");
    sb_printf(e->out, "  addi t0, t0, 1\n");
    sb_printf(e->out, "  addi t1, t1, 1\n");
    sb_printf(e->out, "  addi t2, t2, -1\n");
    sb_printf(e->out, "  bne t2, zero, Lcpy%d\n", lbl);
    sb_printf(e->out, "Lcpyd%d:\n", lbl);
    break;
  }
  case IR_ZERO: {
    ld32(e, vreg_off32(e, i->addr), 4, false, "t0");
    li32(e, "t2", (uint32_t)i->size);
    int lbl = g_label32++;
    sb_printf(e->out, "Lzro%d:\n", lbl);
    sb_printf(e->out, "  sb zero, 0(t0)\n");
    sb_printf(e->out, "  addi t0, t0, 1\n");
    sb_printf(e->out, "  addi t2, t2, -1\n");
    sb_printf(e->out, "  bne t2, zero, Lzro%d\n", lbl);
    break;
  }
  case IR_LITADDR: {
    int64_t dof = vreg_off32(e, i->dst);
    IRLiteral *l = e->fn->literals.items[i->lit];
    sb_printf(e->out, "  auipc t0, %%hi(L%s)\n", l->label);
    sb_printf(e->out, "  addi t0, t0, %%lo(L%s)\n", l->label);
    st_keep_t0(e, dof, 4, "t0");
    break;
  }
  case IR_CALL: {
    if (i->callee_vreg) {
      // indirect target in t6, outside the argument registers
      ld32(e, vreg_off32(e, i->callee_vreg), 4, false, "t6");
    }
    static const char *aregs[8] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    int ri = 0;
    for (size_t k = 0; k < i->args.n && ri < 8; k++) {
      IRArg *a = (IRArg *)i->args.items[k];
      int64_t off = vreg_off32(e, a->vreg);
      bool is64arg = a->ty && (a->ty->kind == TY_I64 || a->ty->kind == TY_U64);
      if (is64arg && ri + 2 <= 8) {
        ld32(e, off, 4, false, aregs[ri]);
        ld32(e, off + 4, 4, false, aregs[ri + 1]);
        ri += 2;
      } else {
        ld32(e, off, 4, false, aregs[ri]);
        ri += 1;
      }
    }
    if (i->callee) {
      sb_printf(e->out, "  auipc ra, %%hi(%s)\n", i->callee);
      sb_printf(e->out, "  jalr ra, ra, %%lo(%s)\n", i->callee);
    } else {
      sb_printf(e->out, "  jalr ra, 0(t6)\n");
    }
    if (i->dst) {
      if (rv_is64(i->dst->ty)) {
        st_pair(e, vreg_off32(e, i->dst), "a0", "a1");
      } else {
        st32(e, vreg_off32(e, i->dst), 4, "a0");
      }
    }
    break;
  }
  default:
    break;
  }
}

static void emit_edge32(Emitter32 *e, IRBlock *succ) {
  for (size_t p = 0; p < succ->phis.n; p++) {
    IRPhi *phi = succ->phis.items[p];
    for (size_t k = 0; k < phi->preds.n; k++) {
      if (phi->preds.items[k] == e->cur) {
        IRVreg *val = phi->args.items[k];
        if (!val || !phi->dst)
          continue;
        int64_t d = vreg_off32(e, phi->dst), s = vreg_off32(e, val);
        ld32(e, s, 4, false, "t1");
        st32(e, d, 4, "t1");
        break;
      }
    }
  }
}

static void emit_epilogue32(Emitter32 *e) {
  // mirror of the prologue: ra at s0-4, caller s0 at s0-8 (the reserved
  // band at the top of the frame), then pop the frame and return
  ld32(e, -4, 4, false, "ra");
  ld32(e, -8, 4, false, "s0");
  li32(e, "t0", (uint32_t)e->frame);
  sb_printf(e->out, "  add sp, sp, t0\n");
  sb_printf(e->out, "  jalr zero, 0(ra)\n");
}

static void emit_term32(Emitter32 *e, IRIns *t) {
  if (t->op == (IROp)OP_BR) {
    IRBlock *to = (IRBlock *)t->dst;
    emit_edge32(e, to);
    sb_printf(e->out, "  jal zero, L%d_%d\n", e->fn->uid, to->id);
  } else if (t->op == (IROp)OP_CBR) {
    IRBlock *tt = (IRBlock *)t->dst;
    IRBlock *ff = (IRBlock *)t->b;
    int64_t cof = vreg_off32(e, t->a);
    int lbl = g_label32++;
    ld32(e, cof, 1, false, "t1");
    sb_printf(e->out, "  beq t1, zero, Lcbf%d\n", lbl);
    emit_edge32(e, tt);
    sb_printf(e->out, "  jal zero, L%d_%d\n", e->fn->uid, tt->id);
    sb_printf(e->out, "Lcbf%d:\n", lbl);
    emit_edge32(e, ff);
    sb_printf(e->out, "  jal zero, L%d_%d\n", e->fn->uid, ff->id);
  } else if (t->op == (IROp)OP_RET) {
    if (t->a) {
      if (rv_is64(t->a->ty)) {
        ld32(e, vreg_off32(e, t->a), 4, false, "a0");
        ld32(e, vreg_off32(e, t->a) + 4, 4, false, "a1");
      } else {
        ld32(e, vreg_off32(e, t->a), 4, false, "a0");
      }
    }
    emit_epilogue32(e);
  }
}

static void emit_fn32(Emitter32 *e, IRFn *fn) {
  e->fn = fn;
  e->cur = NULL;
  layout_frame32(e);
  fn->uid = g_uid32++;
  sb_printf(e->out, "\n  .globl %s\n%s:\n", fn->symbol, fn->symbol);
  // frame reservation through a temp: FRAME may exceed the 12-bit immediate.
  // caller s0 + ra go into the frame's RESERVED 16-byte band at the very top
  // (s0-4 = ra, s0-8 = caller s0 — the same addresses the epilogue's s0-
  // relative ld32 uses); t1 walks down from the future s0 while s0 still
  // holds the caller's value, so the saves cannot use s0-relative addressing
  li32(e, "t0", (uint32_t)e->frame);
  sb_printf(e->out, "  sub sp, sp, t0\n");
  sb_printf(e->out, "  add t1, sp, t0\n");
  sb_printf(e->out, "  addi t1, t1, -4\n");
  sb_printf(e->out, "  sw ra, 0(t1)\n");
  sb_printf(e->out, "  addi t1, t1, -4\n");
  sb_printf(e->out, "  sw s0, 0(t1)\n");
  sb_printf(e->out, "  add s0, sp, t0\n");

  int ri = 0;
  if (fn->returns_aggregate) {
    ri++;
    addr_into32(e, slot_off32(e, fn->out_slot));
    sb_printf(e->out, "  sw a0, 0(t0)\n");
  }
  for (size_t p = 0; p < fn->params.n && ri < 8; p++) {
    IRSlot *slot = fn->params.items[p];
    Type *pt = fn->param_types.items[p];
    if (pt && (pt->kind == TY_I64 || pt->kind == TY_U64)) {
      if (ri + 2 > 8)
        break;
      addr_into32(e, slot_off32(e, slot));
      sb_printf(e->out, "  sw a%d, 0(t0)\n", ri);
      sb_printf(e->out, "  sw a%d, 4(t0)\n", ri + 1);
      ri += 2;
    } else {
      addr_into32(e, slot_off32(e, slot));
      sb_printf(e->out, "  sw a%d, 0(t0)\n", ri);
      ri += 1;
    }
  }

  for (size_t bi = 0; bi < fn->blocks.n; bi++) {
    IRBlock *b = fn->blocks.items[bi];
    e->cur = b;
    sb_printf(e->out, "L%d_%d:\n", fn->uid, b->id);
    for (size_t ii = 0; ii < b->ins.n; ii++)
      emit_ins32(e, b->ins.items[ii]);
    if (b->term)
      emit_term32(e, b->term);
    else
      emit_epilogue32(e);
  }
  if (fn->blocks.n == 0)
    emit_epilogue32(e); // empty-bodied fn: fall off the end into a return
}

// ------------------------------------------------- runtime stubs ----

// __udivmod64: unsigned core; q -> (a0,a1), r -> (a2,a3). The 64 restoring
// iterations are unrolled: no loop bookkeeping to get wrong, and the
// emitter generates the repetition.
static void emit_rt_stubs(SB *out) {
  sb_printf(out, "\n  .globl __udivmod64\n__udivmod64:\n");
  sb_printf(out, "  mv t0, a0\n  mv t1, a1\n  addi t2, zero, 0\n  addi t3, zero, 0\n");
  for (int it = 0; it < 64; it++) {
    sb_printf(out, "  srli t4, t1, 31\n");
    sb_printf(out, "  slli t1, t1, 1\n");
    sb_printf(out, "  srli t5, t0, 31\n");
    sb_printf(out, "  or t1, t1, t5\n");
    sb_printf(out, "  slli t0, t0, 1\n");
    sb_printf(out, "  slli t3, t3, 1\n");
    sb_printf(out, "  srli t5, t2, 31\n");
    sb_printf(out, "  or t3, t3, t5\n");
    sb_printf(out, "  slli t2, t2, 1\n");
    sb_printf(out, "  or t2, t2, t4\n");
    // if R >= B: R -= B; Q |= 1   (unsigned 64-bit pair compare)
    // lt = (R_hi < D_hi) | ((R_hi == D_hi) & (R_lo < D_lo))
    sb_printf(out, "  sltu t5, t3, a3\n");       // lt_hi
    sb_printf(out, "  sltu t6, a3, t3\n");       // gt_hi
    sb_printf(out, "  or t6, t5, t6\n");         // ne_hi
    sb_printf(out, "  seqz t6, t6\n");           // eq_hi
    sb_printf(out, "  sltu t4, t2, a2\n");       // lo_lt
    sb_printf(out, "  and t4, t4, t6\n");        // lo_lt & eq_hi
    sb_printf(out, "  or t4, t5, t4\n");         // lt
    sb_printf(out, "  bne t4, zero, Ldvs%d\n", it);
    // subtract D from R with borrow taken before the low subtract
    sb_printf(out, "  sltu t5, t2, a2\n");       // borrow = R_lo < D_lo
    sb_printf(out, "  sub t2, t2, a2\n");
    sb_printf(out, "  sub t3, t3, a3\n");
    sb_printf(out, "  sub t3, t3, t5\n");
    sb_printf(out, "  ori t0, t0, 1\n");
    sb_printf(out, "Ldvs%d:\n", it);
  }
  sb_printf(out, "  mv a0, t0\n  mv a1, t1\n");
  sb_printf(out, "  mv a2, t2\n  mv a3, t3\n");
  sb_printf(out, "  jalr zero, 0(ra)\n");

  // the core call goes through ra, so the caller's return address parks
  // in s11 (and __div64/__rem64 park their sign flag in s10); s10/s11 are
  // callee-saved registers this emitter never allocates
  sb_printf(out, "\n  .globl __divu64\n__divu64:\n");
  sb_printf(out, "  mv s11, ra\n");
  sb_printf(out, "  auipc ra, %%hi(__udivmod64)\n");
  sb_printf(out, "  jalr ra, ra, %%lo(__udivmod64)\n");
  sb_printf(out, "  mv ra, s11\n");
  sb_printf(out, "  jalr zero, 0(ra)\n");
  sb_printf(out, "\n  .globl __remu64\n__remu64:\n");
  sb_printf(out, "  mv s11, ra\n");
  sb_printf(out, "  auipc ra, %%hi(__udivmod64)\n");
  sb_printf(out, "  jalr ra, ra, %%lo(__udivmod64)\n");
  sb_printf(out, "  mv a0, a2\n  mv a1, a3\n");
  sb_printf(out, "  mv ra, s11\n");
  sb_printf(out, "  jalr zero, 0(ra)\n");

  // signed division: magnitudes, core, sign fixups.
  // negate pair X=(x0,x1): sub x0, zero, x0; borrow = zero < old_x0;
  //                        x1 = ~x1 + borrow
  sb_printf(out, "\n  .globl __div64\n__div64:\n");
  sb_printf(out, "  xor t0, a1, a3\n");
  sb_printf(out, "  slti t0, t0, 0\n");
  sb_printf(out, "  mv s10, t0\n");
  sb_printf(out, "  slti t1, a1, 0\n");
  sb_printf(out, "  beq t1, zero, Ldivnap\n");
  sb_printf(out, "  sltu t2, zero, a0\n");
  sb_printf(out, "  sub a0, zero, a0\n");
  sb_printf(out, "  not a1, a1\n");
  sb_printf(out, "  add a1, a1, t2\n");
  sb_printf(out, "Ldivnap:\n");
  sb_printf(out, "  slti t1, a3, 0\n");
  sb_printf(out, "  beq t1, zero, Ldivnbp\n");
  sb_printf(out, "  sltu t2, zero, a2\n");
  sb_printf(out, "  sub a2, zero, a2\n");
  sb_printf(out, "  not a3, a3\n");
  sb_printf(out, "  add a3, a3, t2\n");
  sb_printf(out, "Ldivnbp:\n");
  sb_printf(out, "  mv s11, ra\n");
  sb_printf(out, "  auipc ra, %%hi(__udivmod64)\n");
  sb_printf(out, "  jalr ra, ra, %%lo(__udivmod64)\n");
  sb_printf(out, "  beq s10, zero, Ldivdone\n");
  sb_printf(out, "  sltu t2, zero, a0\n");
  sb_printf(out, "  sub a0, zero, a0\n");
  sb_printf(out, "  not a1, a1\n");
  sb_printf(out, "  add a1, a1, t2\n");
  sb_printf(out, "Ldivdone:\n");
  sb_printf(out, "  mv ra, s11\n");
  sb_printf(out, "  jalr zero, 0(ra)\n");

  sb_printf(out, "\n  .globl __rem64\n__rem64:\n");
  sb_printf(out, "  slti t0, a1, 0\n");
  sb_printf(out, "  slti t1, a3, 0\n");
  sb_printf(out, "  beq t1, zero, Lremnb\n");
  sb_printf(out, "  sltu t2, zero, a2\n");
  sb_printf(out, "  sub a2, zero, a2\n");
  sb_printf(out, "  not a3, a3\n");
  sb_printf(out, "  add a3, a3, t2\n");
  sb_printf(out, "Lremnb:\n");
  sb_printf(out, "  beq t0, zero, Lrempa\n");
  sb_printf(out, "  sltu t2, zero, a0\n");
  sb_printf(out, "  sub a0, zero, a0\n");
  sb_printf(out, "  not a1, a1\n");
  sb_printf(out, "  add a1, a1, t2\n");
  sb_printf(out, "Lrempa:\n");
  sb_printf(out, "  mv s10, t0\n");
  sb_printf(out, "  mv s11, ra\n");
  sb_printf(out, "  auipc ra, %%hi(__udivmod64)\n");
  sb_printf(out, "  jalr ra, ra, %%lo(__udivmod64)\n");
  sb_printf(out, "  mv a0, a2\n  mv a1, a3\n");
  sb_printf(out, "  beq s10, zero, Lremdone\n");
  sb_printf(out, "  sltu t2, zero, a0\n");
  sb_printf(out, "  sub a0, zero, a0\n");
  sb_printf(out, "  not a1, a1\n");
  sb_printf(out, "  add a1, a1, t2\n");
  sb_printf(out, "Lremdone:\n");
  sb_printf(out, "  mv ra, s11\n");
  sb_printf(out, "  jalr zero, 0(ra)\n");

  sb_printf(out, "\n  .globl __shl64\n__shl64:\n");
  sb_printf(out, "  andi a2, a2, 63\n");
  for (int it = 0; it < 63; it++) {
    sb_printf(out, "  beq a2, zero, Lshl63\n");
    sb_printf(out, "  srli t4, a1, 31\n");
    sb_printf(out, "  slli a1, a1, 1\n");
    sb_printf(out, "  srli t5, a0, 31\n");
    sb_printf(out, "  or a1, a1, t5\n");
    sb_printf(out, "  slli a0, a0, 1\n");
    sb_printf(out, "  addi a2, a2, -1\n");
  }
  sb_printf(out, "Lshl63:\n  jalr zero, 0(ra)\n");

  sb_printf(out, "\n  .globl __srl64\n__srl64:\n");
  sb_printf(out, "  andi a2, a2, 63\n");
  for (int it = 0; it < 63; it++) {
    sb_printf(out, "  beq a2, zero, Lsrl63\n");
    sb_printf(out, "  srli t4, a0, 1\n");
    sb_printf(out, "  andi t4, t4, 0x40000000\n");
    sb_printf(out, "  srli a0, a0, 1\n");
    sb_printf(out, "  slli t5, a1, 31\n");
    sb_printf(out, "  or a0, a0, t5\n");
    sb_printf(out, "  srli a1, a1, 1\n");
    sb_printf(out, "  or a1, a1, t4\n");
    sb_printf(out, "  addi a2, a2, -1\n");
  }
  sb_printf(out, "Lsrl63:\n  jalr zero, 0(ra)\n");

  sb_printf(out, "\n  .globl __sar64\n__sar64:\n");
  sb_printf(out, "  andi a2, a2, 63\n");
  for (int it = 0; it < 63; it++) {
    sb_printf(out, "  beq a2, zero, Lsar63\n");
    sb_printf(out, "  srli t4, a0, 1\n");
    sb_printf(out, "  andi t4, t4, 0x40000000\n");
    sb_printf(out, "  srli a0, a0, 1\n");
    sb_printf(out, "  slli t5, a1, 31\n");
    sb_printf(out, "  or a0, a0, t5\n");
    sb_printf(out, "  srai a1, a1, 1\n");
    sb_printf(out, "  or a1, a1, t4\n");
    sb_printf(out, "  addi a2, a2, -1\n");
  }
  sb_printf(out, "Lsar63:\n  jalr zero, 0(ra)\n");
}

// prelude statics arrive mangled (rho__prelude___heap_base); the entry
// stub must reference the same mangled symbols
static const char *rv32_heap_sym(const char *suffix) {
  for (size_t i = 0; i < g_ir_globals.n; i++) {
    IRGlobal *g = g_ir_globals.items[i];
    size_t n = strlen(g->symbol), m = strlen(suffix);
    if (n >= m && strcmp(g->symbol + n - m, suffix) == 0)
      return g->symbol;
  }
  return suffix;
}

void emit_riscv32(SB *out) {
  Emitter32 e = {0};
  e.out = out;
  const char *heap_base_sym = rv32_heap_sym("__heap_base");
  const char *heap_top_sym = rv32_heap_sym("__heap_top");

  // entry stub: park the machine-provided heap bounds, call main, exit
  sb_printf(out, "  .globl __entry\n__entry:\n");
  sb_printf(out, "  auipc t0, %%hi(%s)\n", heap_base_sym);
  sb_printf(out, "  addi t0, t0, %%lo(%s)\n", heap_base_sym);
  sb_printf(out, "  sw a0, 0(t0)\n");
  sb_printf(out, "  auipc t0, %%hi(%s)\n", heap_top_sym);
  sb_printf(out, "  addi t0, t0, %%lo(%s)\n", heap_top_sym);
  sb_printf(out, "  sw a1, 0(t0)\n");
  if (g_main_symbol) {
    sb_printf(out, "  auipc ra, %%hi(%s)\n", g_main_symbol);
    sb_printf(out, "  jalr ra, ra, %%lo(%s)\n", g_main_symbol);
  }
  sb_printf(out, "  lui t0, 0x80000\n");
  sb_printf(out, "  sw a0, 4(t0)\n");
  sb_printf(out, "__idle:\n");
  sb_printf(out, "  jal zero, __idle\n");

  for (size_t i = 0; i < g_ir_fns.n; i++)
    emit_fn32(&e, g_ir_fns.items[i]);

  emit_rt_stubs(out);

  // literals: header block + raw bytes (buf/ptr/len stride stays 8)
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    for (size_t j = 0; j < fn->literals.n; j++) {
      IRLiteral *l = fn->literals.items[j];
      // 24-byte header (3 quads, low word carries each 8-byte field) so the
      // bytes begin at litaddr + 24, the stride the lower hard-codes
      sb_printf(out,
                "  .p2align 2\nL%s:\n  .word 0x80000000\n  .word 0\n  .word 0\n  .word 0\n"
                "  .word 0\n  .word 0\nL%s_b:\n  .ascii \"",
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

  // globals: same 24-byte static-slice header shape as arm64 (each relocated
  // quad emits as a .word pair; the low word carries the 32-bit pointer)
  for (size_t i = 0; i < g_ir_globals.n; i++) {
    IRGlobal *g = g_ir_globals.items[i];
    sb_printf(out, "\n  .globl %s\n  .p2align 2\n%s:\n", g->symbol, g->symbol);
    if (g->relocs) {
      for (int w = 0; w < 3; w++) {
        if (g->relocs[w]) {
          sb_printf(out, "  .word %s\n  .word 0\n", g->relocs[w]);
        } else {
          uint32_t v = 0;
          for (int b = 0; b < 4; b++)
            v |= (uint32_t)(unsigned char)(long)g->init.items[w * 8 + b] << (8 * b);
          sb_printf(out, "  .word %u\n", v);
          v = 0;
          for (int b = 4; b < 8; b++)
            v |= (uint32_t)(unsigned char)(long)g->init.items[w * 8 + b] << (8 * (b - 4));
          sb_printf(out, "  .word %u\n", v);
        }
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
}


// ------------------------------------------------------------ assembler ----

// two-pass assembler for the restricted RV32IM dialect the emitter prints:
// ABI register names, decimal immediates, labels, %hi/%lo symbol refs, and
// the directives .globl/.p2align/.word/.byte/.zero/.ascii

typedef struct {
  char *name;
  uint32_t addr;
} RVLabel;

static int rv_reg(const char *s) {
  static const char *names[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
                                  "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
                                  "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
                                  "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
  for (int i = 0; i < 32; i++)
    if (strcmp(s, names[i]) == 0)
      return i;
  return -1;
}

static uint32_t rv_enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
                         uint32_t op) {
  return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t rv_enc_i(uint32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return (imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static uint32_t rv_enc_s(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  return (imm >> 5) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (imm & 0x1f) << 7 | op;
}
static uint32_t rv_enc_b(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  return ((imm >> 12) & 1) << 31 | ((imm >> 5) & 0x3f) << 25 | rs2 << 20 | rs1 << 15 |
         f3 << 12 | ((imm >> 1) & 0xf) << 8 | ((imm >> 11) & 1) << 7 | op;
}
static uint32_t rv_enc_u(uint32_t imm20, uint32_t rd, uint32_t op) {
  return (imm20 & 0xfffff) << 12 | rd << 7 | op;
}
static uint32_t rv_enc_j(uint32_t imm, uint32_t rd, uint32_t op) {
  return ((imm >> 20) & 1) << 31 | ((imm >> 1) & 0x3ff) << 21 | ((imm >> 11) & 1) << 20 |
         ((imm >> 12) & 0xff) << 12 | rd << 7 | op;
}

// decode one comma-separated operand into a register number; returns -1 if
// the operand is not a register
static int rv_parse_reg(const char *s) {
  while (*s == ' ')
    s++;
  char buf[8];
  int n = 0;
  while (s[n] && s[n] != ',' && s[n] != ' ' && n < 7) {
    buf[n] = s[n];
    n++;
  }
  buf[n] = 0;
  return rv_reg(buf);
}

// split "imm(reg)" — returns imm and fills base; returns -1 on malformed
static int64_t rv_mem_split(const char *s, int *base) {
  const char *lp = strchr(s, '(');
  const char *rp = lp ? strchr(lp, ')') : NULL;
  if (!lp || !rp)
    return -1;
  char basebuf[8];
  int n = 0;
  for (const char *q = lp + 1; q < rp && n < 7; q++)
    basebuf[n++] = *q;
  basebuf[n] = 0;
  *base = rv_reg(basebuf);
  if (*base < 0)
    return -1;
  return strtoll(s, NULL, 10);
}

static uint32_t rv_sym_hi(uint32_t addr) {
  return (((addr + 0x800u) >> 12) & 0xfffff);
}
static uint32_t rv_sym_lo(uint32_t addr) {
  uint32_t hi = rv_sym_hi(addr);
  return addr - (hi << 12);
}

// pc-relative pair resolution: the auipc's delta decides both halves
static uint32_t g_rv32_pair_pc;
static int g_rv32_asm_fail;
static const char *g_rv32_line;

// label lookup that names its misses: a symbol unresolved by pass 2 (the
// label table is complete by then) would otherwise silently encode as 0
// and the machine would jump to address 0
static uint32_t rv_label_addr(RVLabel *labels, size_t labels_n, const char *name) {
  for (size_t i = 0; i < labels_n; i++)
    if (strcmp(labels[i].name, name) == 0)
      return labels[i].addr;
  fprintf(stderr, "rho: rv32 asm: unresolved symbol '%s' in: %s\n", name, g_rv32_line ? g_rv32_line : "?");
  g_rv32_asm_fail = 1;
  return 0;
}
static uint32_t rv_pair_hi(uint32_t sym, uint32_t pc) {
  uint32_t delta = sym - pc;
  return ((delta + 0x800u) >> 12) & 0xfffff;
}
static uint32_t rv_pair_lo(uint32_t sym, uint32_t pc) {
  uint32_t delta = sym - pc;
  uint32_t hi = (delta + 0x800u) >> 12;
  return delta - (hi << 12);
}

uint32_t g_rv32_last_size = 0;

int assemble_rv32(const char *text, unsigned char **image_out) {
  // pass 1: sizes + labels
  uint32_t addr = 0;
  RVLabel *labels = NULL;
  size_t labels_n = 0, labels_cap = 0;
  const char *p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    if (len >= sizeof(line))
      len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    const char *s = line;
    g_rv32_line = s;
    while (*s == ' ' || *s == '\t')
      s++;
    if (strncmp(s, ".p2align", 8) == 0) {
      int k = atoi(s + 8);
      uint32_t mask = (1u << k) - 1;
      addr = (addr + mask) & ~mask;
    } else if (strncmp(s, ".zero", 5) == 0) {
      addr += (uint32_t)strtoul(s + 5, NULL, 10);
    } else if (strncmp(s, ".word", 5) == 0) {
      addr += 4;
    } else if (strncmp(s, ".byte", 5) == 0) {
      const char *q = s + 5;
      int cnt = 0;
      while (*q) {
        if (*q == ',')
          cnt++;
        q++;
      }
      addr += (uint32_t)cnt + 1;
    } else if (strncmp(s, ".ascii", 6) == 0) {
      const char *q = strchr(s, '"');
      const char *qe = q ? strchr(q + 1, '"') : NULL;
      if (!q || !qe) {
        fprintf(stderr, "rho: rv32asm bad .ascii\n");
        return 1;
      }
      // count DECODED bytes: escape sequences collapse (three-digit octal
      // \\012 is four raw chars, one byte) — pass 2 writes decoded bytes
      uint32_t nb = 0;
      for (const char *r = q + 1; r < qe; r++) {
        if (*r == '\\') {
          r++;
          if (*r >= '0' && *r <= '7')
            r += 2;
        }
        nb++;
      }
      addr += nb;
    } else {
      const char *colon = strchr(s, ':');
      if (colon) {
        char name[128];
        size_t nl = (size_t)(colon - s);
        if (nl >= sizeof(name))
          nl = sizeof(name) - 1;
        memcpy(name, s, nl);
        name[nl] = 0;
        if (labels_n == labels_cap) {
          labels_cap = labels_cap ? labels_cap * 2 : 256;
          labels = realloc(labels, labels_cap * sizeof(RVLabel));
        }
        labels[labels_n].name = strdup(name);
        labels[labels_n].addr = addr;
        labels_n++;
        if (getenv("RHO_ASM_DEBUG"))
          fprintf(stderr, "asm: %s %u\n", name, addr);
      } else if (*s && strncmp(s, ".globl", 6) != 0) {
        addr += 4; // every instruction is 4 bytes in our dialect
      }
    }
    p = eol ? eol + 1 : p + strlen(p);
  }

  uint32_t image_size = addr;
  unsigned char *image = calloc(1, image_size ? image_size : 4);

  // pass 2: emit
  addr = 0;
  p = text;
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    if (len >= sizeof(line))
      len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = 0;
    const char *s = line;
    g_rv32_line = s;
    while (*s == ' ' || *s == '\t')
      s++;
    if (strncmp(s, ".p2align", 8) == 0) {
      int k = atoi(s + 8);
      uint32_t mask = (1u << k) - 1;
      uint32_t na = (addr + mask) & ~mask;
      addr = na;
    } else if (strncmp(s, ".word", 5) == 0) {
      const char *q = s + 5;
      uint32_t v;
      while (*q == ' ')
        q++;
      if (*q >= '0' && *q <= '9') {
        v = (uint32_t)strtoull(q, NULL, 0);
      } else {
        char name[128];
        sscanf(q, "%127s", name);
        v = rv_label_addr(labels, labels_n, name);
      }
      memcpy(image + addr, &v, 4);
      addr += 4;
    } else if (strncmp(s, ".zero", 5) == 0) {
      addr += (uint32_t)strtoul(s + 5, NULL, 10);
    } else if (strncmp(s, ".byte", 5) == 0) {
      const char *q = s + 5;
      while (*q) {
        while (*q == ' ' || *q == ',')
          q++;
        if (!*q)
          break;
        image[addr] = (unsigned char)strtoul(q, (char **)&q, 10);
        addr += 1;
      }
    } else if (strncmp(s, ".ascii", 6) == 0) {
      const char *q = strchr(s, '"');
      q++;
      while (*q && *q != '"') {
        unsigned char ch = (unsigned char)*q;
        if (ch == '\\') {
          q++;
          if (*q == 'n') {
            ch = '\n';
          } else if (*q == 't') {
            ch = '\t';
          } else if (*q == 'r') {
            ch = '\r';
          } else if (*q == '\\') {
            ch = '\\';
          } else if (*q == '"') {
            ch = '"';
          } else if (*q >= '0' && *q <= '7') {
            ch = (unsigned char)((q[0] - '0') * 64 + (q[1] - '0') * 8 + (q[2] - '0'));
            q += 2;
          }
        }
        image[addr++] = ch;
        q++;
      }
    } else if (strncmp(s, ".globl", 6) == 0) {
      // nothing to emit
    } else {
      const char *colon = strchr(s, ':');
      if (colon) {
        // label: binds in pass 1
      } else if (*s) {
        char mn[16];
        sscanf(s, "%15s", mn);
        // operand list after the mnemonic
        const char *ops = s + strlen(mn);
        uint32_t ins = 0;
        int ok = 1;
        // gather operand text pieces
        char o1[64] = "", o2[64] = "", o3[64] = "", o4[64] = "";
        char *dst[4] = {o1, o2, o3, o4};
        int no = 0;
        const char *q = ops;
        while (*q && no < 4) {
          while (*q == ' ' || *q == ',')
            q++;
          if (!*q)
            break;
          int n = 0;
          while (q[n] && q[n] != ',' && n < 63) {
            dst[no][n] = q[n];
            n++;
          }
          while (n > 0 && dst[no][n - 1] == ' ')
            n--;
          dst[no][n] = 0;
          no++;
          q += n;
        }
        if (!strcmp(mn, "lui")) {
          uint32_t v;
          if (o2[0] == '%') {
            char name[128];
            int hi = o2[1] == 'h';
            sscanf(o2 + 4, "%127[^)]", name);
            uint32_t value = rv_label_addr(labels, labels_n, name);
            v = hi ? rv_sym_hi(value) : rv_sym_lo(value);
          } else {
            v = (uint32_t)strtoul(o2, NULL, 0);
          }
          ins = rv_enc_u(v, rv_reg(o1), 0x37);
        } else if (!strcmp(mn, "auipc")) {
          uint32_t v;
          if (o2[0] == '%') {
            char name[128];
            sscanf(o2 + 4, "%127[^)]", name);
            uint32_t value = rv_label_addr(labels, labels_n, name);
            v = rv_pair_hi(value, addr);
            g_rv32_pair_pc = addr;
          } else {
            v = (uint32_t)strtoul(o2, NULL, 0);
          }
          ins = rv_enc_u(v, rv_reg(o1), 0x17);
        } else if (!strcmp(mn, "jal")) {
          int rd = rv_reg(o1);
          uint32_t target = rv_label_addr(labels, labels_n, o2);
          ins = rv_enc_j(target - addr, rd, 0x6f);
        } else if (!strcmp(mn, "jalr")) {
          int rd = rv_reg(o1);
          int base = -1;
          int64_t imm = rv_mem_split(o2, &base);
          if (imm < 0) {
            // jalr rd, rs1, imm — imm may be %lo(sym)
            base = rv_reg(o2);
            if (o3[0] == '%') {
              char name[128];
              sscanf(o3 + 4, "%127[^)]", name);
              uint32_t value = rv_label_addr(labels, labels_n, name);
              imm = (int64_t)rv_pair_lo(value, g_rv32_pair_pc);
            } else {
              imm = strtoll(o3, NULL, 0);
            }
          }
          ins = rv_enc_i((uint32_t)imm & 0xfff, base, 0, rd, 0x67);
        } else if (!strcmp(mn, "beq") || !strcmp(mn, "bne") || !strcmp(mn, "blt") ||
                   !strcmp(mn, "bge") || !strcmp(mn, "bltu") || !strcmp(mn, "bgeu")) {
          static const struct {
            const char *n;
            uint32_t f3;
          } brs[6] = {{"beq", 0}, {"bne", 1}, {"blt", 4}, {"bge", 5}, {"bltu", 6}, {"bgeu", 7}};
          uint32_t f3 = 0;
          for (int i = 0; i < 6; i++)
            if (!strcmp(mn, brs[i].n))
              f3 = brs[i].f3;
          uint32_t target = rv_label_addr(labels, labels_n, o3);
          ins = rv_enc_b(target - addr, rv_reg(o2), rv_reg(o1), f3, 0x63);
        } else if (!strcmp(mn, "lb") || !strcmp(mn, "lh") || !strcmp(mn, "lw") ||
                   !strcmp(mn, "lbu") || !strcmp(mn, "lhu")) {
          static const struct {
            const char *n;
            uint32_t f3;
          } lds[5] = {{"lb", 0}, {"lh", 1}, {"lw", 2}, {"lbu", 4}, {"lhu", 5}};
          uint32_t f3 = 2;
          for (int i = 0; i < 5; i++)
            if (!strcmp(mn, lds[i].n))
              f3 = lds[i].f3;
          int base = -1;
          int64_t imm = rv_mem_split(o2, &base);
          ins = rv_enc_i((uint32_t)imm & 0xfff, base, f3, rv_reg(o1), 0x03);
        } else if (!strcmp(mn, "sb") || !strcmp(mn, "sh") || !strcmp(mn, "sw")) {
          uint32_t f3 = !strcmp(mn, "sb") ? 0 : !strcmp(mn, "sh") ? 1 : 2;
          int base = -1;
          int64_t imm = rv_mem_split(o2, &base);
          ins = rv_enc_s((uint32_t)imm & 0xfff, rv_reg(o1), base, f3, 0x23);
        } else if (!strcmp(mn, "addi") || !strcmp(mn, "slti") || !strcmp(mn, "sltiu") ||
                   !strcmp(mn, "xori") || !strcmp(mn, "ori") || !strcmp(mn, "andi")) {
          static const struct {
            const char *n;
            uint32_t f3;
          } ims[6] = {{"addi", 0}, {"slti", 2}, {"sltiu", 3}, {"xori", 4}, {"ori", 6}, {"andi", 7}};
          uint32_t f3 = 0;
          for (int i = 0; i < 6; i++)
            if (!strcmp(mn, ims[i].n))
              f3 = ims[i].f3;
          // addi rd, rs1, imm — imm may be %lo(sym) paired with the
          // preceding auipc (absolute %hi also works for lui pairs)
          uint32_t imm;
          if (o3[0] == '%') {
            char name[128];
            sscanf(o3 + 4, "%127[^)]", name);
            uint32_t value = rv_label_addr(labels, labels_n, name);
            imm = o3[1] == 'h' ? rv_sym_hi(value) : rv_pair_lo(value, g_rv32_pair_pc);
          } else {
            imm = (uint32_t)(int64_t)strtoll(o3, NULL, 0);
          }
          ins = rv_enc_i(imm & 0xfff, rv_reg(o2), f3, rv_reg(o1), 0x13);
        } else if (!strcmp(mn, "slli") || !strcmp(mn, "srli") || !strcmp(mn, "srai")) {
          uint32_t f7 = !strcmp(mn, "slli") ? 0x00 : !strcmp(mn, "srli") ? 0x20 : 0x60;
          uint32_t f3 = !strcmp(mn, "slli") ? 1 : 5;
          uint32_t sh = (uint32_t)strtoul(o3, NULL, 0);
          ins = (f7 << 25) | (sh & 0x1f) << 20 | rv_reg(o2) << 15 | f3 << 12 | rv_reg(o1) << 7 |
                0x13;
        } else if (!strcmp(mn, "add") || !strcmp(mn, "sub") || !strcmp(mn, "sll") ||
                   !strcmp(mn, "slt") || !strcmp(mn, "sltu") || !strcmp(mn, "xor") ||
                   !strcmp(mn, "srl") || !strcmp(mn, "sra") || !strcmp(mn, "or") ||
                   !strcmp(mn, "and") || !strcmp(mn, "mul") || !strcmp(mn, "mulh") ||
                   !strcmp(mn, "mulhsu") || !strcmp(mn, "mulhu") || !strcmp(mn, "div") ||
                   !strcmp(mn, "divu") || !strcmp(mn, "rem") || !strcmp(mn, "remu")) {
          static const struct {
            const char *n;
            uint32_t f3, f7;
          } ops[18] = {{"add", 0, 0},      {"sub", 0, 0x20},  {"sll", 1, 0},
                       {"slt", 2, 0},      {"sltu", 3, 0},    {"xor", 4, 0},
                       {"srl", 5, 0},      {"sra", 5, 0x20},  {"or", 6, 0},
                       {"and", 7, 0},      {"mul", 0, 1},     {"mulh", 1, 1},
                       {"mulhsu", 2, 1},   {"mulhu", 3, 1},   {"div", 4, 1},
                       {"divu", 5, 1},     {"rem", 6, 1},     {"remu", 7, 1}};
          uint32_t f3 = 0, f7 = 0;
          for (int i = 0; i < 18; i++)
            if (!strcmp(mn, ops[i].n)) {
              f3 = ops[i].f3;
              f7 = ops[i].f7;
            }
          ins = rv_enc_r(f7, rv_reg(o3), rv_reg(o2), f3, rv_reg(o1), 0x33);
        } else if (!strcmp(mn, "seqz")) {
          ins = rv_enc_i(1, rv_reg(o2), 3, rv_reg(o1), 0x13); // sltiu rd, rs, 1
        } else if (!strcmp(mn, "snez")) {
          ins = rv_enc_r(0, rv_reg(o2), 0, 3, rv_reg(o1), 0x33); // sltu rd, zero, rs
        } else if (!strcmp(mn, "mv")) {
          ins = rv_enc_i(0, rv_reg(o2), 0, rv_reg(o1), 0x13);
        } else if (!strcmp(mn, "not")) {
          ins = rv_enc_i(0xfff, rv_reg(o2), 4, rv_reg(o1), 0x13);
        } else if (!strcmp(mn, "ebreak")) {
          ins = 0x00100073;
        } else {
          ok = 0;
        }
        if (!ok) {
          fprintf(stderr, "rho: rv32asm unknown instruction: %s\n", s);
          return 1;
        }
        memcpy(image + addr, &ins, 4);
        addr += 4;
      }
    }
    p = eol ? eol + 1 : p + strlen(p);
  }
  free(labels);
  if (g_rv32_asm_fail) {
    free(image);
    return 1;
  }
  *image_out = image;
  g_rv32_last_size = image_size;
  return 0;
}
