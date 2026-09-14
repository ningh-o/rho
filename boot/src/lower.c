#include "ir.h"

// AST -> SSA IR. Scalars travel in virtual registers; aggregates (structs,
// enums, slices, strings, arrays) live in frame slots and are addressed.
// An aggregate rvalue is represented by the address of a temp slot holding
// it; calls pass aggregates by pointer to the caller's copy (our documented
// ABI deviation).

Vec g_ir_fns = {0};
Vec g_ir_globals = {0};
const char *g_main_symbol = NULL;

static Module *g_root_module;

void lower_set_root(Module *m) { g_root_module = m; }

// per-function lowering context
typedef struct LScope {
  Map bindings;        // Str name -> IRSlot*
  Vec defers;          // Vec of Vec<Stmt*> (defer payloads, in order)
  struct LScope *parent;
  IRBlock *break_to, *continue_to;
} LScope;

typedef struct LCtx {
  IRFn *fn;
  LScope *scope;
  Vec panic_strs; // IRLiteral idx reuse
} LCtx;

static IRVreg *lv_expr(LCtx *c, Expr *e);
static IRVreg *fconst(LCtx *c, double v, IRType ty);
static IRVreg *compute_call_value(LCtx *c, Expr *e);
static IRVreg *block_value(LCtx *c, Vec *stmts, Type *t);
static int field_index_of(Expr *e, RecType *rec);
bool ty_is_signed_int(IRType t);
static void lv_agg(LCtx *c, Expr *e, IRVreg *dest);
static void lv_stmt(LCtx *c, Stmt *s);
static void lv_stmts(LCtx *c, Vec *stmts);
static void run_defers(LCtx *c, LScope *stop);
static IRVreg *compute_addr(LCtx *c, Expr *e);
static void panic_call(LCtx *c, const char *msg);
static IRVreg *lower_make(LCtx *c, Expr *e, IRVreg *dest);
static IRVreg *lower_slice(LCtx *c, Expr *e);
static IRVreg *lower_new(LCtx *c, Expr *e);
static bool is_make_call(Expr *e);
static Sym *prelude_fn(const char *name);
Type *struct_field_type(RecType *rec, size_t i);

// ------------------------------------------------------------- builders ----

static IRSlot *new_slot(LCtx *c, int64_t size, int64_t align, const char *name) {
  IRSlot *s = arena_alloc(sizeof(IRSlot));
  s->id = c->fn->next_slot++;
  // scalars get 8-byte slots: the register spill stores full 64-bit slots
  // and narrower accesses simply use the low bytes
  size = size ? size : 8;
  s->size = size < 8 ? 8 : size;
  s->align = align ? align : 8;
  s->name = name;
  vec_push(&c->fn->slots, s);
  return s;
}

static IRType ir_type_of(Type *t) {
  switch (t->kind) {
  case TY_BOOL: return IT_U8;
  case TY_I8: return IT_I8;
  case TY_I16: return IT_I16;
  case TY_I32: return IT_I32;
  case TY_I64: return IT_I64;
  case TY_U8: return IT_U8;
  case TY_U16: return IT_U16;
  case TY_U32: return IT_U32;
  case TY_U64: return IT_U64;
  case TY_USIZE: return IT_USIZE;
  case TY_ISIZE: return IT_I64;
  case TY_F32: return IT_F32;
  case TY_F64: return IT_F64;
  default: return IT_PTR;
  }
}

static IRVreg *new_vreg(LCtx *c, IRType ty) {
  IRVreg *v = arena_alloc(sizeof(IRVreg));
  v->id = c->fn->next_vreg++;
  v->ty = ty;
  v->def_slot = -1;
  return v;
}

static IRIns *emit(LCtx *c, IROp op) {
  IRIns *i = arena_alloc_zeroed(sizeof(IRIns));
  i->op = op;
  i->line = 0;
  vec_push(&c->fn->cur->ins, i);
  return i;
}

static IRBlock *new_block(LCtx *c) {
  IRBlock *b = arena_alloc_zeroed(sizeof(IRBlock));
  b->id = c->fn->next_block++;
  vec_push(&c->fn->blocks, b);
  return b;
}

static void use_block(LCtx *c, IRBlock *b) { c->fn->cur = b; }

static void emit_br(LCtx *c, IRBlock *to) {
  IRIns *t = arena_alloc_zeroed(sizeof(IRIns));
  t->op = (IROp)OP_BR;
  t->dst = (IRVreg *)to;
  vec_push(&c->fn->cur->ins, t);
  c->fn->cur->term = t;
  c->fn->cur->sealed = true;
}

static void emit_cbr(LCtx *c, IRVreg *cond, IRBlock *t, IRBlock *f) {
  IRIns *i = arena_alloc_zeroed(sizeof(IRIns));
  i->op = (IROp)OP_CBR;
  i->a = cond;
  i->dst = (IRVreg *)t;
  i->b = (IRVreg *)f;
  vec_push(&c->fn->cur->ins, i);
  c->fn->cur->term = i;
  c->fn->cur->sealed = true;
}

static void emit_ret(LCtx *c, IRVreg *v) {
  IRIns *t = arena_alloc_zeroed(sizeof(IRIns));
  t->op = (IROp)OP_RET;
  t->a = v;
  vec_push(&c->fn->cur->ins, t);
  c->fn->cur->term = t;
  c->fn->cur->sealed = true;
}

static IRPhi *emit_phi(LCtx *c, IRType ty) {
  IRPhi *p = arena_alloc_zeroed(sizeof(IRPhi));
  p->dst = new_vreg(c, ty);
  vec_push(&c->fn->cur->phis, p);
  return p;
}

// a phi lives in the block whose execution merges: the JOIN block
static IRPhi *emit_phi_in(LCtx *c, IRBlock *block, IRType ty) {
  IRBlock *saved = c->fn->cur;
  c->fn->cur = block;
  IRPhi *p = emit_phi(c, ty);
  c->fn->cur = saved;
  return p;
}

static void phi_add(IRPhi *p, IRBlock *pred, IRVreg *v) {
  vec_push(&p->preds, pred);
  vec_push(&p->args, v);
}

static int g_lit_counter = 0;

static int lit_bytes(LCtx *c, Str s) {
  IRLiteral *l = arena_alloc(sizeof(IRLiteral));
  l->label = arena_printf("str%d", g_lit_counter++);
  for (size_t i = 0; i < s.n; i++)
    vec_push(&l->bytes, (void *)(long)(unsigned char)s.p[i]);
  vec_push(&l->bytes, (void *)0L); // NUL pad for C interop debugging
  vec_push(&c->fn->literals, l);
  return (int)c->fn->literals.n - 1;
}

static IRVreg *v_const(LCtx *c, uint64_t v, IRType ty) {
  IRIns *i = emit(c, IR_CONST);
  i->dst = new_vreg(c, ty);
  i->imm = (int64_t)v;
  return i->dst;
}

static IRVreg *v_slotaddr(LCtx *c, IRSlot *s) {
  IRIns *i = emit(c, IR_ADDRC);
  i->dst = new_vreg(c, IT_PTR);
  i->slot = s;
  return i->dst;
}

static IRVreg *v_load(LCtx *c, IRVreg *addr, IRType ty) {
  IRIns *i = emit(c, IR_LOAD);
  i->dst = new_vreg(c, ty);
  i->addr = addr;
  i->size = ir_size_of(ty);
  i->is_float = ir_is_float(ty);
  return i->dst;
}

static void v_store(LCtx *c, IRVreg *addr, IRVreg *v) {
  IRIns *i = emit(c, IR_STORE);
  i->addr = addr;
  i->a = v;
  i->size = ir_size_of(v->ty);
  i->is_float = ir_is_float(v->ty);
}

static IRVreg *v_binop(LCtx *c, IROp op, IRVreg *a, IRVreg *b, IRType ty, bool signed_ops) {
  IRIns *i = emit(c, op);
  i->dst = new_vreg(c, ty);
  i->a = a;
  i->b = b;
  i->is_float = ir_is_float(ty);
  i->signed_ops = signed_ops;
  return i->dst;
}

