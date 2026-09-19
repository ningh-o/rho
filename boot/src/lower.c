#include "ir.h"

// AST -> SSA IR. Scalars travel in virtual registers; aggregates (structs,
// enums, slices, strings, arrays) live in frame slots and are addressed.
// An aggregate rvalue is represented by the address of a temp slot holding
// it; calls pass aggregates by pointer to the caller's copy (our documented
// ABI deviation).

Vec g_ir_fns = {0};
Vec g_ir_globals = {0};
const char *g_main_symbol = NULL;

static void lower_drop_unreachable(void);
extern bool g_prelude_esp32; // set by the build driver: RV32 keep-set (below)

static Module *g_root_module;

void lower_set_root(Module *m) { g_root_module = m; }

// per-function lowering context
typedef struct OwnedBind {
  IRSlot *slot;
  Type *ty;
} OwnedBind;

typedef struct LScope {
  Map bindings;        // Str name -> IRSlot*
  Vec defers;          // Vec of Vec<Stmt*> (defer payloads, in order)
  Vec owned;           // OwnedBind*: bindings holding a +1 that must be
                       // released when this scope dies
  struct LScope *parent;
  IRBlock *break_to, *continue_to;
  bool is_loop;        // loop body scope: its owned bindings release only at
                       // the back edge; unwinding past an interrupted
                       // iteration must not release stale or unwritten slots
} LScope;

typedef struct LCtx {
  IRFn *fn;
  LScope *scope;
  Vec panic_strs; // IRLiteral idx reuse
} LCtx;

static IRVreg *lv_expr(LCtx *c, Expr *e);
static IRVreg *fconst(LCtx *c, double v, IRType ty);
static IRVreg *compute_call_value(LCtx *c, Expr *e);
static IRVreg *call_prelude1(LCtx *c, const char *name, IRVreg *arg0, IRType ret);
static const char *value_fn_for(Type *t, bool retain);
static void rc_call1(LCtx *c, const char *helper, IRVreg *arg);
static void rc_inc_v(LCtx *c, IRVreg *p);
static void rc_dec_v(LCtx *c, IRVreg *p);
static void rc_runtime_build(void);
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
Type *variant_field_type(Type *enum_t, int variant, int field);

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
  case TY_INT_LIT: return IT_I32;
  case TY_FLOAT_LIT: return IT_F64; // BISECT-A
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

// join: the block whose code follows the if (NULL where the join is only
// discoverable heuristically — the wasm emitter falls back then)
static void emit_cbr(LCtx *c, IRVreg *cond, IRBlock *t, IRBlock *f,
                     IRBlock *join) {
  IRIns *i = arena_alloc_zeroed(sizeof(IRIns));
  i->op = (IROp)OP_CBR;
  i->a = cond;
  i->dst = (IRVreg *)t;
  i->b = (IRVreg *)f;
  i->join_hint = join;
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
  // signedness follows the operand: usize/u64 counters with the top bit
  // set must compare unsigned
  i->signed_ops = ty_is_signed_int(a->ty);
  return i->dst;
}

// unsigned >= : rc plausibility guards compare counts, not signs
static IRVreg *v_cmp_uge(LCtx *c, IRVreg *a, IRVreg *b) {
  IRIns *i = emit(c, IR_CMP);
  i->dst = new_vreg(c, IT_U8);
  i->a = a;
  i->b = b;
  i->cc = CC_GE;
  i->is_float = false;
  i->signed_ops = false;
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
  else if (ir_size_of(src->ty) == ir_size_of(to) &&
           ir_is_float(src->ty) && !ir_is_float(to) &&
           (to == IT_U32 || to == IT_U64))
    i->cast = CAST_REINTERP; // exact bit move (f32_bits/f64_bits)
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

static int g_panic_site = 0;
static void panic_call(LCtx *c, const char *msg) {
  // stamp the function name + a per-site counter into the message: runtime
  // panics from generated checks are otherwise indistinguishable
  int site = g_panic_site++;
  if (getenv("RHO_PANIC_MAP"))
    fprintf(stderr, "SITE %d in %s: %s (frame slot base now %d vregs %d)\n", site, c->fn->symbol,
            msg, c->fn->next_slot, c->fn->next_vreg);
  Str m = str_from(arena_printf("[%s#%d] %s", c->fn->symbol, site, msg));
  int lit = lit_bytes(c, m);
  IRSlot *tmp = new_slot(c, 24, 8, "panicstr");
  IRVreg *addr = v_slotaddr(c, tmp);
  IRIns *la = emit(c, IR_LITADDR);
  la->dst = new_vreg(c, IT_PTR);
  la->lit = lit;
  IRVreg *ptrv = la->dst;
  // slice temp: {buf=header, bytes, len}
  IRVreg *bytes = v_addi(c, ptrv, 24);
  IRVreg *addr8 = v_addi(c, addr, 8);
  IRVreg *addr16 = v_addi(c, addr, 16);
  IRVreg *lenv = v_const(c, (uint64_t)m.n, IT_USIZE);
  IRIns *st1 = emit(c, IR_STORE);
  st1->addr = addr;
  st1->a = ptrv;
  st1->size = 8;
  IRIns *st2 = emit(c, IR_STORE);
  st2->addr = addr8;
  st2->a = bytes;
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
  emit_cbr(c, isnull, bad, ok, ok);
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
  emit_cbr(c, oob, bad, ok, ok);
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
  emit_cbr(c, oob, bad, ok, ok);
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
  case TY_FN: // closure pair {code, env}
    return true;
  default:
    return false;
  }
}

// ---------------------------------------------------- ownership rules ----
//
// An owning rvalue carries its +1 (new/make/call/closure, and match/if
// results which normalize their arms); everything else borrows. Bindings
// own: a let either takes the +1 or retains a borrowed value; scopes
// release what they own, innermost first, on every exit path.

static bool expr_owned(Expr *e) {
  if (!e || !e->typed)
    return false;
  switch (e->kind) {
  case EX_NEW:
  case EX_CLOSURE:
    return true;
  case EX_CALL:
    // weak.get() lends the object without taking a count: it is a borrow
    if (e->a->kind == EX_FIELD && str_eq_c(e->a->sv, "get") && e->a->a &&
        e->a->a->typed && ((Type *)e->a->a->typed)->kind == TY_WEAK)
      return false;
    return ty_is_managed(e->typed);
  case EX_MAKE:
  case EX_MATCH:
  case EX_IF:
    return ty_is_managed(e->typed);
  default:
    return false;
  }
}

static void own_slot(LCtx *c, IRSlot *slot, Type *t) {
  OwnedBind *ob = arena_alloc(sizeof(OwnedBind));
  ob->slot = slot;
  ob->ty = t;
  vec_push(&c->scope->owned, ob);
}

// retain/release the value living at addr through the per-type walkers
static void retain_addr(LCtx *c, Type *t, IRVreg *addr) {
  const char *fn = value_fn_for(t, true);
  if (fn)
    rc_call1(c, fn, addr);
}
static void release_addr(LCtx *c, Type *t, IRVreg *addr) {
  const char *fn = value_fn_for(t, false);
  if (fn)
    rc_call1(c, fn, addr);
}

static void release_scope_only(LCtx *c) {
  for (size_t i = c->scope->owned.n; i > 0; i--) {
    OwnedBind *ob = c->scope->owned.items[i - 1];
    release_addr(c, ob->ty, v_slotaddr(c, ob->slot));
  }
}

// scope exit: release owned bindings, then pop
static void scope_pop_release(LCtx *c) {
  release_scope_only(c);
  scope_pop(c);
}

// release every owned binding from the current scope down to (exclusive)
// `stop` — the rc twin of run_defers
static void release_scopes_to(LCtx *c, LScope *stop) {
  for (LScope *s = c->scope; s && s != stop; s = s->parent) {
    if (s->is_loop)
      continue; // loop bindings release only at the back edge; unwinding an
                // interrupted iteration would double-release or release junk
    for (size_t i = s->owned.n; i > 0; i--) {
      OwnedBind *ob = s->owned.items[i - 1];
      release_addr(c, ob->ty, v_slotaddr(c, ob->slot));
    }
  }
}

// retain a produced rvalue: aggregates are value addresses, scalars are
// plain pointer vregs
static void retain_or_inc(LCtx *c, Type *t, IRVreg *v) {
  if (ty_is_aggregate(t)) {
    retain_addr(c, t, v);
  } else if (t->kind == TY_PTR) {
    rc_inc_v(c, v);
  } else if (t->kind == TY_WEAK) {
    rc_call1(c, "rho__rc_winc", v);
  }
}

// a block arm's value is pre-owned when its trailing expression is an
// owning producer (or a nested if, which normalizes itself)
static bool arm_value_owned(Vec *stmts) {
  if (!stmts->n)
    return true; // void-ish; nothing to normalize
  Stmt *last = stmts->items[stmts->n - 1];
  if (last->kind != ST_EXPR || !last->tail || !last->a)
    return true;
  return last->a->kind == EX_IF || expr_owned(last->a);
}

// ------------------------------------------------------------ closures ----
//
// A closure value is {code: *fn, env: *u8}. Calling one always passes env as
// the final hidden argument, so closure bodies and shims share one ABI.
// Static fn references materialize as {shim, null}: the shim drops env and
// forwards. Boot-era captures are copies — immutable snapshots.

static int g_closure_counter;
static Map g_shims; // shim symbol -> built marker

