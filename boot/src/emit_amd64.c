#include "ir.h"

// amd64 System V emitter — spill-everything: every vreg owns a frame slot,
// instructions reload operands into scratch registers. Registers are free
// between instructions, which keeps allocation trivial and correctness
// boring. Optimization replaces this policy after self-hosting.

#define RAX "%rax"
#define RCX "%rcx"
#define RDX "%rdx"
#define XMM0 "%xmm0"
#define XMM1 "%xmm1"
#define RDI "%rdi"
#define RSI "%rsi"

typedef struct Emitter {
  Target tgt;
  SB *out;
  IRFn *fn;
  IRBlock *cur; // block currently being emitted
  Vec slot_off; // int64 offsets from rbp (negative), parallel to fn->slots
  int64_t frame;
} Emitter;

static bool mac(Target t) { return t == TGT_AMD64_MAC || t == TGT_ARM64_MAC; }

static const char *sym(Emitter *e, const char *s) {
  if (mac(e->tgt))
    return arena_printf("_%s", s);
  return s;
}

static const char *lit_label(Emitter *e, const char *l) {
  if (mac(e->tgt))
    return arena_printf("L%s", l);
  return arena_printf(".%s", l);
}

// ---------------------------------------------------------------- layout --

static int64_t slot_off(Emitter *e, IRSlot *s) {
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

static void layout_frame(Emitter *e) {
  e->slot_off = (Vec){0};
  e->frame = 0;
  for (size_t i = 0; i < e->fn->slots.n; i++)
    slot_off(e, e->fn->slots.items[i]);
  // vreg spill slots
  for (int v = 0; v < e->fn->next_vreg; v++) {
    IRSlot virt = {0};
    virt.id = -1;
    virt.size = 8;
    virt.align = 8;
    // reserve by direct allocation
    int64_t off = e->frame;
    off += 8;
    e->frame = off;
    vec_push(&e->slot_off, (void *)(long)(-off)); // vreg v gets slot_off.n slot
  }
  e->frame = (e->frame + 15) / 16 * 16 + 16;
}

// vreg v: index = fn->slots.n + v
static int64_t vreg_off(Emitter *e, IRVreg *v) {
  size_t idx = e->fn->slots.n + (size_t)v->id;
  if (idx >= e->slot_off.n) {
    // layout_frame guaranteed one entry per vreg
    while (e->slot_off.n <= idx)
      vec_push(&e->slot_off, (void *)0);
  }
  return (long)e->slot_off.items[idx];
}

static void mov_slot_reg(Emitter *e, int64_t off, int64_t size, bool f64, const char *reg) {
  // load from slot into reg
  if (f64)
    sb_printf(e->out, "  movsd %lld(%%rbp), %s\n", off, reg);
  else if (size == 1)
    sb_printf(e->out, "  movsbq %lld(%%rbp), %s\n", off, reg);
  else if (size == 2)
    sb_printf(e->out, "  movswq %lld(%%rbp), %s\n", off, reg);
  else if (size == 4)
    sb_printf(e->out, "  movslq %lld(%%rbp), %s\n", off, reg);
  else
    sb_printf(e->out, "  movq %lld(%%rbp), %s\n", off, reg);
}

static void mov_reg_slot(Emitter *e, int64_t off, int64_t size, bool f64, const char *reg) {
  if (f64)
    sb_printf(e->out, "  movsd %s, %lld(%%rbp)\n", reg, off);
  else if (size == 1)
    sb_printf(e->out, "  movb %s, %lld(%%rbp)\n", reg, off);
  else if (size == 2)
    sb_printf(e->out, "  movw %s, %lld(%%rbp)\n", reg, off);
  else if (size == 4)
    sb_printf(e->out, "  movl %s, %lld(%%rbp)\n", reg, off);
  else
    sb_printf(e->out, "  movq %s, %lld(%%rbp)\n", reg, off);
}


// ---------------------------------------------------------------- body ----

static const char *cc_suffix(IRCC cc, bool is_float, bool is_unsigned) {
  if (is_float) {
    // NaN-safe: emitter emits explicit code for float cmps
    switch (cc) {
    case CC_EQ: return "eq";
    case CC_NE: return "ne";
    case CC_LT: return "b";
    case CC_LE: return "be";
    case CC_GT: return "a";
    case CC_GE: return "ae";
    }
  }
  switch (cc) {
  case CC_EQ: return "e";
  case CC_NE: return "ne";
  case CC_LT: return is_unsigned ? "b" : "l";
  case CC_LE: return is_unsigned ? "be" : "le";
  case CC_GT: return is_unsigned ? "a" : "g";
  case CC_GE: return is_unsigned ? "ae" : "ge";
  }
  return "e";
}

static int g_label_counter = 0;

static void emit_cmp(Emitter *e, IRIns *i) {
  int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
  if (!i->is_float) {
    bool u = !i->signed_ops;
    mov_slot_reg(e, ao, i->size, false, RAX);
    mov_slot_reg(e, bo, i->size, false, RCX);
    sb_printf(e->out, "  cmpq %s, %s\n", RCX, RAX);
    sb_printf(e->out, "  set%s %%al\n", cc_suffix(i->cc, false, u));
    sb_printf(e->out, "  movzbq %%al, %%rax\n");
    sb_printf(e->out, "  movq %%rax, %lld(%%rbp)\n", dof);
    return;
  }
  // floats: NaN-safe comparisons
  bool size64 = i->size == 8;
  const char *sfx = size64 ? "sd" : "ss";
  mov_slot_reg(e, ao, size64 ? 8 : 4, true, XMM0);
  mov_slot_reg(e, bo, size64 ? 8 : 4, true, XMM1);
  sb_printf(e->out, "  ucomi%s %s, %s\n", sfx, XMM1, XMM0);
  int lbl = g_label_counter++;
  const char *cc = cc_suffix(i->cc, true, false);
  if (!strcmp(cc, "eq")) {
    // eq = ZF && !PF
    sb_printf(e->out, "  sete %%al\n  setnp %%cl\n  andb %%cl, %%al\n");
  } else if (!strcmp(cc, "ne")) {
    sb_printf(e->out, "  setne %%al\n  setp %%cl\n  orb %%cl, %%al\n");
  } else if (!strcmp(cc, "b")) {
    // lt = CF && !PF
    sb_printf(e->out, "  setb %%al\n  setnp %%cl\n  andb %%cl, %%al\n");
  } else if (!strcmp(cc, "be")) {
    sb_printf(e->out, "  setbe %%al\n  setnp %%cl\n  andb %%cl, %%al\n");
  } else if (!strcmp(cc, "a")) {
    sb_printf(e->out, "  seta %%al\n  setnp %%cl\n  andb %%cl, %%al\n");
  } else {
    sb_printf(e->out, "  setae %%al\n  setnp %%cl\n  andb %%cl, %%al\n");
  }
  (void)lbl;
  sb_printf(e->out, "  movzbq %%al, %%rax\n");
  sb_printf(e->out, "  movq %%rax, %lld(%%rbp)\n", dof);
}

static void emit_divmod(Emitter *e, IRIns *i, bool is_mod) {
  int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
  int lbl = g_label_counter++;
  if (i->is_float) {
    mov_slot_reg(e, ao, 8, true, XMM0);
    mov_slot_reg(e, bo, 8, true, XMM1);
    sb_printf(e->out, "  divsd %s, %s\n", XMM1, XMM0);
    mov_reg_slot(e, dof, 8, true, XMM0);
    return;
  }
  const char *ext = i->signed_ops ? "cqo" : "xorl %edx, %edx";
  sb_printf(e->out, "Ldiv%d_guard:\n", lbl);
  mov_slot_reg(e, bo, i->size, false, RCX);
  sb_printf(e->out, "  testq %s, %s\n", RCX, RCX);
  sb_printf(e->out, "  jne Ldiv%d_nz\n", lbl);
  sb_printf(e->out, "  movq %lld(%%rbp), %%rdi\n", ao);
  sb_printf(e->out, "  jmp Ldiv%d_neg\n", lbl);
  sb_printf(e->out, "Ldiv%d_neg:\n", lbl);
  sb_printf(e->out, "  call %s\n", sym(e, prelude_symbol("__panic_div")));
  sb_printf(e->out, "Ldiv%d_nz:\n", lbl);
  if (i->signed_ops) {
    // MIN / -1 wraps to MIN: special-case divisor == -1
    sb_printf(e->out, "  cmpq $-1, %s\n", RCX);
    sb_printf(e->out, "  jne Ldiv%d_ok\n", lbl);
    mov_slot_reg(e, ao, i->size, false, RAX);
    sb_printf(e->out, "  negq %s\n", RAX);
    if (is_mod)
      sb_printf(e->out, "  xorq %s, %s\n", RDX, RDX);
    sb_printf(e->out, "  jmp Ldiv%d_done\n", lbl);
    sb_printf(e->out, "Ldiv%d_ok:\n", lbl);
  }
  mov_slot_reg(e, ao, i->size, false, RAX);
  sb_printf(e->out, "  %s\n", ext);
  sb_printf(e->out, "  %s %s\n", i->signed_ops ? "idivq" : "divq", RCX);
  sb_printf(e->out, "Ldiv%d_done:\n", lbl);
  if (is_mod)
    mov_reg_slot(e, dof, 8, false, RDX);
  else
    mov_reg_slot(e, dof, 8, false, RAX);
}

// parallel copies for phis on the edge out of the current block to `succ`
static void emit_edge_copies(Emitter *e, IRBlock *succ) {
  // collect (dst_off, src_off) pairs
  Vec dsts = {0}, srcs = {0};
  for (size_t p = 0; p < succ->phis.n; p++) {
    IRPhi *phi = succ->phis.items[p];
    IRBlock *cur = e->cur;
    for (size_t k = 0; k < phi->preds.n; k++) {
      if (phi->preds.items[k] == cur) {
        IRVreg *val = phi->args.items[k];
        vec_push(&dsts, (void *)(long)vreg_off(e, phi->dst));
        vec_push(&srcs, (void *)(long)vreg_off(e, val));
        break;
      }
    }
  }
  while (dsts.n) {
    // find a copy whose dst is not any remaining src (a safe copy)
    long done_at = -1;
    for (size_t i = 0; i < dsts.n; i++) {
      long d = (long)dsts.items[i];
      bool is_src = false;
      for (size_t j = 0; j < srcs.n; j++)
        if ((long)srcs.items[j] == d)
          is_src = true;
      if (!is_src) {
        done_at = (long)i;
        break;
      }
    }
    size_t pick = done_at >= 0 ? (size_t)done_at : 0;
    long d = (long)dsts.items[pick], s = (long)srcs.items[pick];
    if (done_at < 0) {
      // cycle: break it through the temp slot at -frame-8
      sb_printf(e->out, "  movq %ld(%%rbp), %%rax\n", s);
      sb_printf(e->out, "  movq %%rax, -%lld(%%rbp)\n", e->frame - 8);
      dsts.items[pick] = (void *)s; // copy temp -> dst next round
      srcs.items[pick] = (void *)(-(long)(e->frame - 8));
      continue;
    }
    sb_printf(e->out, "  movq %ld(%%rbp), %%rax\n", s);
    sb_printf(e->out, "  movq %%rax, %ld(%%rbp)\n", d);
    // remove
    for (size_t j = pick; j + 1 < dsts.n; j++) {
      dsts.items[j] = dsts.items[j + 1];
      srcs.items[j] = srcs.items[j + 1];
    }
    dsts.n--;
    srcs.n--;
  }
}

static void emit_call(Emitter *e, IRIns *i) {
  // classify args: int-class regs rdi..r9, float xmm0..7
  const char *intregs[] = {RDI, RSI, RDX, RCX, "%r8", "%r9"};
  int ri = 0, fi = 0;
  if (i->callee_vreg) // indirect target rides in r10, outside the arg regs
    sb_printf(e->out, "  movq %lld(%%rbp), %%r10\n", vreg_off(e, i->callee_vreg));
  for (size_t k = 0; k < i->args.n; k++) {
    IRArg *a = i->args.items[k];
    bool is_float_arg = a->ty && (a->ty->kind == TY_F32 || a->ty->kind == TY_F64);
    int64_t off = vreg_off(e, a->vreg);
    if (is_float_arg && fi < 8) {
      sb_printf(e->out, "  movsd %lld(%%rbp), %%xmm%d\n", off, fi++);
    } else if (!is_float_arg && ri < 6) {
      sb_printf(e->out, "  movq %lld(%%rbp), %s\n", off, intregs[ri++]);
    } else {
      // stack arg (rare): push
      sb_printf(e->out, "  movq %lld(%%rbp), %%rax\n  pushq %%rax\n", off);
    }
  }
  if (i->callee_vreg)
    sb_printf(e->out, "  call *%%r10\n");
  else
    sb_printf(e->out, "  call %s\n", sym(e, i->callee));
  if (i->dst) {
    bool f = ir_is_float(i->dst->ty);
    mov_reg_slot(e, vreg_off(e, i->dst), f ? (i->dst->ty == IT_F32 ? 4 : 8) : 8, f, f ? XMM0 : RAX);
  }
}

static void emit_ins(Emitter *e, IRIns *i) {
  switch (i->op) {
  case IR_CONST: {
    int64_t dof = vreg_off(e, i->dst);
    if ((int64_t)i->imm >= INT32_MIN && (int64_t)i->imm <= INT32_MAX)
      sb_printf(e->out, "  movq $%lld, %lld(%%rbp)\n", (long long)i->imm, dof);
    else
      sb_printf(e->out, "  movabsq $%lld, %%rax\n  movq %%rax, %lld(%%rbp)\n",
                (long long)i->imm, dof);
    break;
  }
  case IR_FCONST: {
    // materialize via a literal double in rodata
    int lbl = g_label_counter++;
    sb_printf(e->out, "  movsd Lfconst%d(%%rip), %%xmm0\n", lbl);
    mov_reg_slot(e, vreg_off(e, i->dst), 8, true, XMM0);
    e->fn->literals.n = e->fn->literals.n; // no-op; float consts appended below
    IRIns *keep = i;
    (void)keep;
    // stash for the rodata pass
    extern void amd64_note_fconst(int lbl, double v);
    amd64_note_fconst(lbl, i->fimm);
    break;
  }
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_AND:
  case IR_OR:
  case IR_XOR: {
    int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
    const char *mn = i->op == IR_ADD ? "add" : i->op == IR_SUB ? "sub" : i->op == IR_MUL ? "imul"
                                                            : i->op == IR_AND            ? "and"
                                                            : i->op == IR_OR             ? "or"
                                                                                          : "xor";
    if (i->is_float) {
      mov_slot_reg(e, ao, 8, true, XMM0);
      mov_slot_reg(e, bo, 8, true, XMM1);
      const char *fmn = i->op == IR_ADD ? "addsd" : i->op == IR_SUB ? "subsd"
                                                    : i->op == IR_MUL ? "mulsd"
                                                                      : "divsd";
      sb_printf(e->out, "  %s %s, %s\n", fmn, XMM1, XMM0);
      mov_reg_slot(e, dof, 8, true, XMM0);
    } else {
      mov_slot_reg(e, ao, 8, false, RAX);
      mov_slot_reg(e, bo, 8, false, RCX);
      sb_printf(e->out, "  %s %s, %s\n", mn, RCX, RAX);
      mov_reg_slot(e, dof, 8, false, RAX);
    }
    break;
  }
  case IR_DIV:
  case IR_MOD:
    emit_divmod(e, i, i->op == IR_MOD);
    break;
  case IR_SHL:
  case IR_SHR: {
    int64_t ao = vreg_off(e, i->a), bo = vreg_off(e, i->b), dof = vreg_off(e, i->dst);
    mov_slot_reg(e, ao, 8, false, RAX);
    mov_slot_reg(e, bo, 8, false, RCX);
    const char *mn = i->op == IR_SHL ? "salq" : i->signed_ops ? "sarq" : "shrq";
    sb_printf(e->out, "  %s %%cl, %s\n", mn, RAX);
    mov_reg_slot(e, dof, 8, false, RAX);
    break;
  }
  case IR_CMP:
    emit_cmp(e, i);
    break;
  case IR_LOAD: {
    int64_t dof = vreg_off(e, i->dst);
    mov_slot_reg(e, vreg_off(e, i->addr), 8, false, RAX);
    if (i->is_float)
      sb_printf(e->out, "  %s (%%rax), %%xmm0\n", i->size == 8 ? "movsd" : "movss");
    else if (i->size == 1)
      sb_printf(e->out, "  movsbq (%%rax), %s\n", RAX);
    else if (i->size == 2)
      sb_printf(e->out, "  movswq (%%rax), %s\n", RAX);
    else if (i->size == 4)
      sb_printf(e->out, "  movslq (%%rax), %s\n", RAX);
    else
      sb_printf(e->out, "  movq (%%rax), %s\n", RAX);
    if (i->is_float)
      mov_reg_slot(e, dof, i->size, true, XMM0);
    else
      mov_reg_slot(e, dof, 8, false, RAX);
    break;
  }
  case IR_STORE: {
    mov_slot_reg(e, vreg_off(e, i->addr), 8, false, RAX);
    mov_slot_reg(e, vreg_off(e, i->a), i->size, i->is_float, i->is_float ? XMM0 : RCX);
    if (i->is_float)
      sb_printf(e->out, "  %s %s, (%%rax)\n", i->size == 8 ? "movsd" : "movss", XMM0);
    else if (i->size == 1)
      sb_printf(e->out, "  movb %%cl, (%%rax)\n");
    else if (i->size == 2)
      sb_printf(e->out, "  movw %%cx, (%%rax)\n");
    else if (i->size == 4)
      sb_printf(e->out, "  movl %%ecx, (%%rax)\n");
    else
      sb_printf(e->out, "  movq %s, (%%rax)\n", RCX);
    break;
  }
  case IR_ADDRC: {
    int64_t dof = vreg_off(e, i->dst);
    if (i->lit == -1) // global symbol address
      sb_printf(e->out, "  leaq %s(%%rip), %s\n", sym(e, i->callee), RAX);
    else
      sb_printf(e->out, "  leaq %lld(%%rbp), %s\n", slot_off(e, i->slot), RAX);
    sb_printf(e->out, "  movq %s, %lld(%%rbp)\n", RAX, dof);
    break;
  }
  case IR_ADDI: {
    int64_t dof = vreg_off(e, i->dst);
    mov_slot_reg(e, vreg_off(e, i->a), 8, false, RAX);
    sb_printf(e->out, "  addq $%lld, %s\n", (long long)i->imm, RAX);
    sb_printf(e->out, "  movq %s, %lld(%%rbp)\n", RAX, dof);
    break;
  }
  case IR_CAST: {
    int64_t sof = vreg_off(e, i->a), dof = vreg_off(e, i->dst);
    switch (i->cast) {
    case CAST_TRUNC:
      mov_slot_reg(e, sof, 8, false, RAX);
      mov_reg_slot(e, dof, i->size == 4 ? 4 : (int)i->size, false, RAX);
      break;
    case CAST_SEXT:
      mov_slot_reg(e, sof, ir_size_of(i->cast_from), false, RAX);
      mov_reg_slot(e, dof, 8, false, RAX);
      break;
    case CAST_ZEXT:
      mov_slot_reg(e, sof, ir_size_of(i->cast_from), false, RAX);
      if (ir_size_of(i->cast_from) == 1)
        sb_printf(e->out, "  movzbq %%al, %s\n", RAX);
      else if (ir_size_of(i->cast_from) == 2)
        sb_printf(e->out, "  movzwq %%ax, %s\n", RAX);
      else if (ir_size_of(i->cast_from) == 4)
        sb_printf(e->out, "  movl %%eax, %%eax\n");
      mov_reg_slot(e, dof, 8, false, RAX);
      break;
    case CAST_I2F:
      mov_slot_reg(e, sof, 8, false, RAX);
      sb_printf(e->out, "  %s %s, %s\n", i->cast_to == IT_F32 ? "cvtsi2ss" : "cvtsi2sd", RAX,
                XMM0);
      mov_reg_slot(e, dof, i->cast_to == IT_F32 ? 4 : 8, true, XMM0);
      break;
    case CAST_F2I:
      mov_slot_reg(e, sof, i->size, true, XMM0);
      sb_printf(e->out, "  %s %s, %s\n", i->cast_from == IT_F32 ? "cvttss2si" : "cvttsd2si", XMM0,
                RAX);
      mov_reg_slot(e, dof, 8, false, RAX);
      break;
    case CAST_F32_F64:
      mov_slot_reg(e, sof, 4, true, XMM0);
      sb_printf(e->out, "  cvtss2sd %s, %s\n", XMM0, XMM0);
      mov_reg_slot(e, dof, 8, true, XMM0);
      break;
    case CAST_F64_F32:
      mov_slot_reg(e, sof, 8, true, XMM0);
      sb_printf(e->out, "  cvtsd2ss %s, %s\n", XMM0, XMM0);
      mov_reg_slot(e, dof, 4, true, XMM0);
      break;
    case CAST_BITCOPY:
      mov_slot_reg(e, sof, 8, false, RAX);
      mov_reg_slot(e, dof, 8, false, RAX);
      break;
    }
    break;
  }
  case IR_COPYMEM: {
    mov_slot_reg(e, vreg_off(e, i->addr), 8, false, "%rdi");
    mov_slot_reg(e, vreg_off(e, i->a), 8, false, "%rsi");
    if (i->size < 0)
      mov_slot_reg(e, vreg_off(e, i->b), 8, false, "%rcx"); // runtime count
    else
      sb_printf(e->out, "  movq $%lld, %%rcx\n", (long long)i->size);
    sb_printf(e->out, "  cld\n  rep movsb\n");
    break;
  }
  case IR_ZERO: {
    mov_slot_reg(e, vreg_off(e, i->addr), 8, false, RAX);
    sb_printf(e->out, "  movq $%lld, %%rcx\n  movq $0, %%rdx\n", (long long)i->size);
    int lbl = g_label_counter++;
    sb_printf(e->out, "Lzero%d:\n  testq %%rcx, %%rcx\n  jz Lzero%d_end\n", lbl, lbl);
    sb_printf(e->out, "  movb %%dl, (%%rax)\n  incq %%rax\n  decq %%rcx\n  jnz Lzero%d\n", lbl);
    sb_printf(e->out, "Lzero%d_end:\n", lbl);
    break;
  }
  case IR_LITADDR: {
    int64_t dof = vreg_off(e, i->dst);
    IRLiteral *l = e->fn->literals.items[i->lit];
    sb_printf(e->out, "  leaq %s(%%rip), %s\n", lit_label(e, l->label), RAX);
    sb_printf(e->out, "  movq %s, %lld(%%rbp)\n", RAX, dof);
    break;
  }
  case IR_CALL:
    emit_call(e, i);
    break;
  default:
    break;
  }
}

static void emit_terminator(Emitter *e, IRIns *t) {
  if (t->op == (IROp)OP_BR) {
    IRBlock *to = (IRBlock *)t->dst;
    emit_edge_copies(e, to);
    sb_printf(e->out, "  jmp L%d_%d\n", e->fn->uid, to->id);
  } else if (t->op == (IROp)OP_CBR) {
    IRBlock *tt = (IRBlock *)t->dst;
    IRBlock *ff = (IRBlock *)t->b;
    int64_t cof = vreg_off(e, t->a);
    sb_printf(e->out, "  cmpb $0, %lld(%%rbp)\n", cof);
    sb_printf(e->out, "  je Lcbf%d\n", g_label_counter);
    emit_edge_copies(e, tt);
    sb_printf(e->out, "  jmp L%d_%d\n", e->fn->uid, tt->id);
    sb_printf(e->out, "Lcbf%d:\n", g_label_counter);
    emit_edge_copies(e, ff);
    sb_printf(e->out, "  jmp L%d_%d\n", e->fn->uid, ff->id);
    g_label_counter++;
  } else if (t->op == (IROp)OP_RET) {
    if (t->a) {
      mov_slot_reg(e, vreg_off(e, t->a), 8, ir_is_float(t->a->ty), ir_is_float(t->a->ty) ? XMM0 : RAX);
    }
    sb_printf(e->out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  ret\n");
  }
}

// float const pool (per-emission, not per-fn: labels are unique already)
typedef struct FConstNote {
  int lbl;
  double v;
} FConstNote;
static Vec g_fconsts = {0};

void amd64_note_fconst(int lbl, double v) {
  FConstNote *n = arena_alloc(sizeof(FConstNote));
  n->lbl = lbl;
  n->v = v;
  vec_push(&g_fconsts, n);
}

static int g_fn_uid = 0;

static void emit_fn(Emitter *e, IRFn *fn) {
  e->fn = fn;
  fn->uid = g_fn_uid++;
  layout_frame(e);
  const char *s = sym(e, fn->symbol);
  sb_printf(e->out, "\n  %s:\n", s);
  (void)0;
  sb_printf(e->out, "  pushq %%rbp\n  movq %%rsp, %%rbp\n");
  if (e->frame)
    sb_printf(e->out, "  subq $%lld, %%rsp\n", (long long)e->frame);

  // spill register params into their slots
  const char *intregs[] = {RDI, RSI, RDX, RCX, "%r8", "%r9"};
  int ri = 0, fi = 0;
  if (fn->returns_aggregate) {
    ri++; // rdi carries the out pointer
    sb_printf(e->out, "  movq %%rdi, %lld(%%rbp)\n", slot_off(e, fn->out_slot));
  }
  for (size_t p = 0; p < fn->params.n; p++) {
    IRSlot *slot = fn->params.items[p];
    Type *pt = fn->param_types.items[p];
    bool isf = pt && (pt->kind == TY_F32 || pt->kind == TY_F64);
    if (isf && fi < 8) {
      sb_printf(e->out, "  movsd %%xmm%d, %lld(%%rbp)\n", fi++, slot_off(e, slot));
    } else if (!isf && ri < 6) {
      sb_printf(e->out, "  movq %s, %lld(%%rbp)\n", intregs[ri++], slot_off(e, slot));
    } else {
      // stack param: above the saved rbp and return address
      int stack_index = (isf ? fi : ri) + (int)p;
      sb_printf(e->out, "  movq %d(%%rbp), %%rax\n",
                16 + 8 * (stack_index - 6 > 0 ? stack_index - 6 : 0));
      sb_printf(e->out, "  movq %%rax, %lld(%%rbp)\n", slot_off(e, slot));
    }
  }

  for (size_t bi = 0; bi < fn->blocks.n; bi++) {
    IRBlock *b = fn->blocks.items[bi];
    e->cur = b;
    sb_printf(e->out, "L%d_%d:\n", e->fn->uid, b->id);
    for (size_t ii = 0; ii < b->ins.n; ii++)
      emit_ins(e, b->ins.items[ii]);
    if (b->term)
      emit_terminator(e, b->term);
    else
      sb_printf(e->out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  ret\n"); // fall-off safety
  }
}

void emit_amd64(Target target, SB *out) {
  Emitter e = {0};
  e.tgt = target;
  e.out = out;
  bool is_mac = mac(target);
  if (is_mac) {
    sb_append_c(out, ".section __TEXT,__text,regular,pure_instructions\n");
    sb_append_c(out, ".build_version macos, 13, 0\n");
  } else {
    sb_append_c(out, ".text\n");
  }

  // per-fn float const pools
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    (void)fn;
  }

  for (size_t i = 0; i < g_ir_fns.n; i++)
    emit_fn(&e, g_ir_fns.items[i]);

  // string literals + float consts in rodata; every literal carries an
  // immortal rc header {sentinel, 0, null} so slice traffic no-ops on it.
  // Mach-O side needs a non-merging section: ld64 folds/reorders __cstring
  // literals, which would break the buf/+24 pairing
  sb_append_c(out, is_mac ? ".section __TEXT,__rhostr,regular\n"
                          : ".section .rodata\n");
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    for (size_t j = 0; j < fn->literals.n; j++) {
      IRLiteral *l = fn->literals.items[j];
      sb_printf(out, "  .p2align 3\n%s:\n  .quad 0x8000000000000000\n  .quad 0\n  .quad 0\n%s_b:\n  .ascii \"",
                lit_label(&e, l->label), lit_label(&e, l->label));
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
  for (size_t i = 0; i < g_fconsts.n; i++) {
    FConstNote *n = g_fconsts.items[i];
    sb_printf(out, "  .p2align 3\nLfconst%d:\n  .quad %llu\n", n->lbl,
              (unsigned long long)({ uint64_t b; __builtin_memcpy(&b, &n->v, 8); b; }));
  }

  // globals
  for (size_t i = 0; i < g_ir_globals.n; i++) {
    IRGlobal *g = g_ir_globals.items[i];
    sb_printf(out, "\n  .globl %s\n", sym(&e, g->symbol));
    sb_append_c(out, is_mac ? ".section __DATA,__data\n" : ".section .data\n");
    sb_printf(out, "  .p2align %d\n", g->align >= 8 ? 3 : g->align >= 4 ? 2 : g->align >= 2 ? 1 : 0);
    sb_printf(out, "%s:\n", sym(&e, g->symbol));
    size_t word = 0;
    if (g->relocs) {
      for (int w = 0; w < 3; w++) {
        if (g->relocs[w])
          sb_printf(out, "  .quad %s\n", lit_label(&e, g->relocs[w]));
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

  // C entry: call the root module's main
  if (g_main_symbol) {
    sb_append_c(out, is_mac ? ".section __TEXT,__text,regular,pure_instructions\n" : ".text\n");
    sb_printf(out, "\n  .globl %s\n", sym(&e, "main"));
    sb_printf(out, "%s:\n", sym(&e, "main"));
    sb_printf(out, "  pushq %%rbp\n  movq %%rsp, %%rbp\n");
    sb_printf(out, "  call %s\n", sym(&e, g_main_symbol));
    sb_printf(out, "  movq %%rbp, %%rsp\n  popq %%rbp\n  ret\n");
  }
}