static IRVreg *v_cmp(LCtx *c, IRCC cc, IRVreg *a, IRVreg *b, bool is_float) {
  IRIns *i = emit(c, IR_CMP);
  i->dst = new_vreg(c, IT_U8);
  i->a = a;
  i->b = b;
  i->cc = cc;
  i->is_float = is_float;
  i->signed_ops = true;
  return i->dst;
}

static IRVreg *v_addi(LCtx *c, IRVreg *a, int64_t imm) {
  IRIns *i = emit(c, IR_ADDI);
  i->dst = new_vreg(c, IT_PTR);
  i->a = a;
  i->imm = imm;
  return i->dst;
}

static IRVreg *v_cast(LCtx *c, IRVreg *src, IRType to) {
  IRIns *i = emit(c, IR_CAST);
  i->dst = new_vreg(c, to);
  i->a = src;
  i->cast_from = src->ty;
  i->cast_to = to;
  // classify
  if (ir_is_float(src->ty) && ir_is_float(to))
    i->cast = src->ty == IT_F32 && to == IT_F64 ? CAST_F32_F64 : CAST_F64_F32;
  else if (ir_is_float(src->ty))
    i->cast = CAST_F2I;
  else if (ir_is_float(to))
    i->cast = CAST_I2F;
  else if (ir_size_of(src->ty) > ir_size_of(to))
    i->cast = CAST_TRUNC;
  else if (ir_size_of(src->ty) < ir_size_of(to))
    i->cast = ty_is_signed_int(src->ty) ? CAST_SEXT : CAST_ZEXT;
  else
    i->cast = CAST_BITCOPY;
  return i->dst;
}

// the aggregate-return destination: the out slot holds the caller's buffer
// pointer — load through it, never write into the slot itself
static IRVreg *ret_dest(LCtx *c) {
  return v_load(c, v_slotaddr(c, c->fn->out_slot), IT_PTR);
}

// ------------------------------------------------------------ scopes -------

static void scope_push(LCtx *c) {
  LScope *s = arena_alloc_zeroed(sizeof(LScope));
  s->parent = c->scope;
  c->scope = s;
}

static void scope_pop(LCtx *c) { c->scope = c->scope->parent; }

static void bind(LCtx *c, Str name, IRSlot *slot) {
  map_put(&c->scope->bindings, name, slot);
}

static IRSlot *lookup_slot(LCtx *c, Str name) {
  for (LScope *s = c->scope; s; s = s->parent) {
    IRSlot *slot = map_get(&s->bindings, name);
    if (slot)
      return slot;
  }
  return NULL;
}

// emit defer calls from innermost scope down to (exclusive) `stop`
static void run_defers(LCtx *c, LScope *stop);

// ------------------------------------------------------- panic plumbing ----

static void panic_call(LCtx *c, const char *msg) {
  Str m = str_from(msg);
  int lit = lit_bytes(c, m);
  IRSlot *tmp = new_slot(c, 24, 8, "panicstr");
  IRVreg *addr = v_slotaddr(c, tmp);
  IRIns *la = emit(c, IR_LITADDR);
  la->dst = new_vreg(c, IT_PTR);
  la->lit = lit;
  IRVreg *ptrv = la->dst;
  // slice temp: {buf, ptr, len}
  IRVreg *addr8 = v_addi(c, addr, 8);
  IRVreg *addr16 = v_addi(c, addr, 16);
  IRVreg *lenv = v_const(c, (uint64_t)m.n, IT_USIZE);
  IRIns *st1 = emit(c, IR_STORE);
  st1->addr = addr;
  st1->a = ptrv;
  st1->size = 8;
  IRIns *st2 = emit(c, IR_STORE);
  st2->addr = addr8;
  st2->a = ptrv;
  st2->size = 8;
  IRIns *st3 = emit(c, IR_STORE);
  st3->addr = addr16;
  st3->a = lenv;
  st3->size = 8;
  // call prelude panic(msg)
  Sym *panic_fn = prelude_fn("panic");
  IRIns *call = emit(c, IR_CALL);
  call->callee = sym_symbol(panic_fn);
  IRArg *a = arena_alloc(sizeof(IRArg));
  a->vreg = addr;
  a->ty = NULL;
  vec_push(&call->args, a);
  // panic never returns; keep the block unsealed — a br follows at call site
  (void)st1;
  (void)st2;
  (void)st3;
}

// ------------------------------------------------------------ null check ---

static IRVreg *null_check(LCtx *c, IRVreg *ptr) {
  IRBlock *ok = new_block(c);
  IRBlock *bad = new_block(c);
  IRVreg *isnull = v_cmp(c, CC_EQ, ptr, v_const(c, 0, IT_PTR), false);
  emit_cbr(c, isnull, bad, ok);
  use_block(c, bad);
  panic_call(c, "null dereference");
  emit_br(c, ok); // unreachable, keeps the CFG well-formed
  use_block(c, ok);
  return ptr;
}

// bounds check: idx < len, unsigned
static void bounds_check(LCtx *c, IRVreg *idx, IRVreg *len) {
  IRBlock *ok = new_block(c);
  IRBlock *bad = new_block(c);
  IRVreg *oob = v_cmp(c, CC_GE, idx, len, false); // unsigned via emitter flag
  emit_cbr(c, oob, bad, ok);
  use_block(c, bad);
  panic_call(c, "index out of bounds");
  emit_br(c, ok);
  use_block(c, ok);
}

// slice upper bound: hi <= len is legal
static void bounds_check_hi(LCtx *c, IRVreg *hi, IRVreg *len) {
  IRBlock *ok = new_block(c);
  IRBlock *bad = new_block(c);
  IRVreg *oob = v_cmp(c, CC_GT, hi, len, false);
  emit_cbr(c, oob, bad, ok);
  use_block(c, bad);
  panic_call(c, "index out of bounds");
  emit_br(c, ok);
  use_block(c, ok);
}

// slice expression a[lo..hi]: materialize {buf, ptr+lo, hi-lo} into a temp
static IRVreg *lower_slice(LCtx *c, Expr *e) {
  Type *bt = e->a->typed;
  IRVreg *base = lv_expr(c, e->a); // aggregate: address of slice/array
  IRVreg *lo = lv_expr(c, e->b);
  IRVreg *hi = lv_expr(c, e->c);
  if (lo->ty != IT_USIZE)
    lo = v_cast(c, lo, IT_USIZE);
  if (hi->ty != IT_USIZE)
    hi = v_cast(c, hi, IT_USIZE);
  IRVreg *len;
  if (bt->kind == TY_ARRAY)
    len = v_const(c, (uint64_t)bt->len, IT_USIZE);
  else
    len = v_load(c, v_addi(c, base, 16), IT_USIZE);
  bounds_check(c, lo, len);
  bounds_check_hi(c, hi, len);
  IRVreg *buf, *ptr;
  if (bt->kind == TY_ARRAY) {
    buf = compute_addr(c, e->a); // array address as buffer
    ptr = buf;
  } else {
    buf = v_load(c, base, IT_PTR);
    ptr = v_load(c, v_addi(c, base, 8), IT_PTR);
  }
  int64_t esize = bt->kind == TY_STRING ? 1 : type_size(bt->elem);
  IRVreg *scaled;
  IRVreg *esz = v_const(c, (uint64_t)esize, IT_USIZE);
  IRIns *mul = emit(c, IR_MUL);
  mul->dst = new_vreg(c, IT_USIZE);
  mul->a = lo;
  mul->b = esz;
  scaled = mul->dst;
  IRIns *pa = emit(c, IR_ADD);
  pa->dst = new_vreg(c, IT_PTR);
  pa->a = ptr;
  pa->b = scaled;
  IRIns *sub = emit(c, IR_SUB);
  sub->dst = new_vreg(c, IT_USIZE);
  sub->a = hi;
  sub->b = lo;
  IRVreg *nlen = sub->dst;
  IRSlot *tmp = new_slot(c, 24, 8, "sliceval");
  IRVreg *dest = v_slotaddr(c, tmp);
  IRVreg *d8 = v_addi(c, dest, 8);
  IRVreg *d16 = v_addi(c, dest, 16);
  IRIns *s1 = emit(c, IR_STORE);
  s1->addr = dest;
  s1->a = buf;
  s1->size = 8;
  IRIns *s2 = emit(c, IR_STORE);
  s2->addr = d8;
  s2->a = pa->dst;
  s2->size = 8;
  IRIns *s3 = emit(c, IR_STORE);
  s3->addr = d16;
  s3->a = nlen;
  s3->size = 8;
  return dest;
}