static const char *shim_for(Sym *fn) {
  const char *shim_name = arena_printf("rho__shim_%s", sym_symbol(fn));
  if (map_has(&g_shims, str_from(shim_name)))
    return shim_name;
  map_put(&g_shims, str_from(shim_name), (void *)1);
  Type *ft = fn->type;
  Type *ret = ft->ret;
  bool agg_ret = ret && ty_is_aggregate(ret);

  IRFn *lf = arena_alloc_zeroed(sizeof(IRFn));
  lf->symbol = shim_name;
  lf->ret = ret;
  LCtx cc = {0};
  cc.fn = lf;
  if (agg_ret) {
    lf->returns_aggregate = true;
    lf->out_slot = new_slot(&cc, type_size(ret), type_align(ret), "out");
  }
  lf->entry = new_block(&cc);
  use_block(&cc, lf->entry);

  Vec args = {0};
  if (agg_ret) {
    IRVreg *outp = v_load(&cc, v_slotaddr(&cc, lf->out_slot), IT_PTR);
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = outp;
    a->ty = NULL;
    vec_push(&args, a);
  }
  for (size_t i = 0; i < ft->params.n; i++) {
    Type *pt = ft->params.items[i];
    IRSlot *sl = new_slot(&cc, type_size(pt), type_align(pt), "p");
    vec_push(&lf->params, sl);
    vec_push(&lf->param_types, pt);
    IRVreg *pv = v_load(&cc, v_slotaddr(&cc, sl), ty_is_aggregate(pt) ? IT_PTR : ir_type_of(pt));
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = pv;
    a->ty = pt;
    vec_push(&args, a);
  }
  // the closure-ABI env parameter arrives last and is dropped here
  IRSlot *env = new_slot(&cc, 8, 8, "env");
  vec_push(&lf->params, env);
  vec_push(&lf->param_types, NULL);

  IRIns *call = emit(&cc, IR_CALL);
  call->callee = sym_symbol(fn);
  call->args = args;
  IRVreg *rv = NULL;
  if (ret && !agg_ret && ret->kind != TY_VOID) {
    call->dst = new_vreg(&cc, ir_type_of(ret));
    rv = call->dst;
  }
  emit_ret(&cc, rv);
  vec_push(&g_ir_fns, lf);
  return shim_name;
}

// a fn value for a named function: {shim, null env}
static IRVreg *closure_value_for_sym(LCtx *c, Sym *fn) {
  const char *shim = shim_for(fn);
  IRIns *la = emit(c, IR_ADDRC);
  la->dst = new_vreg(c, IT_PTR);
  la->callee = shim;
  la->lit = -1; // global/fn address form
  IRVreg *zero = v_const(c, 0, IT_PTR);
  IRSlot *tmp = new_slot(c, 16, 8, "fnval");
  IRVreg *addr = v_slotaddr(c, tmp);
  IRVreg *a8 = v_addi(c, addr, 8);
  IRIns *s1 = emit(c, IR_STORE);
  s1->addr = addr;
  s1->a = la->dst;
  s1->size = 8;
  IRIns *s2 = emit(c, IR_STORE);
  s2->addr = a8;
  s2->a = zero;
  s2->size = 8;
  return addr;
}

// ------------------------------------------------------ rc runtime -------
//
// Every heap object carries {rc: usize, wrc: usize, drop: fn} 24 bytes
// before its data; references point at the data. rc with the top bit set
// marks immortal static data. The helpers are synthesized as IR functions
// (rho itself has no pointer arithmetic); per-type retain/release functions
// walk values field by field, so a binding's worth of count traffic is one
// call regardless of shape.

static Map g_rel_fns;  // mangled type name -> symbol
static Map g_ret_fns;

static const char *rel_fn_for(Type *t);

static IRFn *rc_new_fn(const char *name) {
  IRFn *lf = arena_alloc_zeroed(sizeof(IRFn));
  lf->symbol = name;
  lf->ret = NULL;
  LCtx cc = {0};
  cc.fn = lf;
  lf->entry = new_block(&cc);
  use_block(&cc, lf->entry);
  return lf;
}

static void rc_finish(Vec *into, IRFn *lf) {
  if (!lf->cur->sealed)
    emit_ret(&(LCtx){.fn = lf}, NULL);
  vec_push(into, lf);
}

// scratch context accessor for the builders below
static LCtx *rc_ctx(IRFn *lf) {
  LCtx *cc = arena_alloc(sizeof(LCtx));
  cc->fn = lf;
  cc->fn->cur = lf->cur;
  return cc;
}