static Sym *prelude_fn(const char *name);

// string equality: both operands lowered as slice addresses; calls prelude
static IRVreg *call_str_cmp(LCtx *c, Expr *e, bool want_eq) {
  IRVreg *a = lv_expr(c, e->a);
  IRVreg *b = lv_expr(c, e->b);
  Sym *fn = prelude_fn("__streq");
  IRIns *call = emit(c, IR_CALL);
  call->callee = sym_symbol(fn);
  IRArg *aa = arena_alloc(sizeof(IRArg));
  aa->vreg = a;
  aa->ty = NULL;
  vec_push(&call->args, aa);
  IRArg *ab = arena_alloc(sizeof(IRArg));
  ab->vreg = b;
  ab->ty = NULL;
  vec_push(&call->args, ab);
  call->dst = new_vreg(c, IT_U8);
  if (!want_eq) {
    IRVreg *one = v_const(c, 1, IT_U8);
    // negate via XOR 1
    IRIns *x = emit(c, IR_XOR);
    x->dst = new_vreg(c, IT_U8);
    x->a = call->dst;
    x->b = one;
    return x->dst;
  }
  return call->dst;
}

// ------------------------------------------------------------ aggregates ---

static bool ty_is_aggregate(Type *t) {
  switch (t->kind) {
  case TY_STRUCT: case TY_ENUM: case TY_ARRAY: case TY_SLICE: case TY_STRING:
    return true;
  default:
    return false;
  }
}

// ------------------------------------------------------------- lvalues -----

static IRVreg *compute_addr(LCtx *c, Expr *e) {
  switch (e->kind) {
  case EX_STR:
    // string literal used in address context: materialize its slice temp
    return lv_expr(c, e);
  case EX_NAME: {
    Sym *sym = e->sym;
    IRSlot *slot = lookup_slot(c, e->sv);
    if (slot)
      return v_slotaddr(c, slot);
    if (sym && (sym->kind == SY_STATIC)) {
      IRIns *i = emit(c, IR_ADDRC);
      i->dst = new_vreg(c, IT_PTR);
      i->callee = sym->symbol; // reuse: global symbol address
      i->lit = -1;             // marks global-address form
      return i->dst;
    }
    if (sym && sym->kind == SY_CONST) {
      // constants have no storage; caller only hits this for aggregates
      panic_call(c, "internal: constant has no address");
      return v_const(c, 0, IT_PTR);
    }
    panic_call(c, "internal: unbound name");
    return v_const(c, 0, IT_PTR);
  }
  case EX_FIELD: {
    Type *bt = e->a->typed;
    // slice/string built-in fields: {buf, ptr, len}
    Type *value_t = bt;
    bool through_ptr = false;
    if (value_t && value_t->kind == TY_PTR) {
      value_t = value_t->elem;
      through_ptr = true;
    }
    if (value_t && (value_t->kind == TY_SLICE || value_t->kind == TY_STRING)) {
      int64_t off;
      if (str_eq_c(e->sv, "len"))
        off = 16;
      else if (str_eq_c(e->sv, "ptr"))
        off = 8;
      else {
        panic_call(c, "internal: bad slice field");
        return v_const(c, 0, IT_PTR);
      }
      if (through_ptr)
        return v_addi(c, null_check(c, lv_expr(c, e->a)), off);
      return v_addi(c, lv_expr(c, e->a), off);
    }
    if (bt && bt->kind == TY_PTR) {
      IRVreg *base = null_check(c, lv_expr(c, e->a));
      return v_addi(c, base, struct_field_offset(bt->elem->rec, field_index_of(e, bt->elem->rec)));
    }
    IRVreg *base = compute_addr(c, e->a);
    return v_addi(c, base, struct_field_offset(bt->rec, field_index_of(e, bt->rec)));
  }
  case EX_INDEX: {
    Type *bt = e->a->typed;
    IRVreg *idx = lv_expr(c, e->b);
    IRVreg *baseaddr;
    int64_t esize = bt->kind == TY_STRING ? 1 : type_size(bt->elem);
    if (bt->kind == TY_ARRAY) {
      baseaddr = compute_addr(c, e->a);
      bounds_check(c, idx, v_const(c, (uint64_t)bt->len, IT_USIZE));
    } else {
      // slice: {buf, ptr, len}
      IRVreg *sliceaddr = compute_addr(c, e->a);
      IRVreg *len = v_load(c, v_addi(c, sliceaddr, 16), IT_USIZE);
      bounds_check(c, idx, len);
      IRVreg *ptr = v_load(c, v_addi(c, sliceaddr, 8), IT_PTR);
      baseaddr = ptr;
    }
    IRVreg *esz = v_const(c, (uint64_t)esize, IT_USIZE);
    IRIns *mul = emit(c, IR_MUL);
    mul->dst = new_vreg(c, IT_USIZE);
    mul->a = idx;
    mul->b = esz;
    IRIns *add = emit(c, IR_ADD);
    add->dst = new_vreg(c, IT_PTR);
    add->a = baseaddr;
    add->b = mul->dst;
    return add->dst;
  }
  case EX_UN: // *ptr
    return null_check(c, lv_expr(c, e->a));
  default:
    panic_call(c, "internal: not an lvalue");
    return v_const(c, 0, IT_PTR);
  }
}

// ------------------------------------------------------------ expressions --

static IRVreg *lv_expr(LCtx *c, Expr *e) {
  Type *t = e->typed;
  switch (e->kind) {
  case EX_INT:
    return v_const(c, e->iv, ir_type_of(t));
  case EX_FLOAT:
    return fconst(c, e->fv, ir_type_of(t));
  case EX_BOOL:
    return v_const(c, e->bv ? 1 : 0, IT_U8);
  case EX_NULL:
    return v_const(c, 0, IT_PTR);
  case EX_NAME: {
    Sym *sym = e->sym;
    if (sym && sym->kind == SY_CONST) {
      CV *memo = sym->decl->ceval_cache;
      if (memo && memo->ok && memo->is_int)
        return v_const(c, memo->i, ir_type_of(t));
      if (memo && memo->ok && !memo->is_int && t &&
          (t->kind == TY_F32 || t->kind == TY_F64))
        return fconst(c, memo->f, ir_type_of(t));
      if (t && ty_is_aggregate(t))
        return compute_addr(c, e);
      return v_const(c, 0, ir_type_of(t));
    }
    if (t && ty_is_aggregate(t))
      return compute_addr(c, e);
    IRSlot *slot = lookup_slot(c, e->sv);
    if (!slot) {
      if (sym && sym->kind == SY_STATIC) {
        IRIns *i = emit(c, IR_ADDRC);
        i->dst = new_vreg(c, IT_PTR);
        i->callee = sym->symbol;
        i->lit = -1;
        return v_load(c, i->dst, ir_type_of(t));
      }
      return v_const(c, 0, ir_type_of(t));
    }
    return v_load(c, v_slotaddr(c, slot), ir_type_of(t));
  }
  case EX_STR: {
    // slice value: temp {lit, lit, len}
    IRSlot *tmp = new_slot(c, 24, 8, "strlit");
    IRVreg *addr = v_slotaddr(c, tmp);
    int lit = lit_bytes(c, e->sv);
    IRIns *la = emit(c, IR_LITADDR);
    la->dst = new_vreg(c, IT_PTR);
    la->lit = lit;
    IRVreg *a8 = v_addi(c, addr, 8);
    IRVreg *a16 = v_addi(c, addr, 16);
    IRVreg *lenv = v_const(c, (uint64_t)e->sv.n, IT_USIZE);
    IRIns *s1 = emit(c, IR_STORE);
    s1->addr = addr;
    s1->a = la->dst;
    s1->size = 8;
    IRIns *s2 = emit(c, IR_STORE);
    s2->addr = a8;
    s2->a = la->dst;
    s2->size = 8;
    IRIns *s3 = emit(c, IR_STORE);
    s3->addr = a16;
    s3->a = lenv;
    s3->size = 8;
    return addr;
  }
  case EX_UN: {
    if (e->unop == P_STAR) {
      IRVreg *p = null_check(c, lv_expr(c, e->a));
      Type *pointee = e->typed;
      if (pointee && ty_is_aggregate(pointee))
        return p;
      return v_load(c, p, ir_type_of(pointee));
    }
    IRVreg *a = lv_expr(c, e->a);
    IRType ty = ir_type_of(t);
    if (e->unop == P_BANG)
      return v_cmp(c, CC_EQ, a, v_const(c, 0, a->ty), false);
    if (e->unop == P_TILDE)
      return v_binop(c, IR_XOR, a, v_const(c, ~(uint64_t)0, a->ty), a->ty, false);
    if (e->unop == P_MINUS) {
      if (ir_is_float(a->ty)) {
        IRVreg *zero = v_const(c, 0, a->ty);
        IRIns *i = emit(c, IR_SUB);
        i->dst = new_vreg(c, a->ty);
        i->a = zero;
        i->b = a;
        i->is_float = true;
        i->signed_ops = true;
        return i->dst;
      }
      IRVreg *zero = v_const(c, 0, a->ty);
      return v_binop(c, IR_SUB, zero, a, a->ty, true);
    }
    return v_const(c, 0, ty);
  }
  case EX_CAST: {
    Type *src_t = e->a->typed;
    if (src_t->kind == TY_ENUM) {
      IRVreg *addr = lv_expr(c, e->a); // aggregate: address
      IRVreg *tag = v_load(c, addr, IT_I32);
      return v_cast(c, tag, ir_type_of(t));
    }
    IRVreg *a = lv_expr(c, e->a);
    return v_cast(c, a, ir_type_of(t));
  }
  case EX_BIN: {
    Tok op = e->binop;
    if (op == P_ANDAND || op == P_OROR) {
      IRBlock *rhs = new_block(c), *join = new_block(c);
      IRPhi *phi = emit_phi_in(c, join, IT_U8);
      IRBlock *lhs_block = c->fn->cur;
      IRVreg *l = lv_expr(c, e->a);
      IRVreg *l_as_u8 = v_cast(c, l, IT_U8);
      if (op == P_ANDAND)
        emit_cbr(c, l, rhs, join);
      else
        emit_cbr(c, l, join, rhs);
      use_block(c, rhs);
      IRVreg *r = lv_expr(c, e->b);
      IRVreg *r_as_u8 = v_cast(c, r, IT_U8);
      IRBlock *rhs_block = c->fn->cur;
      emit_br(c, join);
      use_block(c, join);
      phi_add(phi, lhs_block, op == P_ANDAND ? v_const(c, 0, IT_U8) : v_const(c, 1, IT_U8));
      phi_add(phi, rhs_block, r_as_u8);
      (void)l_as_u8;
      return phi->dst;
    }
    IRVreg *a = lv_expr(c, e->a);
    IRVreg *b = lv_expr(c, e->b);
    IRType ty = ir_type_of(t);
    bool flt = ir_is_float(a->ty);
    switch (op) {
    case P_PLUS: return v_binop(c, IR_ADD, a, b, a->ty, true);
    case P_MINUS: return v_binop(c, IR_SUB, a, b, a->ty, true);
    case P_STAR: return v_binop(c, IR_MUL, a, b, a->ty, flt);
    case P_SLASH: return v_binop(c, IR_DIV, a, b, a->ty, flt || ty_is_signed_int(a->ty));
    case P_PERCENT: return v_binop(c, IR_MOD, a, b, a->ty, ty_is_signed_int(a->ty));
    case P_AMP: return v_binop(c, IR_AND, a, b, a->ty, false);
    case P_PIPE: return v_binop(c, IR_OR, a, b, a->ty, false);
    case P_CARET: return v_binop(c, IR_XOR, a, b, a->ty, false);
    case P_SHL: return v_binop(c, IR_SHL, a, b, a->ty, false);
    case P_SHR: return v_binop(c, IR_SHR, a, b, a->ty, ty_is_signed_int(a->ty));
    case P_EQ:
      if (e->a->typed && ((Type *)e->a->typed)->kind == TY_STRING)
        return call_str_cmp(c, e, true);
      return v_cmp(c, CC_EQ, a, b, flt);
    case P_NE:
      if (e->a->typed && ((Type *)e->a->typed)->kind == TY_STRING)
        return call_str_cmp(c, e, false);
      return v_cmp(c, CC_NE, a, b, flt);
    case P_LT: return v_cmp(c, CC_LT, a, b, flt);
    case P_LE: return v_cmp(c, CC_LE, a, b, flt);
    case P_GT: return v_cmp(c, CC_GT, a, b, flt);
    case P_GE: return v_cmp(c, CC_GE, a, b, flt);
    default:
      return v_const(c, 0, ty);
    }
  }
  case EX_SLICE:
    return lower_slice(c, e);
  case EX_INDEX: {
    Type *t2 = e->typed;
    if (t2 && ty_is_aggregate(t2))
      return compute_addr(c, e);
    return v_load(c, compute_addr(c, e), ir_type_of(t2));
  }
  case EX_FIELD: {
    Type *t2 = e->typed;
    // unit enum variant used as a value: materialize {tag, zeros}
    if (e->sym && ((Sym *)e->sym)->kind == SY_VARIANT) {
      Sym *vs = (Sym *)e->sym;
      Decl *ed = vs->decl;
      uint64_t disc = ((VariantAst *)ed->variants.items[vs->variant_index])->disc;
      IRSlot *tmp = new_slot(c, type_size(t2), type_align(t2), "enumval");
      IRVreg *dest = v_slotaddr(c, tmp);
      IRVreg *tagv = v_const(c, disc, IT_I32);
      IRVreg *a4 = v_addi(c, dest, 4);
      int64_t rest = type_size(t2) - 4;
      if (rest < 0)
        rest = 0;
      IRIns *tag = emit(c, IR_STORE);
      tag->addr = dest;
      tag->a = tagv;
      tag->size = 4;
      if (rest > 0) {
        IRIns *z = emit(c, IR_ZERO);
        z->addr = a4;
        z->size = rest;
      }
      return dest;
    }
    if (t2 && ty_is_aggregate(t2))
      return compute_addr(c, e);
    return v_load(c, compute_addr(c, e), ir_type_of(t2));
  }
  case EX_CALL:
    if (is_make_call(e))
      return lower_make(c, e, NULL);
    return compute_call_value(c, e);
  case EX_NEW:
    return lower_new(c, e);
  case EX_MAKE:
    if (t && ty_is_aggregate(t))
      return compute_addr(c, e);
    return compute_call_value(c, e);
  case EX_IF: {
    if (!t || t->kind == TY_VOID) {
      // statement context: no value to merge
      IRBlock *then_b = new_block(c);
      IRBlock *join = new_block(c);
      IRBlock *false_b = e->items.n > 1 ? new_block(c) : join;
      IRVreg *cond = lv_expr(c, e->a);
      emit_cbr(c, cond, then_b, false_b);
      use_block(c, then_b);
      lv_stmts(c, &((Stmt *)e->items.items[0])->stmts);
      emit_br(c, join);
      if (e->items.n > 1) {
        use_block(c, false_b);
        void *els = e->items.items[1];
        Expr *else_if = els;
        if (else_if->kind == EX_IF)
          lv_expr(c, else_if);
        else
          lv_stmts(c, &((Stmt *)els)->stmts);
        emit_br(c, join);
      }
      use_block(c, join);
      return v_const(c, 0, IT_U8);
    }
    IRBlock *then_b = new_block(c);
    IRBlock *join = new_block(c);
    IRBlock *false_b = e->items.n > 1 ? new_block(c) : join;
    IRPhi *phi = emit_phi_in(c, join, ir_type_of(t));
    IRBlock *cond_block = c->fn->cur;
    IRVreg *cond = lv_expr(c, e->a);
    emit_cbr(c, cond, then_b, false_b);
    use_block(c, then_b);
    Vec *then_stmts = &((Stmt *)e->items.items[0])->stmts;
    IRVreg *tv = block_value(c, then_stmts, t);
    IRBlock *then_end = c->fn->cur;
    emit_br(c, join);
    IRVreg *ev = NULL;
    IRBlock *else_end = NULL;
    if (e->items.n > 1) {
      use_block(c, false_b);
      void *els = e->items.items[1];
      Expr *else_if = els;
      if (else_if->kind == EX_IF)
        ev = lv_expr(c, else_if);
      else
        ev = block_value(c, &((Stmt *)els)->stmts, t);
      else_end = c->fn->cur;
      emit_br(c, join);
      use_block(c, join);
    } else {
      use_block(c, join);
    }
    phi_add(phi, then_end, tv);
    if (e->items.n > 1)
      phi_add(phi, else_end, ev);
    (void)ev;
    return phi->dst;
  }
  case EX_MATCH: {
    // chain of comparisons; arm values merge at a phi
    IRBlock *join = new_block(c);
    IRPhi *phi = emit_phi_in(c, join, ir_type_of(t));
    Type *scrut_t = e->a->typed;
    IRVreg *scrut = lv_expr(c, e->a);
    IRVreg *disc = NULL;
    if (scrut_t->kind == TY_ENUM)
      disc = v_load(c, scrut, IT_I32); // scrutinee addr (aggregate)
    size_t n = e->arms.n;
    for (size_t i = 0; i < n; i++) {
      MatchArm *arm = e->arms.items[i];
      IRBlock *body = new_block(c);
      IRBlock *next = (i + 1 < n) ? new_block(c) : join;
      if (arm->pk == PAT_WILDCARD) {
        emit_br(c, body);
      } else if (scrut_t->kind == TY_ENUM) {
        IRVreg *is = v_cmp(c, CC_EQ, disc, v_const(c, arm->disc, IT_I32), false);
        emit_cbr(c, is, body, next);
      } else {
        IRVreg *is = v_cmp(c, CC_EQ, scrut, v_const(c, arm->pat_int, scrut->ty), false);
        emit_cbr(c, is, body, next);
      }
      use_block(c, body);
      bool bind_scope = arm->bind_syms.n > 0;
      if (bind_scope) {
        // payload bindings: slots copied out of the scrutinee's payload area
        scope_push(c);
        for (size_t b = 0; b < arm->bind_syms.n; b++) {
          Sym *bs = arm->bind_syms.items[b];
          if (!bs)
            continue;
          Type *ft = bs->type;
          int fidx = arm->bind_fidx.n > b ? (int)(long)arm->bind_fidx.items[b] : (int)b;
          int64_t off = variant_field_offset(scrut_t, arm->variant_index, fidx);
          IRVreg *faddr = v_addi(c, scrut, off);
          IRSlot *slot = new_slot(c, type_size(ft), type_align(ft), str_to_c(bs->name));
          bind(c, bs->name, slot);
          IRVreg *saddr = v_slotaddr(c, slot);
          if (ty_is_aggregate(ft)) {
            IRIns *cp = emit(c, IR_COPYMEM);
            cp->addr = saddr;
            cp->a = faddr;
            cp->size = type_size(ft);
          } else {
            v_store(c, saddr, v_load(c, faddr, ir_type_of(ft)));
          }
        }
      }
      IRVreg *val;
      if (arm->body->kind == EX_BLOCK) {
        scope_push(c);
        val = block_value(c, &arm->body->items, t);
        run_defers(c, c->scope->parent);
        scope_pop(c);
      } else {
        val = lv_expr(c, arm->body);
      }
      if (bind_scope)
        scope_pop(c);
      emit_br(c, join);
      phi_add(phi, c->fn->cur, val);
      if (i + 1 < n)
        use_block(c, next);
    }
    use_block(c, join);
    return phi->dst;
  }
  case EX_QMARK: {
    // `expr?` — on failure, return the whole value unchanged (it already
    // carries the failing tag and payload); on success, yield the payload
    Type *et = e->a->typed;
    IRVreg *val = lv_expr(c, e->a);
    IRVreg *tag = v_load(c, val, IT_I32);
    VariantAst *okv =
        ((Decl *)et->rec->decl)->variants.items[e->q_ok];
    IRVreg *is_ok = v_cmp(c, CC_EQ, tag, v_const(c, okv->disc, IT_I32), false);
    IRBlock *cont = new_block(c);
    IRBlock *prop = new_block(c);
    emit_cbr(c, is_ok, cont, prop);
    use_block(c, prop);
    run_defers(c, NULL);
    IRVreg *dst = ret_dest(c);
    IRIns *cp = emit(c, IR_COPYMEM);
    cp->addr = dst;
    cp->a = val;
    cp->size = type_size(et);
    emit_ret(c, NULL);
    IRBlock *dead = new_block(c);
    use_block(c, dead);
    emit_br(c, cont); // unreachable; keeps the CFG well-formed
    use_block(c, cont);
    Type *pt = e->typed;
    int64_t off = variant_field_offset(et, e->q_ok, 0);
    if (pt && ty_is_aggregate(pt))
      return v_addi(c, val, off);
    return v_load(c, v_addi(c, val, off), ir_type_of(pt));
  }
  default:
    if (t && ty_is_aggregate(t))
      return compute_addr(c, e);
    panic_call(c, "internal: unlowerable expression");
    return v_const(c, 0, IT_PTR);
  }
}