static void rc_runtime_build(void) {
  // count helpers come in two conventions: __rc_inc/__rc_dec take the DATA
  // pointer (header at p-24 — *T values, closure envs), __rc_inch/__rc_dech
  // take the HEADER directly (slice buf fields, which sub-slices share while
  // ptr moves). Both null-check their argument.
  for (int variant = 0; variant < 4; variant++) {
    bool retain = (variant & 1) == 0;
    bool from_data = (variant & 2) == 0;
    IRFn *lf = rc_new_fn(
        arena_printf("rho__rc_%s%s", retain ? "inc" : "dec", from_data ? "" : "h"));
    LCtx *c = rc_ctx(lf);
    IRSlot *ps = new_slot(c, 8, 8, "p");
    vec_push(&lf->params, ps);
    vec_push(&lf->param_types, NULL);
    IRVreg *p = v_load(c, v_slotaddr(c, ps), IT_PTR);
    IRVreg *isnull = v_cmp(c, CC_EQ, p, v_const(c, 0, IT_PTR), false);
    IRBlock *cont = new_block(c), *done = new_block(c);
    emit_cbr(c, isnull, done, cont, done);
    use_block(c, cont);
    IRVreg *h = from_data ? v_addi(c, p, -24) : p;
    IRVreg *rc = v_load(c, h, IT_USIZE);
    IRVreg *top = v_binop(c, IR_SHR, rc, v_const(c, 63, IT_USIZE), IT_USIZE, false);
    IRVreg *immortal = v_cmp(c, CC_NE, top, v_const(c, 0, IT_USIZE), false);
    if (retain) {
      IRBlock *bump = new_block(c);
      emit_cbr(c, immortal, done, bump, done);
      use_block(c, bump);
      // same plausibility bound as the release path below: never write a
      // count through a header that does not look like one
      IRVreg *implausible = v_cmp_uge(c, rc, v_const(c, 65536, IT_USIZE));
      IRBlock *sane = new_block(c);
      emit_cbr(c, implausible, done, sane, done);
      use_block(c, sane);
      IRVreg *rc2 = v_binop(c, IR_ADD, rc, v_const(c, 1, IT_USIZE), IT_USIZE, true);
      v_store(c, h, rc2);
      emit_br(c, done);
      use_block(c, done);
      rc_finish(&g_ir_fns, lf);
      continue;
    }
    // release: on zero, run header.drop (with the data pointer) and free
    // unless weak refs hold it
    IRBlock *live = new_block(c);
    emit_cbr(c, immortal, done, live, done);
    use_block(c, live);
    // a plausible live count is small; a pointer-sized value here means h
    // is not a real header — trap (with the caller on the stack) instead of
    // writing the count through it and corrupting the malloc freelist
    IRVreg *implausible = v_cmp_uge(c, rc, v_const(c, 65536, IT_USIZE));
    IRBlock *countsane = new_block(c);
    emit_cbr(c, implausible, done, countsane, done);
    use_block(c, countsane);
    IRVreg *rc2 = v_binop(c, IR_SUB, rc, v_const(c, 1, IT_USIZE), IT_USIZE, true);
    v_store(c, h, rc2);
    IRVreg *dead = v_cmp(c, CC_EQ, rc2, v_const(c, 0, IT_USIZE), false);
    IRBlock *maybe_drop = new_block(c);
    emit_cbr(c, dead, maybe_drop, done, done);
    use_block(c, maybe_drop);
    IRVreg *dropf = v_load(c, v_addi(c, h, 16), IT_PTR);
    IRVreg *nod = v_cmp(c, CC_EQ, dropf, v_const(c, 0, IT_PTR), false);
    IRBlock *run_drop = new_block(c), *maybe_free = new_block(c);
    emit_cbr(c, nod, maybe_free, run_drop, done);
    use_block(c, run_drop);
    Vec dargs = {0};
    IRArg *da = arena_alloc(sizeof(IRArg));
    da->vreg = from_data ? p : v_addi(c, h, 24);
    da->ty = NULL;
    vec_push(&dargs, da);
    IRIns *dc = emit(c, IR_CALL);
    dc->callee_vreg = dropf;
    dc->args = dargs;
    emit_br(c, maybe_free);
    use_block(c, maybe_free);
    IRVreg *wrc = v_load(c, v_addi(c, h, 8), IT_USIZE);
    IRVreg *noweak = v_cmp(c, CC_EQ, wrc, v_const(c, 0, IT_USIZE), false);
    IRBlock *freeh = new_block(c);
    emit_cbr(c, noweak, freeh, done, done);
    use_block(c, freeh);
    Sym *fr = prelude_fn("__free");
    Vec fargs = {0};
    IRArg *fa = arena_alloc(sizeof(IRArg));
    fa->vreg = h;
    fa->ty = NULL;
    vec_push(&fargs, fa);
    IRIns *fc = emit(c, IR_CALL);
    fc->callee = sym_symbol(fr);
    fc->args = fargs;
    emit_br(c, done);
    use_block(c, done);
    rc_finish(&g_ir_fns, lf);
  }
  // __rc_winc(h) / __rc_wdec(h): weak count up/down; last weak of a dead
  // object frees the storage
  {
    IRFn *lf = rc_new_fn("rho__rc_winc");
    LCtx *c = rc_ctx(lf);
    IRSlot *hs = new_slot(c, 8, 8, "h");
    vec_push(&lf->params, hs);
    vec_push(&lf->param_types, NULL);
    IRVreg *h = v_load(c, v_slotaddr(c, hs), IT_PTR);
    IRVreg *isnull = v_cmp(c, CC_EQ, h, v_const(c, 0, IT_PTR), false);
    IRBlock *cont = new_block(c), *done = new_block(c);
    emit_cbr(c, isnull, done, cont, done);
    use_block(c, cont);
    IRVreg *w = v_load(c, v_addi(c, h, 8), IT_USIZE);
    IRVreg *w2 = v_binop(c, IR_ADD, w, v_const(c, 1, IT_USIZE), IT_USIZE, true);
    v_store(c, v_addi(c, h, 8), w2);
    emit_br(c, done);
    use_block(c, done);
    rc_finish(&g_ir_fns, lf);
  }
  {
    IRFn *lf = rc_new_fn("rho__rc_wdec");
    LCtx *c = rc_ctx(lf);
    IRSlot *hs = new_slot(c, 8, 8, "h");
    vec_push(&lf->params, hs);
    vec_push(&lf->param_types, NULL);
    IRVreg *h = v_load(c, v_slotaddr(c, hs), IT_PTR);
    IRVreg *isnull = v_cmp(c, CC_EQ, h, v_const(c, 0, IT_PTR), false);
    IRBlock *cont = new_block(c), *done = new_block(c);
    emit_cbr(c, isnull, done, cont, done);
    use_block(c, cont);
    IRVreg *w = v_load(c, v_addi(c, h, 8), IT_USIZE);
    IRVreg *w2 = v_binop(c, IR_SUB, w, v_const(c, 1, IT_USIZE), IT_USIZE, true);
    v_store(c, v_addi(c, h, 8), w2);
    IRVreg *last = v_cmp(c, CC_EQ, w2, v_const(c, 0, IT_USIZE), false);
    IRBlock *chk = new_block(c);
    emit_cbr(c, last, chk, done, done);
    use_block(c, chk);
    IRVreg *rc = v_load(c, h, IT_USIZE);
    IRVreg *deadc = v_cmp(c, CC_EQ, rc, v_const(c, 0, IT_USIZE), false);
    IRBlock *freeh = new_block(c);
    emit_cbr(c, deadc, freeh, done, done);
    use_block(c, freeh);
    Sym *fr = prelude_fn("__free");
    Vec fargs = {0};
    IRArg *fa = arena_alloc(sizeof(IRArg));
    fa->vreg = h;
    fa->ty = NULL;
    vec_push(&fargs, fa);
    IRIns *fc = emit(c, IR_CALL);
    fc->callee = sym_symbol(fr);
    fc->args = fargs;
    emit_br(c, done);
    use_block(c, done);
    rc_finish(&g_ir_fns, lf);
  }
  // __rc_wref(p) -> header: weak.from on a data pointer
  {
    IRFn *lf = rc_new_fn("rho__rc_wref");
    LCtx *c = rc_ctx(lf);
    lf->ret = NULL;
    IRSlot *ps = new_slot(c, 8, 8, "p");
    vec_push(&lf->params, ps);
    vec_push(&lf->param_types, NULL);
    IRVreg *p = v_load(c, v_slotaddr(c, ps), IT_PTR);
    IRVreg *nullv = v_const(c, 0, IT_PTR); // before the branch: phi edges read it
    IRVreg *isnull = v_cmp(c, CC_EQ, p, nullv, false);
    IRBlock *cont = new_block(c), *done = new_block(c);
    IRVreg *res = NULL;
    emit_cbr(c, isnull, done, cont, done);
    use_block(c, cont);
    IRVreg *h = v_addi(c, p, -24);
    IRVreg *w = v_load(c, v_addi(c, h, 8), IT_USIZE);
    IRVreg *w2 = v_binop(c, IR_ADD, w, v_const(c, 1, IT_USIZE), IT_USIZE, true);
    v_store(c, v_addi(c, h, 8), w2);
    emit_br(c, done);
    use_block(c, done);
    // phi: null path -> 0, cont path -> h
    IRPhi *phi = emit_phi_in(c, done, IT_PTR);
    phi_add(phi, lf->entry, nullv);
    phi_add(phi, cont, h);
    res = phi->dst;
    emit_ret(c, res);
    vec_push(&g_ir_fns, lf);
  }
  // __rc_wget(h) -> data pointer or null when dead
  {
    IRFn *lf = rc_new_fn("rho__rc_wget");
    LCtx *c = rc_ctx(lf);
    IRSlot *hs = new_slot(c, 8, 8, "h");
    vec_push(&lf->params, hs);
    vec_push(&lf->param_types, NULL);
    IRVreg *h = v_load(c, v_slotaddr(c, hs), IT_PTR);
    IRVreg *nullv = v_const(c, 0, IT_PTR); // before the branch: phi edges read it
    IRVreg *isnull = v_cmp(c, CC_EQ, h, nullv, false);
    IRBlock *live = new_block(c), *done = new_block(c);
    emit_cbr(c, isnull, done, live, done);
    use_block(c, live);
    IRVreg *rc = v_load(c, h, IT_USIZE);
    IRVreg *top = v_binop(c, IR_SHR, rc, v_const(c, 63, IT_USIZE), IT_USIZE, false);
    IRVreg *immortal = v_cmp(c, CC_NE, top, v_const(c, 0, IT_USIZE), false);
    IRVreg *alive2 = v_cmp(c, CC_NE, rc, v_const(c, 0, IT_USIZE), false);
    IRVreg *alive = v_binop(c, IR_OR, immortal, alive2, IT_U8, false);
    IRBlock *yes = new_block(c);
    emit_cbr(c, alive, yes, done, done);
    use_block(c, yes);
    IRVreg *data = v_addi(c, h, 24);
    emit_br(c, done);
    use_block(c, done);
    IRPhi *phi = emit_phi_in(c, done, IT_PTR);
    phi_add(phi, lf->entry, nullv);
    phi_add(phi, live, nullv);
    phi_add(phi, yes, data);
    emit_ret(c, phi->dst);
    vec_push(&g_ir_fns, lf);
  }
}

// emit a call to a one-pointer-argument rc helper
static void rc_call1(LCtx *c, const char *helper, IRVreg *arg) {
  IRIns *call = emit(c, IR_CALL);
  call->callee = helper;
  IRArg *a = arena_alloc(sizeof(IRArg));
  a->vreg = arg;
  a->ty = NULL;
  vec_push(&call->args, a);
}

static void rc_inc_v(LCtx *c, IRVreg *p) { rc_call1(c, "rho__rc_inc", p); }
static void rc_dec_v(LCtx *c, IRVreg *p) { rc_call1(c, "rho__rc_dec", p); }

static void value_walk(LCtx *c, Type *t, IRVreg *addr, bool retain);

// lazily synthesize rho__rel$<t> / rho__ret$<t>: walk the value of type t at
// the sole pointer parameter. Returns NULL for unmanaged types.
static const char *value_fn_for(Type *t, bool retain) {
  if (!t || !ty_is_managed(t))
    return NULL;
  Map *cache = retain ? &g_ret_fns : &g_rel_fns;
  if (map_has(cache, str_from(t->mangled)))
    return map_get(cache, str_from(t->mangled));
  // kind prefix keeps the symbol unique: `[]*T` and `*[]T` would otherwise
  // sanitize to the same name and the second glue would clobber the first
  const char *kindp = "";
  switch (t->kind) {
  case TY_PTR: kindp = "p_"; break;
  case TY_SLICE: kindp = "s_"; break;
  case TY_STRING: kindp = "g_"; break;
  case TY_ARRAY: kindp = "a_"; break;
  case TY_ENUM: kindp = "e_"; break;
  case TY_STRUCT: kindp = "r_"; break;
  case TY_FN: kindp = "f_"; break;
  case TY_WEAK: kindp = "w_"; break;
  default: kindp = "x_"; break;
  }
  char *sym = arena_printf("rho__%s$%s%s", retain ? "ret" : "rel", kindp,
                           rho_sanitize(t->mangled));
  map_put(cache, str_from(t->mangled), sym);
  IRFn *lf = rc_new_fn(sym);
  LCtx *c = rc_ctx(lf);
  IRSlot *as_ = new_slot(c, 8, 8, "addr");
  vec_push(&lf->params, as_);
  vec_push(&lf->param_types, NULL);
  IRVreg *addr = v_load(c, v_slotaddr(c, as_), IT_PTR);
  value_walk(c, t, addr, retain);
  rc_finish(&g_ir_fns, lf);
  return sym;
}

static void value_walk_call(LCtx *c, Type *t, IRVreg *addr, bool retain) {
  const char *fn = value_fn_for(t, retain);
  if (!fn)
    return;
  rc_call1(c, fn, addr);
}