// ------------------------------------------------------------ calls --------

static IRVreg *fconst(LCtx *c, double v, IRType ty) {
  IRIns *i = emit(c, IR_FCONST);
  i->dst = new_vreg(c, ty);
  i->fimm = v;
  return i->dst;
}

static int field_index_of(Expr *e, RecType *rec) {
  Decl *d = rec->decl;
  for (size_t i = 0; i < d->fields.n; i++) {
    FieldAst *fa = d->fields.items[i];
    if (str_eq(fa->name, e->sv))
      return (int)i;
  }
  return 0;
}

static void build_call(LCtx *c, Sym *fn, Expr *receiver, Vec *args_exprs, IRVreg *out,
                       IRVreg **scalar_dst) {
  // args: vregs for scalars, addresses for aggregates; the method receiver,
  // when present, is argument 0
  Vec call_args = {0};
  Type *ret = fn->type->ret;
  bool agg_ret = ret && ty_is_aggregate(ret);
  if (agg_ret) {
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = out;
    a->ty = NULL;
    vec_push(&call_args, a);
  }
  size_t param_index = 0;
  if (receiver) {
    Type *self_t = fn->type->params.items[0];
    IRArg *a = arena_alloc(sizeof(IRArg));
    if (ty_is_aggregate(self_t))
      a->vreg = lv_expr(c, receiver);
    else
      a->vreg = lv_expr(c, receiver);
    a->ty = self_t;
    vec_push(&call_args, a);
    param_index = 1;
  }
  for (size_t i = 0; i < args_exprs->n; i++) {
    Expr *arg = args_exprs->items[i];
    Type *pt = fn->type->params.n > param_index + i
                   ? fn->type->params.items[param_index + i]
                   : (arg->typed ? arg->typed : NULL);
    IRArg *a = arena_alloc(sizeof(IRArg));
    if (pt && ty_is_aggregate(pt))
      a->vreg = lv_expr(c, arg); // aggregate rvalue = address of temp
    else
      a->vreg = lv_expr(c, arg);
    a->ty = pt;
    vec_push(&call_args, a);
  }
  IRIns *call = emit(c, IR_CALL);
  call->callee = sym_symbol(fn);
  call->args = call_args;
  if (ret && !agg_ret && ret->kind != TY_VOID)
    call->dst = new_vreg(c, ir_type_of(ret));
  *scalar_dst = call->dst;
}

static Sym *prelude_fn(const char *name) {
  Module *p = g_prelude_module();
  if (!p)
    return NULL;
  return map_get(&p->syms, str_from(name));
}

static IRVreg *call_prelude1(LCtx *c, const char *name, IRVreg *arg0, IRType ret) {
  Sym *fn = prelude_fn(name);
  IRIns *call = emit(c, IR_CALL);
  call->callee = sym_symbol(fn);
  if (arg0) {
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = arg0;
    a->ty = NULL;
    vec_push(&call->args, a);
  }
  if (ret != IT_PTR || 1)
    call->dst = new_vreg(c, ret);
  return call->dst;
}

// make([]T, n): allocate len*esize + header, init slice {buf, ptr, len}
static IRVreg *lower_make(LCtx *c, Expr *e, IRVreg *dest) {
  Type *slice_t = e->typed;
  uint64_t esize = (uint64_t)type_size(slice_t->elem);
  IRVreg *n = lv_expr(c, e->args.items[1]);
  IRVreg *esz = v_const(c, esize, IT_USIZE);
  IRVreg *hdr = v_const(c, 24, IT_USIZE);
  IRIns *mul = emit(c, IR_MUL);
  mul->dst = new_vreg(c, IT_USIZE);
  mul->a = n;
  mul->b = esz;
  IRIns *add = emit(c, IR_ADD);
  add->dst = new_vreg(c, IT_USIZE);
  add->a = mul->dst;
  add->b = hdr;
  IRVreg *obj = call_prelude1(c, "__alloc", add->dst, IT_PTR);
  // header: {rc=1, wrc=0, drop=null}
  IRVreg *obj8 = v_addi(c, obj, 8);
  IRVreg *obj16 = v_addi(c, obj, 16);
  IRVreg *obj24 = v_addi(c, obj, 24);
  IRVreg *rc1 = v_const(c, 1, IT_USIZE);
  IRVreg *zero = v_const(c, 0, IT_USIZE);
  IRIns *rc = emit(c, IR_STORE);
  rc->addr = obj;
  rc->a = rc1;
  rc->size = 8;
  IRIns *wrc = emit(c, IR_STORE);
  wrc->addr = obj8;
  wrc->a = zero;
  wrc->size = 8;
  IRIns *dr = emit(c, IR_STORE);
  dr->addr = obj16;
  dr->a = v_const(c, 0, IT_PTR);
  dr->size = 8;
  // slice {buf=obj, ptr=obj+24, len=n}
  if (!dest) {
    IRSlot *tmp = new_slot(c, 24, 8, "slice");
    dest = v_slotaddr(c, tmp);
  }
  IRVreg *d8 = v_addi(c, dest, 8);
  IRVreg *d16 = v_addi(c, dest, 16);
  IRIns *s1 = emit(c, IR_STORE);
  s1->addr = dest;
  s1->a = obj;
  s1->size = 8;
  IRIns *s2 = emit(c, IR_STORE);
  s2->addr = d8;
  s2->a = obj24;
  s2->size = 8;
  IRIns *s3 = emit(c, IR_STORE);
  s3->addr = d16;
  s3->a = n;
  s3->size = 8;
  return dest;
}

// new T { fields }: allocate, header, init fields, return the pointer
static IRVreg *lower_new(LCtx *c, Expr *e) {
  Type *ptr_t = e->typed;
  Type *struct_t = ptr_t->elem;
  int64_t size = type_size(struct_t);
  IRVreg *obj = call_prelude1(c, "__alloc", v_const(c, (uint64_t)size + 24, IT_USIZE), IT_PTR);
  IRVreg *obj8 = v_addi(c, obj, 8);
  IRVreg *obj16 = v_addi(c, obj, 16);
  IRVreg *rc1 = v_const(c, 1, IT_USIZE);
  IRIns *rc = emit(c, IR_STORE);
  rc->addr = obj;
  rc->a = rc1;
  rc->size = 8;
  IRVreg *zero = v_const(c, 0, IT_USIZE);
  IRIns *wrc = emit(c, IR_STORE);
  wrc->addr = obj8;
  wrc->a = zero;
  wrc->size = 8;
  IRVreg *nullp = v_const(c, 0, IT_PTR);
  IRIns *dr = emit(c, IR_STORE);
  dr->addr = obj16;
  dr->a = nullp;
  dr->size = 8;
  // the returned reference points at the DATA, 24 bytes past the header
  IRVreg *data = v_addi(c, obj, 24);
  RecType *rec = struct_t->rec;
  Decl *sd = rec->decl;
  for (size_t i = 0; i < e->items.n; i++) {
    FieldAst *fa = e->items.items[i];
    Expr *init = e->args.items[i];
    for (size_t j = 0; j < sd->fields.n; j++) {
      if (str_eq(((FieldAst *)sd->fields.items[j])->name, fa->name)) {
        int64_t off = struct_field_offset(rec, j);
        Type *ft = struct_field_type(rec, j);
        IRVreg *faddr = v_addi(c, data, off);
        if (ty_is_aggregate(ft)) {
          lv_agg(c, init, faddr);
        } else {
          v_store(c, faddr, lv_expr(c, init));
        }
        break;
      }
    }
  }
  return data;
}

static bool is_make_call(Expr *e) {
  return e->kind == EX_CALL && e->a->kind == EX_NAME && str_eq_c(e->a->sv, "make") &&
         (Type *)e->typed && ((Type *)e->typed)->kind == TY_SLICE;
}

static Expr *method_receiver(Expr *callee) {
  if (callee->kind != EX_FIELD)
    return NULL;
  Type *bt = callee->a->typed;
  Sym *csym = callee->sym;
  if (!bt || bt->kind == TY_MODULE || !csym || !csym->decl)
    return NULL;
  return csym->decl->is_method ? callee->a : NULL;
}

static bool is_variant_ctor(Expr *e) {
  return e->kind == EX_CALL && e->a->kind == EX_FIELD && e->a->sym &&
         ((Sym *)e->a->sym)->kind == SY_VARIANT;
}

// enum variant construction: write {tag, payload fields} into dest (fresh
// temp when dest is NULL); returns the value's address
static IRVreg *lower_enum_ctor(LCtx *c, Expr *e, IRVreg *dest) {
  Sym *vs = e->a->sym;
  Type *et = e->typed;
  VariantAst *v = ((Decl *)vs->decl)->variants.items[vs->variant_index];
  if (!dest) {
    IRSlot *tmp = new_slot(c, type_size(et), type_align(et), "enumval");
    dest = v_slotaddr(c, tmp);
  }
  IRVreg *tagv = v_const(c, v->disc, IT_I32);
  IRIns *tag = emit(c, IR_STORE);
  tag->addr = dest;
  tag->a = tagv;
  tag->size = 4;
  int64_t size = type_size(et);
  if (size > 4) {
    IRVreg *rest = v_addi(c, dest, 4);
    IRIns *z = emit(c, IR_ZERO);
    z->addr = rest;
    z->size = size - 4;
  }
  size_t nfields =
      v->vkind == VAR_TUPLE ? v->types.n : v->vkind == VAR_STRUCT ? v->fields.n : 0;
  for (size_t i = 0; i < e->args.n && i < nfields; i++) {
    Expr *arg = e->args.items[i];
    int fidx = (int)i;
    if (v->vkind == VAR_STRUCT) {
      char *nm = e->arg_names.n > i ? e->arg_names.items[i] : NULL;
      fidx = -1;
      for (size_t j = 0; j < v->fields.n; j++)
        if (nm && str_eq_c(((FieldAst *)v->fields.items[j])->name, nm)) {
          fidx = (int)j;
          break;
        }
      if (fidx < 0)
        continue;
    }
    int64_t off = variant_field_offset(et, vs->variant_index, fidx);
    Type *ft = arg->typed;
    IRVreg *faddr = v_addi(c, dest, off);
    if (ft && ty_is_aggregate(ft))
      lv_agg(c, arg, faddr);
    else
      v_store(c, faddr, lv_expr(c, arg));
  }
  return dest;
}

static IRVreg *compute_call_value(LCtx *c, Expr *e) {
  // builtins
  if (e->a->kind == EX_NAME && str_eq_c(e->a->sv, "len")) {
    Expr *arg = e->args.items[0];
    Type *at = arg->typed;
    if (at->kind == TY_ARRAY)
      return v_const(c, at->len, IT_USIZE);
    IRVreg *addr = lv_expr(c, arg); // slice/string aggregate address
    return v_load(c, v_addi(c, addr, 16), IT_USIZE);
  }
  if (is_variant_ctor(e)) {
    Type *et = e->typed;
    IRSlot *tmp = new_slot(c, type_size(et), type_align(et), "enumval");
    return lower_enum_ctor(c, e, v_slotaddr(c, tmp));
  }
  // normal call: callee sym stashed by the checker
  Sym *fn = e->a->sym;
  if (!fn) {
    panic_call(c, "internal: unresolved callee");
    return v_const(c, 0, IT_PTR);
  }
  Type *ret = fn->type->ret;
  if (ret && ty_is_aggregate(ret)) {
    IRSlot *tmp = new_slot(c, type_size(ret), type_align(ret), "ret");
    IRVreg *out = v_slotaddr(c, tmp);
    IRVreg *dst;
    build_call(c, fn, method_receiver(e->a), &e->args, out, &dst);
    return out; // value address
  }
  IRVreg *dst;
  build_call(c, fn, method_receiver(e->a), &e->args, NULL, &dst);
  return dst;
}