static void value_walk(LCtx *c, Type *t, IRVreg *addr, bool retain) {
  switch (t->kind) {
  case TY_PTR: {
    IRVreg *p = v_load(c, addr, IT_PTR);
    if (retain)
      rc_inc_v(c, p);
    else
      rc_dec_v(c, p);
    return;
  }
  case TY_WEAK: {
    IRVreg *h = v_load(c, addr, IT_PTR);
    rc_call1(c, retain ? "rho__rc_winc" : "rho__rc_wdec", h);
    return;
  }
  case TY_FN: {
    IRVreg *e = v_load(c, v_addi(c, addr, 8), IT_PTR);
    if (retain)
      rc_inc_v(c, e);
    else
      rc_dec_v(c, e);
    return;
  }
  case TY_SLICE:
  case TY_STRING: {
    // strings are byte buffers: elem is NULL and the buffer refcount below is
    // the whole story; only slices walk their elements
    if (t->kind == TY_SLICE && ty_is_managed(t->elem)) {
      IRVreg *ptr = v_load(c, v_addi(c, addr, 8), IT_PTR);
      IRVreg *n = v_load(c, v_addi(c, addr, 16), IT_USIZE);
      int64_t esz = type_size(t->elem);
      IRSlot *is = new_slot(c, 8, 8, "i");
      v_store(c, v_slotaddr(c, is), v_const(c, 0, IT_USIZE));
      IRBlock *hdr = new_block(c), *body = new_block(c), *done = new_block(c);
      emit_br(c, hdr);
      use_block(c, hdr);
      IRVreg *i = v_load(c, v_slotaddr(c, is), IT_USIZE);
      IRVreg *more = v_cmp(c, CC_LT, i, n, false);
      emit_cbr(c, more, body, done, done);
      use_block(c, body);
      IRVreg *scaled = v_binop(c, IR_MUL, i, v_const(c, (uint64_t)esz, IT_USIZE),
                               IT_USIZE, false);
      IRVreg *ep = v_binop(c, IR_ADD, ptr, scaled, IT_PTR, false);
      value_walk_call(c, t->elem, ep, retain);
      IRVreg *i2 = v_binop(c, IR_ADD, i, v_const(c, 1, IT_USIZE), IT_USIZE, true);
      v_store(c, v_slotaddr(c, is), i2);
      emit_br(c, hdr);
      use_block(c, done);
    }
    // buf is the allocation header (sub-slices share it while ptr moves),
    // so the count helpers take it directly
    IRVreg *buf = v_load(c, addr, IT_PTR);
    rc_call1(c, retain ? "rho__rc_inch" : "rho__rc_dech", buf);
    return;
  }
  case TY_ARRAY: {
    if (!ty_is_managed(t->elem))
      return;
    int64_t esz = type_size(t->elem);
    for (uint64_t i = 0; i < t->len; i++)
      value_walk_call(c, t->elem, v_addi(c, addr, (int64_t)i * esz), retain);
    return;
  }
  case TY_STRUCT: {
    RecType *rec = t->rec;
    for (size_t i = 0; i < rec->decl->fields.n; i++) {
      Type *ft = struct_field_type(rec, i);
      if (!ty_is_managed(ft))
        continue;
      value_walk_call(c, ft, v_addi(c, addr, struct_field_offset(rec, i)), retain);
    }
    return;
  }
  case TY_ENUM: {
    Decl *d = t->rec->decl;
    IRVreg *tag = v_load(c, addr, IT_I32);
    IRBlock *done = new_block(c);
    bool any = false;
    for (size_t vi = 0; vi < d->variants.n; vi++) {
      VariantAst *v = d->variants.items[vi];
      size_t nf = v->vkind == VAR_TUPLE ? v->types.n
                  : v->vkind == VAR_STRUCT ? v->fields.n : 0;
      bool managed = false;
      for (size_t fi = 0; fi < nf && !managed; fi++)
        managed = ty_is_managed(variant_field_type(t, (int)vi, (int)fi));
      if (!managed)
        continue;
      IRBlock *body = new_block(c);
      IRBlock *next = new_block(c);
      IRVreg *is = v_cmp(c, CC_EQ, tag, v_const(c, v->disc, IT_I32), false);
      emit_cbr(c, is, body, next, done);
      use_block(c, body);
      for (size_t fi = 0; fi < nf; fi++) {
        Type *ft = variant_field_type(t, (int)vi, (int)fi);
        if (!ty_is_managed(ft))
          continue;
        value_walk_call(c, ft,
                        v_addi(c, addr, variant_field_offset(t, (int)vi, (int)fi)),
                        retain);
      }
      emit_br(c, done);
      use_block(c, next);
      any = true;
    }
    if (any) {
      emit_br(c, done);
      use_block(c, done);
    }
    return;
  }
  default:
    return;
  }
}

// ---- structural equality glue -------------------------------------------------
// rho__eq$<t>(a, b) compares two enum/struct values field by field through a
// result slot (no phis): tags first for enums, then payload/field pairs.

static Map g_eq_fns; // mangled type name -> symbol

static void eq_walk(LCtx *c, Type *t, IRVreg *pa, IRVreg *pb, IRSlot *rs);

static const char *eq_fn_for(Type *t) {
  if (!t || (t->kind != TY_ENUM && t->kind != TY_STRUCT))
    return NULL;
  if (map_has(&g_eq_fns, str_from(t->mangled)))
    return map_get(&g_eq_fns, str_from(t->mangled));
  const char *sym = arena_printf("rho__eq$%s", rho_sanitize(t->mangled));
  map_put(&g_eq_fns, str_from(t->mangled), sym);
  IRFn *lf = rc_new_fn(sym);
  LCtx *c = rc_ctx(lf);
  IRSlot *as = new_slot(c, 8, 8, "a");
  IRSlot *bs = new_slot(c, 8, 8, "b");
  vec_push(&lf->params, as);
  vec_push(&lf->param_types, NULL);
  vec_push(&lf->params, bs);
  vec_push(&lf->param_types, NULL);
  IRVreg *pa = v_load(c, v_slotaddr(c, as), IT_PTR);
  IRVreg *pb = v_load(c, v_slotaddr(c, bs), IT_PTR);
  IRSlot *rs = new_slot(c, 8, 8, "eq");
  v_store(c, v_slotaddr(c, rs), v_const(c, 1, IT_U8));
  eq_walk(c, t, pa, pb, rs);
  emit_ret(c, v_load(c, v_slotaddr(c, rs), IT_U8));
  rc_finish(&g_ir_fns, lf);
  return sym;
}

// AND one field/variant comparison into the result slot
static void eq_and(LCtx *c, IRSlot *rs, IRVreg *v) {
  IRVreg *cur = v_load(c, v_slotaddr(c, rs), IT_U8);
  IRVreg *both = v_binop(c, IR_AND, cur, v, IT_U8, false);
  v_store(c, v_slotaddr(c, rs), both);
}

// compare a single non-aggregate field at pa/pb (same offset)
static void eq_field(LCtx *c, Type *ft, IRVreg *fa, IRVreg *fb, IRSlot *rs) {
  if (ft->kind == TY_ENUM || ft->kind == TY_STRUCT) {
    const char *fn = eq_fn_for(ft);
    IRIns *call = emit(c, IR_CALL);
    call->callee = fn;
    IRArg *x = arena_alloc(sizeof(IRArg));
    x->vreg = fa;
    x->ty = NULL;
    vec_push(&call->args, x);
    IRArg *y = arena_alloc(sizeof(IRArg));
    y->vreg = fb;
    y->ty = NULL;
    vec_push(&call->args, y);
    call->dst = new_vreg(c, IT_U8);
    eq_and(c, rs, call->dst);
    return;
  }
  IRType irt = ir_type_of(ft);
  IRVreg *va = v_load(c, fa, irt);
  IRVreg *vb = v_load(c, fb, irt);
  IRVreg *same = v_cmp(c, CC_EQ, va, vb, ir_is_float(irt));
  eq_and(c, rs, same);
}

static void eq_walk(LCtx *c, Type *t, IRVreg *pa, IRVreg *pb, IRSlot *rs) {
  if (t->kind == TY_ENUM) {
    Decl *d = t->rec->decl;
    IRVreg *tag_a = v_load(c, pa, IT_I32);
    // tags differ -> false
    {
      IRVreg *tag_b = v_load(c, pb, IT_I32);
      IRVreg *same_tag = v_cmp(c, CC_EQ, tag_a, tag_b, false);
      IRBlock *body = new_block(c), *no = new_block(c), *done = new_block(c);
      emit_cbr(c, same_tag, body, no, done);
      use_block(c, no);
      v_store(c, v_slotaddr(c, rs), v_const(c, 0, IT_U8));
      emit_br(c, done);
      use_block(c, body);
      // per variant: only its own fields, gated on the tag
      for (size_t vi = 0; vi < d->variants.n; vi++) {
        VariantAst *v = d->variants.items[vi];
        size_t nf = v->vkind == VAR_TUPLE ? v->types.n
                    : v->vkind == VAR_STRUCT ? v->fields.n : 0;
        if (!nf)
          continue;
        IRBlock *mine = new_block(c), *next = new_block(c);
        IRVreg *is = v_cmp(c, CC_EQ, tag_a, v_const(c, v->disc, IT_I32), false);
        emit_cbr(c, is, mine, next, done);
        use_block(c, mine);
        for (size_t fi = 0; fi < nf; fi++) {
          Type *ft = variant_field_type(t, (int)vi, (int)fi);
          int64_t off = variant_field_offset(t, (int)vi, (int)fi);
          eq_field(c, ft, v_addi(c, pa, off), v_addi(c, pb, off), rs);
        }
        emit_br(c, next);
        use_block(c, next);
      }
      emit_br(c, done);
      use_block(c, done);
    }
    return;
  }
  // struct: straight field walk
  RecType *rec = t->rec;
  Decl *sd = rec->decl;
  for (size_t i = 0; i < sd->fields.n; i++) {
    Type *ft = struct_field_type(rec, i);
    int64_t off = struct_field_offset(rec, i);
    eq_field(c, ft, v_addi(c, pa, off), v_addi(c, pb, off), rs);
  }
}