// ------------------------------------------------------------ aggregates ---

// write aggregate value `e` into dest (address vreg)
static void lv_agg(LCtx *c, Expr *e, IRVreg *dest) {
  Type *t = e->typed;
  int64_t size = t ? type_size(t) : 0;
  switch (e->kind) {
  case EX_MAKE: {
    lower_make(c, e, dest);
    return;
  }
  case EX_STR: {
    IRVreg *addr = lv_expr(c, e); // lv_expr materializes into a temp
    IRIns *cp = emit(c, IR_COPYMEM);
    cp->addr = dest;
    cp->a = addr;
    cp->size = 24;
    return;
  }
  case EX_CALL: {
    if (is_make_call(e)) {
      lower_make(c, e, dest);
      return;
    }
    if (is_variant_ctor(e)) {
      lower_enum_ctor(c, e, dest);
      return;
    }
    Sym *fn = e->a->sym;
    if (fn && fn->type->ret && ty_is_aggregate(fn->type->ret)) {
      // call straight into dest
      Vec call_args = {0};
      IRArg *a0 = arena_alloc(sizeof(IRArg));
      a0->vreg = dest;
      a0->ty = NULL;
      vec_push(&call_args, a0);
      Expr *recv = method_receiver(e->a);
      size_t pi = 0;
      if (recv) {
        IRArg *a = arena_alloc(sizeof(IRArg));
        a->vreg = lv_expr(c, recv);
        a->ty = fn->type->params.items[0];
        vec_push(&call_args, a);
        pi = 1;
      }
      for (size_t i = 0; i < e->args.n; i++) {
        Expr *arg = e->args.items[i];
        Type *pt = fn->type->params.items[pi + i];
        IRArg *a = arena_alloc(sizeof(IRArg));
        a->vreg = lv_expr(c, arg);
        a->ty = pt;
        vec_push(&call_args, a);
      }
      IRIns *call = emit(c, IR_CALL);
      call->callee = sym_symbol(fn);
      call->args = call_args;
      return;
    }
    IRVreg *src = lv_expr(c, e);
    IRIns *cp = emit(c, IR_COPYMEM);
    cp->addr = dest;
    cp->a = src;
    cp->size = size;
    return;
  }
  case EX_IF:
  case EX_MATCH:
    panic_call(c, "internal: aggregate-valued if/match arrives in 0.0.5");
    return;
  default: {
    IRVreg *src = lv_expr(c, e); // aggregates evaluate to addresses
    IRIns *cp = emit(c, IR_COPYMEM);
    cp->addr = dest;
    cp->a = src;
    cp->size = size;
    return;
  }
  }
}

// block statements with a trailing value; returns value vreg (scalars) or
// address (aggregates); NULL when the block produces nothing
static IRVreg *block_value(LCtx *c, Vec *stmts, Type *t) {
  scope_push(c);
  IRVreg *tail = NULL;
  for (size_t i = 0; i < stmts->n; i++) {
    Stmt *s = stmts->items[i];
    lv_stmt(c, s);
    if (i + 1 == stmts->n && s->kind == ST_EXPR && s->tail) {
      if (s->a->typed && ty_is_aggregate(s->a->typed))
        tail = lv_expr(c, s->a);
      else
        tail = lv_expr(c, s->a);
    }
  }
  scope_pop(c);
  (void)t;
  return tail;
}

// ------------------------------------------------------------ statements ---

static void lv_expr_discard(LCtx *c, Expr *e) {
  Type *t = e->typed;
  if (t && ty_is_aggregate(t))
    lv_expr(c, e); // address materialized; value dropped (RC lands in 0.0.5)
  else
    lv_expr(c, e);
}

static void lv_stmt(LCtx *c, Stmt *s) {
  switch (s->kind) {
  case ST_LET: {
    Type *t = s->a->typed;
    IRSlot *slot = new_slot(c, type_size(t), type_align(t), str_to_c(s->name));
    bind(c, s->name, slot);
    IRVreg *addr = v_slotaddr(c, slot);
    if (ty_is_aggregate(t)) {
      lv_agg(c, s->a, addr);
    } else {
      IRVreg *v = lv_expr(c, s->a);
      v_store(c, addr, v);
    }
    break;
  }
  case ST_ASSIGN: {
    Type *t = s->a->typed;
    IRVreg *addr = compute_addr(c, s->a);
    if (s->assign_op == P_ASSIGN) {
      if (ty_is_aggregate(t)) {
        lv_agg(c, s->b, addr);
      } else {
        IRVreg *v = lv_expr(c, s->b);
        v_store(c, addr, v);
      }
    } else {
      IRType vt = ir_type_of(t);
      IRVreg *cur = v_load(c, addr, vt);
      IRVreg *rhs = lv_expr(c, s->b);
      IRVreg *res = NULL;
      bool flt = ir_is_float(vt);
      switch (s->assign_op) {
      case P_PLUSEQ: res = v_binop(c, IR_ADD, cur, rhs, vt, true); break;
      case P_MINUSEQ: res = v_binop(c, IR_SUB, cur, rhs, vt, true); break;
      case P_STAREQ: res = v_binop(c, IR_MUL, cur, rhs, vt, flt); break;
      case P_SLASHEQ: res = v_binop(c, IR_DIV, cur, rhs, vt, flt || ty_is_signed_int(vt)); break;
      case P_PERCENTEQ: res = v_binop(c, IR_MOD, cur, rhs, vt, ty_is_signed_int(vt)); break;
      case P_AMPEQ: res = v_binop(c, IR_AND, cur, rhs, vt, false); break;
      case P_PIPEEQ: res = v_binop(c, IR_OR, cur, rhs, vt, false); break;
      case P_CARETEQ: res = v_binop(c, IR_XOR, cur, rhs, vt, false); break;
      case P_SHLEQ: res = v_binop(c, IR_SHL, cur, rhs, vt, false); break;
      case P_SHREQ: res = v_binop(c, IR_SHR, cur, rhs, vt, ty_is_signed_int(vt)); break;
      default: res = cur;
      }
      v_store(c, addr, res);
    }
    break;
  }
  case ST_EXPR:
    lv_expr_discard(c, s->a);
    break;
  case ST_RETURN: {
    run_defers(c, NULL);
    Type *t = s->a ? s->a->typed : NULL;
    if (s->a && ty_is_aggregate(t)) {
      IRVreg *src = lv_expr(c, s->a);
      IRVreg *dst = ret_dest(c);
      IRIns *cp = emit(c, IR_COPYMEM);
      cp->addr = dst;
      cp->a = src;
      cp->size = type_size(t);
      emit_ret(c, NULL);
    } else {
      emit_ret(c, s->a ? lv_expr(c, s->a) : NULL);
    }
    // continuation block for unreachable code after return
    IRBlock *dead = new_block(c);
    use_block(c, dead);
    break;
  }
  case ST_BREAK:
  case ST_CONTINUE: {
    LScope *loop = NULL;
    for (LScope *s2 = c->scope; s2; s2 = s2->parent) {
      if ((s->kind == ST_BREAK && s2->break_to) || (s->kind == ST_CONTINUE && s2->continue_to)) {
        loop = s2;
        break;
      }
    }
    run_defers(c, loop); // defers inside the loop body, not the loop scope's own
    emit_br(c, s->kind == ST_BREAK ? loop->break_to : loop->continue_to);
    IRBlock *dead = new_block(c);
    use_block(c, dead);
    break;
  }
  case ST_DEFER: {
    vec_push(&c->scope->defers, s);
    break;
  }
  case ST_WHILE: {
    IRBlock *header = new_block(c);
    IRBlock *body = new_block(c);
    IRBlock *done = new_block(c);
    emit_br(c, header);
    use_block(c, header);
    IRVreg *cond = lv_expr(c, s->cond);
    emit_cbr(c, cond, body, done);
    use_block(c, body);
    scope_push(c);
    c->scope->break_to = done;
    c->scope->continue_to = header;
    for (size_t i = 0; i < s->body.n; i++)
      lv_stmt(c, s->body.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop(c);
    emit_br(c, header);
    use_block(c, done);
    break;
  }
  case ST_LOOP: {
    IRBlock *body = new_block(c);
    IRBlock *done = new_block(c);
    emit_br(c, body);
    use_block(c, body);
    scope_push(c);
    c->scope->break_to = done;
    c->scope->continue_to = body;
    for (size_t i = 0; i < s->body.n; i++)
      lv_stmt(c, s->body.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop(c);
    emit_br(c, body);
    use_block(c, done);
    break;
  }
  case ST_BLOCK: {
    scope_push(c);
    for (size_t i = 0; i < s->stmts.n; i++)
      lv_stmt(c, s->stmts.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop(c);
    break;
  }
  }
}

static void lv_stmts(LCtx *c, Vec *stmts) {
  for (size_t i = 0; i < stmts->n; i++)
    lv_stmt(c, stmts->items[i]);
}

// run defers innermost-first until (exclusive) scope `stop` (NULL = all)
static void run_defers(LCtx *c, LScope *stop) {
  for (LScope *s = c->scope; s && s != stop; s = s->parent) {
    for (size_t i = s->defers.n; i > 0; i--) {
      Stmt *d = s->defers.items[i - 1];
      for (size_t j = 0; j < d->stmts.n; j++)
        lv_stmt(c, d->stmts.items[j]);
    }
  }
}

// ------------------------------------------------------------ driver -------

static IRFn *lower_fn(Sym *sym) {
  Decl *d = sym->decl;
  IRFn *fn = arena_alloc_zeroed(sizeof(IRFn));
  fn->symbol = sym_symbol(sym);
  fn->ret = sym->type->ret;
  fn->next_vreg = 0;
  fn->next_block = 0;
  fn->next_slot = 0;
  if (fn->ret && ty_is_aggregate(fn->ret)) {
    fn->returns_aggregate = true;
    fn->out_slot = new_slot(&(LCtx){.fn = fn}, type_size(fn->ret), type_align(fn->ret), "out");
  }
  LCtx ctx = {0};
  ctx.fn = fn;
  scope_push(&ctx);

  fn->entry = new_block(&ctx);
  use_block(&ctx, fn->entry);

  for (size_t i = 0; i < d->params.n; i++) {
    Param *pa = d->params.items[i];
    Type *pt = sym->type->params.items[i];
    IRSlot *slot = new_slot(&ctx, type_size(pt), type_align(pt), str_to_c(pa->name));
    vec_push(&fn->params, slot);
    vec_push(&fn->param_types, pt);
    if (ty_is_aggregate_t(pt)) {
      // the register holds a POINTER to the caller's copy; give the parameter
      // a proper local slot and copy the value through after the spill
      IRSlot *local = new_slot(&ctx, type_size(pt), type_align(pt), str_to_c(pa->name));
      bind(&ctx, pa->name, local);
      IRVreg *src_addr = v_slotaddr(&ctx, slot);
      IRVreg *src = v_load(&ctx, src_addr, IT_PTR);
      IRVreg *dst = v_slotaddr(&ctx, local);
      IRIns *cp = emit(&ctx, IR_COPYMEM);
      cp->addr = dst;
      cp->a = src;
      cp->size = type_size(pt);
    } else {
      bind(&ctx, pa->name, slot);
    }
  }
  if (sym->owner == g_root_module && str_eq_c(sym->name, "main"))
    g_main_symbol = fn->symbol;

  for (size_t i = 0; i < d->body.n; i++)
    lv_stmt(&ctx, d->body.items[i]);

  if (!fn->cur->sealed) {
    run_defers(&ctx, NULL);
    emit_ret(&ctx, NULL);
  }
  scope_pop(&ctx);
  return fn;
}

static void lower_static(Sym *sym) {
  Decl *d = sym->decl;
  Type *t = sym->type;
  IRGlobal *g = arena_alloc_zeroed(sizeof(IRGlobal));
  g->symbol = sym_symbol(sym);
  g->size = type_size(t);
  g->align = type_align(t);
  if (d->kind == DK_STATIC && d->init && d->init->kind == EX_STR) {
    // {buf, ptr, len} with relocations into rodata
    Str sv = d->init->sv;
    char *label = arena_printf("%s_str", g->symbol);
    IRLiteral *l = arena_alloc(sizeof(IRLiteral));
    l->label = label;
    for (size_t i = 0; i < sv.n; i++)
      vec_push(&l->bytes, (void *)(long)(unsigned char)sv.p[i]);
    vec_push(&l->bytes, (void *)0L);
    g->relocs = arena_alloc(3 * sizeof(char *));
    g->relocs[0] = label;
    g->relocs[1] = label;
    g->relocs[2] = NULL;
    int64_t len = (int64_t)sv.n;
    for (int w = 0; w < 3; w++) {
      int64_t word = w == 2 ? len : 0;
      for (int b = 0; b < 8; b++)
        vec_push(&g->init, (void *)(long)((word >> (8 * b)) & 0xFF));
    }
  } else if (d->init && d->init->typed && !ty_is_aggregate(d->init->typed)) {
    CV *memo = d->ceval_cache;
    uint64_t v = memo && memo->ok && memo->is_int ? memo->i : 0;
    if (memo && memo->ok && !memo->is_int) {
      double f = memo->f;
      uint64_t bits = t && t->kind == TY_F32
                          ? (uint64_t)(float)f
                          : ({ uint64_t b; __builtin_memcpy(&b, &f, 8); b; });
      v = bits;
    }
    int64_t size = type_size(t);
    for (int64_t b = 0; b < size; b++)
      vec_push(&g->init, (void *)(long)((v >> (8 * b)) & 0xFF));
  }
  vec_push(&g_ir_globals, g);
}

void lower_program(void) {
  g_ir_fns = (Vec){0};
  g_ir_globals = (Vec){0};
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    for (size_t k = 0; k < m->syms.keys.n; k++) {
      Str *key = m->syms.keys.items[k];
      Sym *sym = map_get(&m->syms, *key);
      if (sym->kind == SY_FN && sym->decl && sym->decl->body.n && !sym->decl->templated)
        vec_push(&g_ir_fns, lower_fn(sym));
      else if (sym->kind == SY_STATIC)
        lower_static(sym);
    }
  }
}

void lower_dump_ir(void) {
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    printf("fn %s\n", fn->symbol);
    for (size_t bi = 0; bi < fn->blocks.n; bi++) {
      IRBlock *b = fn->blocks.items[bi];
      printf("  block %d\n", b->id);
      for (size_t pi = 0; pi < b->phis.n; pi++) {
        IRPhi *phi = b->phis.items[pi];
        printf("    phi v%d =\n", phi->dst->id);
        for (size_t k = 0; k < phi->preds.n; k++)
          printf("      [pred %d] v%d\n", ((IRBlock *)phi->preds.items[k])->id,
                 ((IRVreg *)phi->args.items[k])->id);
      }
      for (size_t ii = 0; ii < b->ins.n; ii++) {
        IRIns *ins = b->ins.items[ii];
        printf("    op=%d dst=v%d a=v%d b=v%d imm=%lld size=%lld flt=%d sgn=%d callee=%s\n",
               (int)ins->op, ins->dst ? ins->dst->id : -1, ins->a ? ins->a->id : -1,
               ins->b ? ins->b->id : -1, (long long)ins->imm, (long long)ins->size,
               (int)ins->is_float, (int)ins->signed_ops, ins->callee ? ins->callee : "-");
      }
      if (b->term)
        printf("    term op=%d dst(b)=%d a=v%d b(b)=%d\n", (int)b->term->op,
               b->term->dst ? ((IRBlock *)b->term->dst)->id : -1,
               b->term->a ? b->term->a->id : -1,
               b->term->b ? ((IRBlock *)b->term->b)->id : -1);
    }
  }
}

#include <stdlib.h>
void lower_dump_ir_if_requested(void) {
  if (getenv("RHO_DUMP_IR"))
    lower_dump_ir();
}