static const char *lift_closure(LCtx *c, Expr *e, const char **envdrop_out) {
  Type *ft = e->typed;
  Type *ret = ft->ret;
  bool agg_ret = ret && ty_is_aggregate(ret);
  int clo_id = g_closure_counter++;
  const char *sym_name = arena_printf("%s__clo%d", c->fn->symbol, clo_id);
  IRFn *lf = arena_alloc_zeroed(sizeof(IRFn));
  lf->symbol = sym_name;
  lf->ret = ret;
  LCtx cc = {0};
  cc.fn = lf;
  if (agg_ret) {
    lf->returns_aggregate = true;
    lf->out_slot = new_slot(&cc, type_size(ret), type_align(ret), "out");
  }
  lf->entry = new_block(&cc);
  use_block(&cc, lf->entry);
  scope_push(&cc);

  for (size_t i = 0; i < e->params.n; i++) {
    Param *pa = e->params.items[i];
    Type *pt = ft->params.items[i];
    IRSlot *slot = new_slot(&cc, type_size(pt), type_align(pt), str_to_c(pa->name));
    vec_push(&lf->params, slot);
    vec_push(&lf->param_types, pt);
    if (ty_is_aggregate_t(pt)) {
      IRSlot *local = new_slot(&cc, type_size(pt), type_align(pt), str_to_c(pa->name));
      IRVreg *src_addr = v_slotaddr(&cc, slot);
      IRVreg *src = v_load(&cc, src_addr, IT_PTR);
      IRVreg *dst = v_slotaddr(&cc, local);
      IRIns *cp = emit(&cc, IR_COPYMEM);
      cp->addr = dst;
      cp->a = src;
      cp->size = type_size(pt);
      bind(&cc, pa->name, local);
    } else {
      bind(&cc, pa->name, slot);
    }
  }
  IRSlot *env = new_slot(&cc, 8, 8, "env");
  vec_push(&lf->params, env);
  vec_push(&lf->param_types, NULL);

  // captures materialize into plain slots from the env data pointer
  int64_t off = 0;
  for (size_t i = 0; i < e->caps.n; i++) {
    Sym *cap = e->caps.items[i];
    Type *ct = cap->type;
    int64_t al = type_align(ct);
    off = (off + al - 1) / al * al;
    IRVreg *envp = v_load(&cc, v_slotaddr(&cc, env), IT_PTR);
    IRVreg *srcaddr = v_addi(&cc, envp, off);
    IRSlot *slot = new_slot(&cc, type_size(ct), type_align(ct), str_to_c(cap->name));
    IRVreg *dst = v_slotaddr(&cc, slot);
    if (ty_is_aggregate(ct)) {
      IRIns *cp = emit(&cc, IR_COPYMEM);
      cp->addr = dst;
      cp->a = srcaddr;
      cp->size = type_size(ct);
    } else {
      IRVreg *v = v_load(&cc, srcaddr, ir_type_of(ct));
      v_store(&cc, dst, v);
    }
    bind(&cc, cap->name, slot);
    off += type_size(ct);
  }

  for (size_t i = 0; i < e->items.n; i++)
    lv_stmt(&cc, e->items.items[i]);
  if (!lf->cur->sealed) {
    run_defers(&cc, NULL);
    emit_ret(&cc, NULL);
  }
  scope_pop(&cc);
  vec_push(&g_ir_fns, lf);

  // env drop glue: releases every captured value when the env object dies
  const char *envdrop = arena_printf("rho__envdrop$%d", clo_id);
  IRFn *df = rc_new_fn(envdrop);
  LCtx *dc = rc_ctx(df);
  IRSlot *das = new_slot(dc, 8, 8, "addr");
  vec_push(&df->params, das);
  vec_push(&df->param_types, NULL);
  IRVreg *daddr = v_load(dc, v_slotaddr(dc, das), IT_PTR);
  int64_t doff = 0;
  for (size_t i = 0; i < e->caps.n; i++) {
    Sym *cap = e->caps.items[i];
    Type *ct = cap->type;
    int64_t al = type_align(ct);
    doff = (doff + al - 1) / al * al;
    value_walk_call(dc, ct, v_addi(dc, daddr, doff), false);
    doff += type_size(ct);
  }
  rc_finish(&g_ir_fns, df);
  if (envdrop_out)
    *envdrop_out = envdrop;
  return sym_name;
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
      i->callee = sym_symbol(sym); // on demand: lower order is map order
      i->lit = -1;                 // marks global-address form
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
    if (e->fnval && sym && (sym->kind == SY_FN || sym->kind == SY_EXTERN))
      return closure_value_for_sym(c, sym);
    if (t && ty_is_aggregate(t))
      return compute_addr(c, e);
    IRSlot *slot = lookup_slot(c, e->sv);
    if (!slot) {
      if (sym && sym->kind == SY_STATIC) {
        IRIns *i = emit(c, IR_ADDRC);
        i->dst = new_vreg(c, IT_PTR);
        i->callee = sym_symbol(sym);
        i->lit = -1;
        return v_load(c, i->dst, ir_type_of(t));
      }
      return v_const(c, 0, ir_type_of(t));
    }
    return v_load(c, v_slotaddr(c, slot), ir_type_of(t));
  }
  case EX_STR: {
    // slice value: {lit(header), lit+24(bytes), len} — the literal carries
    // an immortal 24-byte rc header, so count traffic on it is a no-op
    IRSlot *tmp = new_slot(c, 24, 8, "strlit");
    IRVreg *addr = v_slotaddr(c, tmp);
    int lit = lit_bytes(c, e->sv);
    IRIns *la = emit(c, IR_LITADDR);
    la->dst = new_vreg(c, IT_PTR);
    la->lit = lit;
    IRVreg *bytes = v_addi(c, la->dst, 24);
    IRVreg *a8 = v_addi(c, addr, 8);
    IRVreg *a16 = v_addi(c, addr, 16);
    IRVreg *lenv = v_const(c, (uint64_t)e->sv.n, IT_USIZE);
    IRIns *s1 = emit(c, IR_STORE);
    s1->addr = addr;
    s1->a = la->dst;
    s1->size = 8;
    IRIns *s2 = emit(c, IR_STORE);
    s2->addr = a8;
    s2->a = bytes;
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
    IRType ty = ir_type_of(t);
    // forward the context-adapted node type to a literal operand: the
    // child of `-2.75` in an f32 position must materialize as f32, not as
    // the unadapted FLOAT_LIT default
    if (e->unop == P_MINUS && (e->a->kind == EX_FLOAT || e->a->kind == EX_INT))
      e->a->typed = t;
    IRVreg *a = lv_expr(c, e->a);
    if (e->unop == P_BANG)
      return v_cmp(c, CC_EQ, a, v_const(c, 0, a->ty), false);
    if (e->unop == P_TILDE)
      return v_binop(c, IR_XOR, a, v_const(c, ~(uint64_t)0, a->ty), a->ty, false);
    if (e->unop == P_MINUS) {
      if (ir_is_float(ty)) {
        IRVreg *zero = fconst(c, 0.0, ty);
        IRIns *i = emit(c, IR_SUB);
        i->dst = new_vreg(c, ty);
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
      IRVreg *l = lv_expr(c, e->a);
      IRVreg *l_as_u8 = v_cast(c, l, IT_U8);
      // short-circuit consts must precede the branch: phi edges read them
      IRVreg *sc_zero = v_const(c, 0, IT_U8);
      IRVreg *sc_one = v_const(c, 1, IT_U8);
      // the phi's lhs edge is the block the cbr actually branches from; a
      // nested `||`/`&&` lhs ends in its own join block, not where we started
      IRBlock *lhs_block = c->fn->cur;
      if (op == P_ANDAND)
        emit_cbr(c, l, rhs, join, join);
      else
        emit_cbr(c, l, join, rhs, join);
      use_block(c, rhs);
      IRVreg *r = lv_expr(c, e->b);
      IRVreg *r_as_u8 = v_cast(c, r, IT_U8);
      IRBlock *rhs_block = c->fn->cur;
      emit_br(c, join);
      use_block(c, join);
      phi_add(phi, lhs_block, op == P_ANDAND ? sc_zero : sc_one);
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
    case P_NE: {
      bool want_eq = e->binop == P_EQ;
      Type *lt = e->a->typed;
      if (lt && lt->kind == TY_STRING)
        return call_str_cmp(c, e, want_eq);
      if (lt && lt->kind == TY_ENUM) {
        // a and b are aggregate addresses: compare through tags/glue
        bool payloadful = false;
        Decl *d = lt->rec->decl;
        for (size_t vi = 0; vi < d->variants.n; vi++)
          if (((VariantAst *)d->variants.items[vi])->vkind != VAR_UNIT)
            payloadful = true;
        if (!payloadful) {
          IRVreg *ta = v_load(c, a, IT_I32);
          IRVreg *tb = v_load(c, b, IT_I32);
          return v_cmp(c, want_eq ? CC_EQ : CC_NE, ta, tb, false);
        }
        const char *fn = eq_fn_for(lt);
        IRIns *call = emit(c, IR_CALL);
        call->callee = fn;
        IRArg *x = arena_alloc(sizeof(IRArg));
        x->vreg = a;
        x->ty = NULL;
        vec_push(&call->args, x);
        IRArg *y = arena_alloc(sizeof(IRArg));
        y->vreg = b;
        y->ty = NULL;
        vec_push(&call->args, y);
        call->dst = new_vreg(c, IT_U8);
        if (want_eq)
          return call->dst;
        return v_cmp(c, CC_EQ, call->dst, v_const(c, 0, IT_U8), false);
      }
      return v_cmp(c, want_eq ? CC_EQ : CC_NE, a, b, flt);
    }
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
    Sym *fsym = e->sym;
    if (e->fnval && fsym && (fsym->kind == SY_FN || fsym->kind == SY_EXTERN))
      return closure_value_for_sym(c, fsym);
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
    // module-qualified statics and consts: `mod.ITEM`
    if (fsym && (fsym->kind == SY_STATIC || fsym->kind == SY_CONST)) {
      if (fsym->kind == SY_CONST) {
        CV *memo = fsym->decl->ceval_cache;
        if (memo && memo->ok && memo->is_int)
          return v_const(c, memo->i, ir_type_of(t2));
        if (memo && memo->ok && !memo->is_int && t2 &&
            (t2->kind == TY_F32 || t2->kind == TY_F64))
          return fconst(c, memo->f, ir_type_of(t2));
        if (t2 && ty_is_aggregate(t2))
          panic_call(c, "internal: constant has no address");
        return v_const(c, 0, ir_type_of(t2));
      }
      IRIns *i = emit(c, IR_ADDRC);
      i->dst = new_vreg(c, IT_PTR);
      i->callee = sym_symbol(fsym);
      i->lit = -1; // global-address form
      if (t2 && ty_is_aggregate(t2))
        return i->dst;
      return v_load(c, i->dst, ir_type_of(t2));
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
  case EX_CLOSURE: {
    const char *envdrop = NULL;
    const char *sym_name = lift_closure(c, e, &envdrop);
    // env object: header + snapshot of every capture
    int64_t esz = 0;
    for (size_t i = 0; i < e->caps.n; i++) {
      Type *ct = ((Sym *)e->caps.items[i])->type;
      int64_t al = type_align(ct);
      esz = (esz + al - 1) / al * al + type_size(ct);
    }
    IRVreg *sz = v_const(c, (uint64_t)esz + 24, IT_USIZE);
    IRVreg *obj = call_prelude1(c, "__alloc", sz, IT_PTR);
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
    IRVreg *dropv = v_const(c, 0, IT_PTR);
    if (e->caps.n) {
      IRIns *ga = emit(c, IR_ADDRC);
      ga->dst = new_vreg(c, IT_PTR);
      ga->callee = envdrop;
      ga->lit = -1;
      dropv = ga->dst;
    }
    IRIns *dr = emit(c, IR_STORE);
    dr->addr = obj16;
    dr->a = dropv;
    dr->size = 8;
    int64_t off = 0;
    for (size_t i = 0; i < e->caps.n; i++) {
      Sym *cap = e->caps.items[i];
      Type *ct = cap->type;
      int64_t al = type_align(ct);
      off = (off + al - 1) / al * al;
      IRVreg *daddr = v_addi(c, obj, 24 + off);
      IRVreg *srcaddr = v_slotaddr(c, lookup_slot(c, cap->name));
      if (ty_is_aggregate(ct)) {
        IRIns *cp = emit(c, IR_COPYMEM);
        cp->addr = daddr;
        cp->a = srcaddr;
        cp->size = type_size(ct);
      } else {
        IRVreg *v = v_load(c, srcaddr, ir_type_of(ct));
        v_store(c, daddr, v);
      }
      if (ty_is_managed(ct))
        retain_addr(c, ct, daddr); // the env owns its snapshot
      off += type_size(ct);
    }
    // the value: {code, env=data pointer}
    IRIns *la = emit(c, IR_ADDRC);
    la->dst = new_vreg(c, IT_PTR);
    la->callee = sym_name;
    la->lit = -1;
    IRVreg *envp = v_addi(c, obj, 24);
    IRSlot *tmp = new_slot(c, 16, 8, "cloval");
    IRVreg *addr = v_slotaddr(c, tmp);
    IRVreg *a8 = v_addi(c, addr, 8);
    IRIns *s1 = emit(c, IR_STORE);
    s1->addr = addr;
    s1->a = la->dst;
    s1->size = 8;
    IRIns *s2 = emit(c, IR_STORE);
    s2->addr = a8;
    s2->a = envp;
    s2->size = 8;
    return addr;
  }
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
      emit_cbr(c, cond, then_b, false_b, join);
      use_block(c, then_b);
      // each arm gets its own scope: a `let` inside an arm must release at
      // the arm's end, not at the enclosing loop's back edge (where it would
      // run on every iteration regardless of which arm executed)
      scope_push(c);
      lv_stmts(c, &((Stmt *)e->items.items[0])->stmts);
      run_defers(c, c->scope->parent);
      scope_pop_release(c);
      emit_br(c, join);
      if (e->items.n > 1) {
        use_block(c, false_b);
        void *els = e->items.items[1];
        Expr *else_if = els;
        if (else_if->kind == EX_IF)
          lv_expr(c, else_if);
        else {
          scope_push(c);
          lv_stmts(c, &((Stmt *)els)->stmts);
          run_defers(c, c->scope->parent);
          scope_pop_release(c);
        }
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
    emit_cbr(c, cond, then_b, false_b, join);
    use_block(c, then_b);
    Vec *then_stmts = &((Stmt *)e->items.items[0])->stmts;
    IRVreg *tv = block_value(c, then_stmts, t);
    if (ty_is_managed(t) && !arm_value_owned(then_stmts))
      retain_or_inc(c, t, tv);
    IRBlock *then_end = c->fn->cur;
    emit_br(c, join);
    IRVreg *ev = NULL;
    IRBlock *else_end = NULL;
    if (e->items.n > 1) {
      use_block(c, false_b);
      void *els = e->items.items[1];
      Expr *else_if = els;
      if (else_if->kind == EX_IF) {
        ev = lv_expr(c, else_if); // nested ifs normalize their own arms
      } else {
        Vec *es = &((Stmt *)els)->stmts;
        ev = block_value(c, es, t);
        if (ty_is_managed(t) && !arm_value_owned(es))
          retain_or_inc(c, t, ev);
      }
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
        emit_cbr(c, is, body, next, join);
      } else {
        IRVreg *is = v_cmp(c, CC_EQ, scrut, v_const(c, arm->pat_int, scrut->ty), false);
        emit_cbr(c, is, body, next, join);
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
          if (ty_is_managed(ft)) {
            retain_addr(c, ft, saddr);
            own_slot(c, slot, ft);
          }
        }
      }
      IRVreg *val;
      if (arm->body->kind == EX_BLOCK) {
        scope_push(c);
        val = block_value(c, &arm->body->items, t);
        run_defers(c, c->scope->parent);
        scope_pop_release(c);
      } else {
        val = lv_expr(c, arm->body);
      }
      if (!val) {
        // a never-returning arm (panic call): the phi still needs an arg,
        // but control never reaches the join from here
        val = v_const(c, 0, ir_type_of(t));
      }
      if (t && ty_is_managed(t) && !expr_owned(arm->body))
        retain_or_inc(c, t, val);
      if (bind_scope)
        scope_pop_release(c);
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
    emit_cbr(c, is_ok, cont, prop, cont);
    use_block(c, prop);
    run_defers(c, NULL);
    IRVreg *dst = ret_dest(c);
    IRIns *cp = emit(c, IR_COPYMEM);
    cp->addr = dst;
    cp->a = val;
    cp->size = type_size(et);
    if (ty_is_managed(et) && !expr_owned(e->a))
      retain_addr(c, et, dst);
    release_scopes_to(c, NULL);
    emit_ret(c, NULL);
    IRBlock *dead = new_block(c);
    use_block(c, dead);
    emit_br(c, cont); // unreachable; keeps the CFG well-formed
    use_block(c, cont);
    if (ty_is_managed(et) && expr_owned(e->a))
      release_addr(c, et, val);
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
  Vec owned_args = {0};   // Expr* of owned rvalues needing a post-call release
  Vec owned_slots = {0};  // parallel: index of each owned value inside call_args
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
    a->vreg = lv_expr(c, receiver);
    a->ty = self_t;
    vec_push(&call_args, a);
    if (ty_is_managed(self_t) && expr_owned(receiver)) {
      vec_push(&owned_args, receiver);
      vec_push(&owned_slots, (void *)(long)(call_args.n - 1));
    }
    param_index = 1;
  }
  for (size_t i = 0; i < args_exprs->n; i++) {
    Expr *arg = args_exprs->items[i];
    Type *pt = fn->type->params.n > param_index + i
                   ? fn->type->params.items[param_index + i]
                   : (arg->typed ? arg->typed : NULL);
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = lv_expr(c, arg); // aggregate rvalue = address of temp
    a->ty = pt;
    vec_push(&call_args, a);
    if (pt && ty_is_managed(pt) && expr_owned(arg)) {
      vec_push(&owned_args, arg);
      vec_push(&owned_slots, (void *)(long)(call_args.n - 1));
    }
  }
  IRIns *call = emit(c, IR_CALL);
  call->callee = sym_symbol(fn);
  call->args = call_args;
  if (ret && !agg_ret && ret->kind != TY_VOID)
    call->dst = new_vreg(c, ir_type_of(ret));
  // owned arguments handed their +1 to the callee's param copies
  for (size_t i = 0; i < owned_args.n; i++) {
    Expr *arg = owned_args.items[i];
    Type *at = arg->typed;
    IRVreg *pv =
        ((IRArg *)call_args.items[(long)owned_slots.items[i]])->vreg;
    if (ty_is_aggregate(at))
      release_addr(c, at, pv);
    else
      rc_dec_v(c, pv);
  }
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
  IRVreg *drop0 = v_const(c, 0, IT_PTR); // operands before emit: see landmine #1
  IRIns *dr = emit(c, IR_STORE);
  dr->addr = obj16;
  dr->a = drop0;
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
  IRVreg *dropv = v_const(c, 0, IT_PTR);
  const char *glue = value_fn_for(struct_t, false);
  if (glue) {
    IRIns *ga = emit(c, IR_ADDRC);
    ga->dst = new_vreg(c, IT_PTR);
    ga->callee = glue;
    ga->lit = -1;
    dropv = ga->dst;
  }
  IRIns *dr = emit(c, IR_STORE);
  dr->addr = obj16;
  dr->a = dropv;
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
          if (!expr_owned(init))
            retain_addr(c, ft, faddr);
        } else {
          IRVreg *v = lv_expr(c, init);
          v_store(c, faddr, v);
          // the field owns a reference: borrowed inits hand it a +1; owned
          // producers transfer their single count
          if (ty_is_managed(ft) && !expr_owned(init))
            rc_inc_v(c, v);
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
    if (ft && ty_is_aggregate(ft)) {
      lv_agg(c, arg, faddr);
      if (!expr_owned(arg))
        retain_addr(c, ft, faddr);
    } else {
      IRVreg *pv = lv_expr(c, arg);
      v_store(c, faddr, pv);
      if (ft && ty_is_managed(ft) && !expr_owned(arg))
        rc_inc_v(c, pv);
    }
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
  // weak.from(p) -> __rc_wref(p): bumps wrc, returns the header handle
  if (e->a->kind == EX_FIELD && e->a->a->kind == EX_NAME &&
      str_eq_c(e->a->a->sv, "weak") && str_eq_c(e->a->sv, "from")) {
    IRVreg *p = lv_expr(c, e->args.items[0]);
    IRIns *call = emit(c, IR_CALL);
    call->callee = "rho__rc_wref";
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = p;
    a->ty = NULL;
    vec_push(&call->args, a);
    call->dst = new_vreg(c, IT_PTR);
    return call->dst;
  }
  // w.get() -> __rc_wget(w): data pointer or null when the object died
  if (e->a->kind == EX_FIELD && str_eq_c(e->a->sv, "get") && e->a->a->typed &&
      ((Type *)e->a->a->typed)->kind == TY_WEAK) {
    IRVreg *w = lv_expr(c, e->a->a);
    IRIns *call = emit(c, IR_CALL);
    call->callee = "rho__rc_wget";
    IRArg *a = arena_alloc(sizeof(IRArg));
    a->vreg = w;
    a->ty = NULL;
    vec_push(&call->args, a);
    call->dst = new_vreg(c, IT_PTR);
    return call->dst;
  }
  // intrinsics.* — raw-memory kernel (spec §10)
  if (e->a->kind == EX_FIELD && e->a->a->kind == EX_NAME &&
      str_eq_c(e->a->a->sv, "intrinsics")) {
    const char *name = str_to_c(e->a->sv);
    if (!strcmp(name, "load_u8") || !strcmp(name, "load_u32") ||
        !strcmp(name, "load_u64") || !strcmp(name, "load_i64")) {
      IRType it = !strcmp(name, "load_u8") ? IT_U8
                  : !strcmp(name, "load_u32") ? IT_U32
                  : !strcmp(name, "load_u64") ? IT_U64
                                              : IT_I64;
      IRVreg *p = lv_expr(c, e->args.items[0]);
      return v_load(c, p, it);
    }
    if (!strcmp(name, "store_u8") || !strcmp(name, "store_u32") ||
        !strcmp(name, "store_u64") || !strcmp(name, "store_i64")) {
      IRVreg *p = lv_expr(c, e->args.items[0]);
      IRVreg *v = lv_expr(c, e->args.items[1]);
      v_store(c, p, v);
      return v_const(c, 0, IT_U8);
    }
    if (!strcmp(name, "f64_bits") || !strcmp(name, "f32_bits")) {
      IRVreg *v = lv_expr(c, e->args.items[0]);
      bool wantf = !strcmp(name, "f64_bits");
      return v_cast(c, v, wantf ? IT_U64 : IT_U32);
    }
    if (!strcmp(name, "memcpy")) {
      IRVreg *dst = lv_expr(c, e->args.items[0]);
      IRVreg *src = lv_expr(c, e->args.items[1]);
      IRIns *cp = emit(c, IR_COPYMEM);
      cp->addr = dst;
      cp->a = src;
      cp->b = lv_expr(c, e->args.items[2]); // runtime size
      cp->size = -1;
      return v_const(c, 0, IT_U8);
    }
    if (!strcmp(name, "slice_string")) {
      IRVreg *base = lv_expr(c, e->args.items[0]); // []u8 aggregate address
      IRVreg *len = v_load(c, v_addi(c, base, 16), IT_USIZE);
      IRVreg *ptr = v_load(c, v_addi(c, base, 8), IT_PTR);
      // obj = __alloc(len + 24): header at obj, data at obj+24 — the same
      // layout make() produces; buf fields hold the header
      IRVreg *total = v_binop(c, IR_ADD, len, v_const(c, 24, IT_USIZE), IT_USIZE, false);
      IRVreg *obj = call_prelude1(c, "__alloc", total, IT_PTR);
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
      IRVreg *drop0 = v_const(c, 0, IT_PTR); // operands before emit: landmine #1
      IRIns *dr = emit(c, IR_STORE);
      dr->addr = obj16;
      dr->a = drop0;
      dr->size = 8;
      IRIns *cp = emit(c, IR_COPYMEM);
      cp->addr = obj24;
      cp->a = ptr;
      cp->b = len; // runtime byte count
      cp->size = -1;
      // build {buf=obj (header), ptr=obj+24 (data), len}
      IRSlot *tmp = new_slot(c, 24, 8, "strval");
      IRVreg *dest = v_slotaddr(c, tmp);
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
      s3->a = len;
      s3->size = 8;
      return dest;
    }
  }
  if (is_variant_ctor(e)) {
    Type *et = e->typed;
    IRSlot *tmp = new_slot(c, type_size(et), type_align(et), "enumval");
    return lower_enum_ctor(c, e, v_slotaddr(c, tmp));
  }
  // closure-value call: load {code, env} and call code(args..., env)
  Type *callee_t = e->a->typed;
  Sym *csym = e->a->sym;
  bool direct = csym && (csym->kind == SY_FN || csym->kind == SY_EXTERN ||
                         csym->kind == SY_VARIANT);
  if (callee_t && callee_t->kind == TY_FN && !direct) {
    IRVreg *val = lv_expr(c, e->a); // 16-byte value address
    IRVreg *code = v_load(c, val, IT_PTR);
    IRVreg *env = v_load(c, v_addi(c, val, 8), IT_PTR);
    Type *ft = callee_t;
    Type *ret = ft->ret;
    bool agg = ret && ty_is_aggregate(ret);
    Vec args = {0};
    Vec owned_args = {0};  // Expr* of owned rvalues needing a post-call release
    Vec owned_slots = {0}; // parallel: index into args
    IRVreg *out = NULL;
    if (agg) {
      IRSlot *tmp = new_slot(c, type_size(ret), type_align(ret), "out");
      out = v_slotaddr(c, tmp);
      IRArg *a = arena_alloc(sizeof(IRArg));
      a->vreg = out;
      a->ty = NULL;
      vec_push(&args, a);
    }
    for (size_t i = 0; i < e->args.n && i < ft->params.n; i++) {
      Expr *arg = e->args.items[i];
      Type *pt = ft->params.items[i];
      IRArg *a = arena_alloc(sizeof(IRArg));
      a->vreg = lv_expr(c, arg);
      a->ty = pt;
      vec_push(&args, a);
      if (pt && ty_is_managed(pt) && expr_owned(arg)) {
        vec_push(&owned_args, arg);
        vec_push(&owned_slots, (void *)(long)(args.n - 1));
      }
    }
    IRArg *ea = arena_alloc(sizeof(IRArg));
    ea->vreg = env;
    ea->ty = NULL;
    vec_push(&args, ea);
    IRIns *call = emit(c, IR_CALL);
    call->callee_vreg = code;
    call->args = args;
    // owned arguments handed their +1 to the callee's param copies
    for (size_t i = 0; i < owned_args.n; i++) {
      Expr *arg = owned_args.items[i];
      Type *at = arg->typed;
      IRVreg *pv = ((IRArg *)args.items[(long)owned_slots.items[i]])->vreg;
      if (ty_is_aggregate(at))
        release_addr(c, at, pv);
      else
        rc_dec_v(c, pv);
    }
    if (agg)
      return out;
    if (ret && ret->kind != TY_VOID) {
      call->dst = new_vreg(c, ir_type_of(ret));
      return call->dst;
    }
    return v_const(c, 0, IT_PTR);
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
  scope_pop_release(c);
  (void)t;
  return tail;
}

// ------------------------------------------------------------ statements ---

static void lv_expr_discard(LCtx *c, Expr *e) {
  Type *t = e->typed;
  if (t && ty_is_managed(t) && expr_owned(e)) {
    IRVreg *v = lv_expr(c, e);
    if (ty_is_aggregate(t))
      release_addr(c, t, v);
    else
      rc_dec_v(c, v);
    return;
  }
  if (t && ty_is_aggregate(t))
    lv_expr(c, e);
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
    if (ty_is_managed(t)) {
      if (!expr_owned(s->a))
        retain_addr(c, t, addr);
      own_slot(c, slot, t);
    }
    break;
  }
  case ST_ASSIGN: {
    Type *t = s->a->typed;
    IRVreg *addr = compute_addr(c, s->a);
    if (s->assign_op == P_ASSIGN) {
      if (ty_is_managed(t)) {
        // save the old value before the store overwrites it
        IRSlot *old = new_slot(c, type_size(t), type_align(t), "oldval");
        IRVreg *olda = v_slotaddr(c, old);
        if (ty_is_aggregate(t)) {
          IRIns *cp = emit(c, IR_COPYMEM);
          cp->addr = olda;
          cp->a = addr;
          cp->size = type_size(t);
        } else {
          v_store(c, olda, v_load(c, addr, ir_type_of(t)));
        }
        if (ty_is_aggregate(t)) {
          lv_agg(c, s->b, addr);
          if (!expr_owned(s->b))
            retain_addr(c, t, addr);
        } else {
          IRVreg *v = lv_expr(c, s->b);
          v_store(c, addr, v);
          if (!expr_owned(s->b))
            rc_inc_v(c, v);
        }
        release_addr(c, t, olda);
        break;
      }
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
    Type *t = s->a ? s->a->typed : NULL;
    if (s->a && ty_is_aggregate(t)) {
      IRVreg *src = lv_expr(c, s->a);
      IRVreg *dst = ret_dest(c);
      IRIns *cp = emit(c, IR_COPYMEM);
      cp->addr = dst;
      cp->a = src;
      cp->size = type_size(t);
      if (ty_is_managed(t) && !expr_owned(s->a))
        retain_addr(c, t, dst);
      run_defers(c, NULL);
      release_scopes_to(c, NULL);
      emit_ret(c, NULL);
    } else if (s->a && ty_is_managed(t)) {
      IRVreg *v = lv_expr(c, s->a);
      if (!expr_owned(s->a))
        rc_inc_v(c, v);
      run_defers(c, NULL);
      release_scopes_to(c, NULL);
      emit_ret(c, v);
    } else {
      // evaluate before defers/release: the value may read scope-owned data
      IRVreg *v = s->a ? lv_expr(c, s->a) : NULL;
      run_defers(c, NULL);
      release_scopes_to(c, NULL);
      emit_ret(c, v);
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
    release_scopes_to(c, loop);
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
    header->loop_header = true;
    header->loop_exit = done;
    emit_br(c, header);
    use_block(c, header);
    IRVreg *cond = lv_expr(c, s->cond);
    emit_cbr(c, cond, body, done, done);
    use_block(c, body);
    scope_push(c);
    c->scope->is_loop = true;
    c->scope->break_to = done;
    c->scope->continue_to = header;
    for (size_t i = 0; i < s->body.n; i++)
      lv_stmt(c, s->body.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop_release(c);
    emit_br(c, header);
    use_block(c, done);
    break;
  }
  case ST_LOOP: {
    IRBlock *body = new_block(c);
    IRBlock *done = new_block(c);
    body->loop_header = true;
    body->loop_exit = done;
    emit_br(c, body);
    use_block(c, body);
    scope_push(c);
    c->scope->is_loop = true;
    c->scope->break_to = done;
    c->scope->continue_to = body;
    for (size_t i = 0; i < s->body.n; i++)
      lv_stmt(c, s->body.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop_release(c);
    emit_br(c, body);
    use_block(c, done);
    break;
  }
  case ST_BLOCK: {
    scope_push(c);
    for (size_t i = 0; i < s->stmts.n; i++)
      lv_stmt(c, s->stmts.items[i]);
    run_defers(c, c->scope->parent);
    scope_pop_release(c);
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
      if (ty_is_managed(pt)) {
        retain_addr(&ctx, pt, dst);
        own_slot(&ctx, local, pt);
      }
    } else {
      bind(&ctx, pa->name, slot);
      if (ty_is_managed(pt)) {
        retain_addr(&ctx, pt, v_slotaddr(&ctx, slot));
        own_slot(&ctx, slot, pt);
      }
    }
  }
  if (sym->owner == g_root_module && str_eq_c(sym->name, "main"))
    g_main_symbol = fn->symbol;

  for (size_t i = 0; i < d->body.n; i++)
    lv_stmt(&ctx, d->body.items[i]);

  if (!fn->cur->sealed) {
    run_defers(&ctx, NULL);
    release_scopes_to(&ctx, NULL);
    emit_ret(&ctx, NULL);
  }
  scope_pop(&ctx);
  return fn;
}

static void lower_static(Sym *sym) {
  Decl *d = sym->decl;
  Type *t = sym->type;
  if (getenv("RHO_DEBUG"))
    fprintf(stderr, "STATIC %s init=%p typed=%p mut=%d\n", str_to_c(sym->name),
            (void *)d->init, d->init ? (void *)d->init->typed : NULL, (int)d->is_mut);
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
    char *bytes_label = arena_printf("%s_b", label);
    g->relocs = arena_alloc(3 * sizeof(char *));
    g->relocs[0] = label;      // buf: the immortal header block
    g->relocs[1] = bytes_label; // ptr: the bytes
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
  // per-compile caches: stale symbols would point at previous compiles'
  // glue fns (which no longer exist in this module's emission)
  g_ret_fns = (Map){0};
  g_rel_fns = (Map){0};
  g_eq_fns = (Map){0};
  rc_runtime_build(); // __rc_inc/dec/w* helpers every managed program needs
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    for (size_t k = 0; k < m->syms.keys.n; k++) {
      Str *key = m->syms.keys.items[k];
      Sym *sym = map_get(&m->syms, *key);
      // lower real definitions only; externs declare without a body and an
      // empty-bodied fn is still callable (e.g. esp32c3 __free is a no-op)
      if (sym->kind == SY_FN && sym->decl && sym->decl->kind == DK_FN &&
          !sym->decl->templated)
        vec_push(&g_ir_fns, lower_fn(sym));
      else if (sym->kind == SY_STATIC)
        lower_static(sym);
    }
  }
  if (!getenv("RHO_NO_DCE"))
    lower_drop_unreachable();
}

// ------------------------------------------------------ reachability -----
//
// Post-lowering dead-function elimination (spec §11.3): the prelude carries
// formatting machinery most programs never touch, and lowering emits every
// checked fn. Every cross-function reference in the IR is a `callee` string
// — direct calls, address-taken fn values and globals, shims — so a symbol
// walk over instructions is the whole reference graph. Two classes are kept
// beyond the graph: the entry fn, and the fns backends call by symbol from
// emitted code (__panic_div everywhere; the esp32c3 soft-float set on RV32).
// Emission order is preserved — determinism is untouched.

typedef struct ReachCtx {
  Map *all;  // symbol -> IRFn*
  Map *keep; // symbol -> IRFn* kept so far
  Vec *work;
} ReachCtx;

static void reach_visit(const char *sym, void *p) {
  ReachCtx *rc = p;
  IRFn *fn = map_get(rc->all, str_from(sym));
  if (fn && !map_get(rc->keep, str_from(fn->symbol))) {
    map_put(rc->keep, str_from(fn->symbol), fn);
    vec_push(rc->work, fn);
  }
}

static void lower_drop_unreachable(void) {
  if (g_ir_fns.n == 0)
    return;
  Map all = {0};
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    map_put(&all, str_from(fn->symbol), fn);
  }
  // entry point; without a resolvable one (defensive — every real build has
  // main) keep everything rather than guess
  if (!g_main_symbol || !map_get(&all, str_from(g_main_symbol)))
    return;
  Map keep = {0};
  Vec work = {0};
  ReachCtx rc = {&all, &keep, &work};
  reach_visit(g_main_symbol, &rc);
  // the divide-by-zero check every backend emits references by symbol
  reach_visit(prelude_symbol("__panic_div"), &rc);
  if (g_prelude_esp32) {
    // RV32 soft-float helpers + the panic family the emitter can reach
    static const char *ESP32_KEEP[] = {
        "__fadd32", "__fadd64", "__fsub32", "__fsub64", "__fmul32", "__fmul64",
        "__fdiv32", "__fdiv64", "__feq32", "__feq64", "__flt32", "__flt64",
        "__f64_f32", "__f32_f64", "__i2f64", "__f2i64", "__panic_oob",
        "__panic_null",
    };
    for (size_t i = 0; i < sizeof(ESP32_KEEP) / sizeof(ESP32_KEEP[0]); i++)
      reach_visit(prelude_symbol(ESP32_KEEP[i]), &rc);
  }
  while (work.n > 0) {
    IRFn *fn = work.items[work.n - 1];
    work.n--;
    for (size_t bi = 0; bi < fn->blocks.n; bi++) {
      IRBlock *b = fn->blocks.items[bi];
      for (size_t ii = 0; ii < b->ins.n; ii++) {
        IRIns *i = b->ins.items[ii];
        if (i->callee)
          reach_visit(i->callee, &rc);
      }
    }
  }
  // filter in place, preserving emission order
  size_t w = 0;
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    if (map_get(&keep, str_from(fn->symbol)))
      g_ir_fns.items[w++] = fn;
  }
  g_ir_fns.n = w;
}

void lower_dump_ir(void) {
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    printf("fn %s\n", fn->symbol);
    printf("  frame_slots:\n");
    for (size_t si = 0; si < fn->slots.n; si++) {
      IRSlot *s = fn->slots.items[si];
      printf("    slot %d size=%lld name=%s\n", s->id, (long long)s->size,
             s->name ? s->name : "-");
    }
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
        printf("    op=%d dst=v%d a=v%d b=v%d addr=v%d slot=%d imm=%lld size=%lld flt=%d sgn=%d callee=%s\n",
               (int)ins->op, ins->dst ? ins->dst->id : -1, ins->a ? ins->a->id : -1,
               ins->b ? ins->b->id : -1, ins->addr ? ins->addr->id : -1,
               ins->slot ? (int)ins->slot->id : -1, (long long)ins->imm,
               (long long)ins->size, (int)ins->is_float, (int)ins->signed_ops,
               ins->callee ? ins->callee : "-");
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
