// wasm32-wasi binary emitter, following docs/wasm-design.md with one
// deviation: the shadow stack grows DOWN from a top-of-stack base above the
// heap (an upward-growing stack would collide with statics at 1024 on the
// frames real functions need).
//
// Vregs become wasm locals (the spill-everything IR maps 1:1 onto locals),
// frame slots live in linear memory at $fb + off, the structured CFG is
// re-nested (if-diamonds close over `block $join`, loops become
// `block $exit loop $h`), and function addresses — closure code fields and
// rc drop glue — are table indices reached through call_indirect.

#include "ir.h"

// ---- raw bytes -----------------------------------------------------------------

static void w8(SB *sb, unsigned char b) { sb_push(sb, (char)b); }

static void wbytes(SB *sb, const void *p, size_t n) {
  const unsigned char *q = p;
  for (size_t i = 0; i < n; i++)
    w8(sb, q[i]);
}

// a Vec whose slots hold byte values as void* — never read its raw storage
static void wvec_bytes(SB *sb, Vec *v) {
  for (size_t i = 0; i < v->n; i++)
    w8(sb, (unsigned char)(long)v->items[i]);
}

static void wuleb(SB *sb, uint64_t v) {
  do {
    unsigned char b = v & 0x7F;
    v >>= 7;
    if (v)
      b |= 0x80;
    w8(sb, b);
  } while (v);
}

static void wsleb(SB *sb, int64_t v) {
  bool more = true;
  while (more) {
    unsigned char b = (unsigned char)(v & 0x7F);
    v >>= 7;
    if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
      more = false;
    else
      b |= 0x80;
    w8(sb, b);
  }
}

static void wstr(SB *sb, const char *s) {
  size_t n = strlen(s);
  wuleb(sb, n);
  wbytes(sb, s, n);
}

static void wsection(SB *out, unsigned char id, SB *body) {
  w8(out, id);
  Str b = sb_finish(body);
  wuleb(out, b.n);
  sb_append(out, b);
}

static unsigned char w_vt(IRType t) {
  switch (t) {
  case IT_F32: return 0x7D;
  case IT_F64: return 0x7C;
  case IT_I64: case IT_U64: case IT_USIZE: return 0x7E;
  default: return 0x7F;
  }
}

static bool w_is64(IRType t) { return t == IT_I64 || t == IT_U64 || t == IT_USIZE; }

IRFn *ir_fn_for_symbol(const char *symbol) {
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *f = g_ir_fns.items[i];
    if (strcmp(f->symbol, symbol) == 0)
      return f;
  }
  return NULL;
}

// ---- module state ---------------------------------------------------------------

typedef struct WType {
  Vec params; // IRType as long
  bool has_ret;
  IRType ret;
  int index;
} WType;

typedef struct WFn {
  IRFn *fn;
  int index; // function index (after the two wasi imports)
  int type;
} WFn;

typedef struct WLabel {
  int kind; // 0 block, 1 loop
  IRBlock *b;
} WLabel;

static struct {
  Vec fns;              // WFn*
  Vec types;            // WType*
  Map type_by_key;
  Map fn_by_symbol;
  Map table_idx;        // symbol -> (long)(index+1)
  Vec table_fns;        // const char* in table order
  Map global_addr;      // symbol -> (long)address
  Map lit_addr;         // "uid:label" -> (long)address
  int64_t heap_base;
  int64_t stack_top;
  int64_t data_end;
} W;

// ---- types ----------------------------------------------------------------------

static int w_type_for(Vec params, bool has_ret, IRType ret) {
  SB key = {0};
  for (size_t i = 0; i < params.n; i++)
    sb_printf(&key, "%d,", (int)(long)params.items[i]);
  sb_printf(&key, "->%d", has_ret ? (int)ret : -1);
  Str k = sb_finish(&key);
  WType *found = map_get(&W.type_by_key, k);
  if (found)
    return found->index;
  WType *t = arena_alloc(sizeof(WType));
  t->params = params;
  t->has_ret = has_ret;
  t->ret = ret;
  t->index = (int)W.types.n;
  vec_push(&W.types, t);
  map_put(&W.type_by_key, k, t);
  return t->index;
}

static IRType w_ir_type(Type *t) {
  if (!t)
    return IT_PTR;
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

static bool ty_travels_as_ptr(Type *pt) {
  return pt && (pt->kind == TY_STRUCT || pt->kind == TY_ENUM ||
                pt->kind == TY_SLICE || pt->kind == TY_STRING ||
                pt->kind == TY_ARRAY || pt->kind == TY_FN);
}

// does any RET terminator in fn carry a value?
static bool fn_returns_value(IRFn *fn) {
  for (size_t b = 0; b < fn->blocks.n; b++) {
    IRBlock *blk = fn->blocks.items[b];
    if (blk->term && blk->term->op == (IROp)OP_RET && blk->term->a)
      return true;
  }
  return false;
}

static int w_type_of_fn(IRFn *fn) {
  Vec params = {0};
  for (size_t i = 0; i < fn->params.n; i++) {
    Type *pt = fn->param_types.n > i ? fn->param_types.items[i] : NULL;
    // synth (rc) fns carry NULL param types: their params are pointers
    vec_push(&params,
             (void *)(long)(pt && !ty_travels_as_ptr(pt) ? w_ir_type(pt) : IT_PTR));
  }
  // synth (rc) fns carry fn->ret == NULL whether or not they return — ask
  // the terminators: any RET with a value means the signature needs one
  bool no_ret = fn->returns_aggregate || (fn->ret && fn->ret->kind == TY_VOID) ||
                (!fn->ret && !fn_returns_value(fn));
  if (no_ret)
    return w_type_for(params, false, IT_I32);
  IRType ret = fn->ret ? w_ir_type(fn->ret) : IT_PTR; // synth fns return ptrs
  return w_type_for(params, true, ret);
}

static int w_type_of_call(IRIns *i) {
  Vec params = {0};
  for (size_t k = 0; k < i->args.n; k++)
    vec_push(&params, (void *)(long)((IRArg *)i->args.items[k])->vreg->ty);
  return i->dst ? w_type_for(params, true, i->dst->ty)
                : w_type_for(params, false, IT_I32);
}

// ---- data layout ------------------------------------------------------------------
// 1024 | statics | [24B immortal header + bytes] per literal | heap | ... | stack

static void w_layout_data(void) {
  int64_t at = 1024;
  for (size_t g = 0; g < g_ir_globals.n; g++) {
    IRGlobal *gl = g_ir_globals.items[g];
    if (gl->is_extern)
      continue;
    int64_t al = gl->align < 8 ? 8 : gl->align;
    at = (at + al - 1) / al * al;
    map_put(&W.global_addr, str_from(gl->symbol), (void *)(long)at);
    at += gl->size;
  }
  for (size_t f = 0; f < g_ir_fns.n; f++) {
    IRFn *fn = g_ir_fns.items[f];
    for (size_t l = 0; l < fn->literals.n; l++) {
      IRLiteral *lit = fn->literals.items[l];
      at = (at + 7) / 8 * 8;
      map_put(&W.lit_addr,
              str_from(arena_printf("%d:%s", fn->uid, lit->label)),
              (void *)(long)at);
      at += 24 + (int64_t)lit->bytes.n;
    }
  }
  W.heap_base = (at + 15) / 16 * 16;
  // the wasi prelude's HEAP static starts allocation above the data image
  for (size_t g = 0; g < g_ir_globals.n; g++) {
    IRGlobal *gl = g_ir_globals.items[g];
    if (gl->is_extern || gl->init.n != 8)
      continue;
    const char *sym = gl->symbol;
    size_t n = strlen(sym);
    if (n >= 5 && strcmp(sym + n - 5, "_HEAP") == 0) {
      uint64_t v = (uint64_t)W.heap_base;
      for (int b = 0; b < 8; b++)
        gl->init.items[b] = (void *)(long)((v >> (8 * b)) & 0xFF);
    }
  }
  // the bump heap grows up toward the stack; leave it 8 MiB of headroom
  W.stack_top = W.heap_base + 0x800000;
  W.data_end = at;
}

// ---- per-function emission ----------------------------------------------------------

typedef struct WClaim {
  IRBlock *b;
  int base; // labels.n at claim time: br depth = labels.n - base
} WClaim;

typedef struct WFnCtx {
  IRFn *fn;
  SB *body;
  Vec slot_off; // slot id -> (long) frame offset (>=0 once laid out)
  int64_t frame;
  Vec labels;  // WLabel: REAL wasm constructs (if placeholders, loop labels)
  Vec claims;  // WClaim: virtual join ownership (no construct of their own)
  Vec loops;   // IRBlock* headers, innermost last
} WFnCtx;

static int64_t slot_offset(WFnCtx *c, IRSlot *s) {
  if (!s) {
    fprintf(stderr, "NULL slot in fn %s\n", c->fn ? c->fn->symbol : "?");
    abort();
  }
  while (c->slot_off.n <= (size_t)s->id)
    vec_push(&c->slot_off, (void *)(long)-1);
  int64_t off = (long)c->slot_off.items[s->id];
  if (off >= 0)
    return off;
  int64_t al = s->align > 8 ? 8 : s->align;
  int64_t sz = s->size < 8 ? 8 : s->size;
  c->frame = (c->frame + al - 1) / al * al;
  off = c->frame;
  c->frame += sz;
  c->slot_off.items[s->id] = (void *)(long)off;
  return off;
}

static int vreg_local(WFnCtx *c, IRVreg *v) {
  return (int)c->fn->params.n + 1 + v->id; // params, $fb, vregs...
}

static void lget(WFnCtx *c, IRVreg *v) {
  w8(c->body, 0x20);
  wuleb(c->body, vreg_local(c, v));
}

static void lset(WFnCtx *c, IRVreg *v) {
  w8(c->body, 0x21);
  wuleb(c->body, vreg_local(c, v));
}

static void i32c(WFnCtx *c, int64_t v) {
  w8(c->body, 0x41);
  wsleb(c->body, v);
}

static void emit_load(WFnCtx *c, IRIns *i) {
  lget(c, i->addr);
  // op follows the DESTINATION wasm type: pointers are 8 bytes in memory
  // but i32 locals (addresses fit the 32-bit memory space)
  IRType t = i->dst->ty;
  if (t == IT_F32) {
    w8(c->body, 0x2A);
    wuleb(c->body, 2);
  } else if (t == IT_F64) {
    w8(c->body, 0x2B);
    wuleb(c->body, 3);
  } else if (w_is64(t)) {
    w8(c->body, 0x29); // i64.load
    wuleb(c->body, 3);
  } else {
    int64_t sz = i->size;
    bool sgn = ty_is_signed_int(t);
    unsigned char op;
    int align;
    if (sz == 1) {
      op = sgn ? 0x2C : 0x2D;
      align = 0;
    } else if (sz == 2) {
      op = sgn ? 0x2E : 0x2F;
      align = 1;
    } else {
      op = 0x28; // i32.load (also pointers)
      align = 2;
    }
    w8(c->body, op);
    wuleb(c->body, align);
  }
  wuleb(c->body, 0);
  lset(c, i->dst);
}

static void emit_store(WFnCtx *c, IRIns *i) {
  lget(c, i->addr);
  lget(c, i->a);
  // op follows the VALUE's wasm type (pointers store as i32)
  IRType t = i->a->ty;
  if (t == IT_F32) {
    w8(c->body, 0x38);
    wuleb(c->body, 2);
  } else if (t == IT_F64) {
    w8(c->body, 0x39);
    wuleb(c->body, 3);
  } else if (w_is64(t)) {
    w8(c->body, 0x37); // i64.store
    wuleb(c->body, 3);
  } else {
    int64_t sz = i->size;
    w8(c->body, sz == 1 ? 0x3A : sz == 2 ? 0x3B : 0x36);
    wuleb(c->body, sz == 1 ? 0 : sz == 2 ? 1 : 2);
  }
  wuleb(c->body, 0);
}

static unsigned char arith_op(IROp op, IRType ty, bool sgn, bool flt) {
  bool is64 = w_is64(ty);
  switch (op) {
  case IR_ADD: return flt ? (ty == IT_F32 ? 0x92 : 0xA0) : (is64 ? 0x7C : 0x6A);
  case IR_SUB: return flt ? (ty == IT_F32 ? 0x91 : 0xA1) : (is64 ? 0x7D : 0x6B);
  case IR_MUL: return flt ? (ty == IT_F32 ? 0x94 : 0xA2) : (is64 ? 0x7E : 0x6C);
  case IR_DIV:
    if (flt)
      return ty == IT_F32 ? 0x95 : 0xA3;
    return is64 ? (sgn ? 0x7F : 0x80) : (sgn ? 0x6D : 0x6E);
  case IR_MOD: return is64 ? (sgn ? 0x81 : 0x82) : (sgn ? 0x6F : 0x70);
  case IR_AND: return is64 ? 0x83 : 0x71;
  case IR_OR: return is64 ? 0x84 : 0x72;
  case IR_XOR: return is64 ? 0x85 : 0x73;
  case IR_SHL: return is64 ? 0x86 : 0x74;
  case IR_SHR: return is64 ? (sgn ? 0x87 : 0x88) : (sgn ? 0x75 : 0x76);
  default: return 0x6A;
  }
}

static void emit_cmp(WFnCtx *c, IRIns *i) {
  lget(c, i->a);
  lget(c, i->b);
  if (i->is_float) {
    // eq ne lt gt le ge — f32 0x5B.., f64 0x61..
    static const unsigned char f32ops[6] = {0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60};
    static const unsigned char f64ops[6] = {0x61, 0x62, 0x63, 0x64, 0x65, 0x66};
    int idx = i->cc == CC_EQ ? 0 : i->cc == CC_NE ? 1 : i->cc == CC_LT ? 2
             : i->cc == CC_GT ? 3 : i->cc == CC_LE ? 4 : 5;
    w8(c->body, i->a->ty == IT_F32 ? f32ops[idx] : f64ops[idx]);
  } else {
    bool is64 = w_is64(i->a->ty);
    int base = is64 ? 0x51 : 0x46;
    int idx;
    switch (i->cc) {
    case CC_EQ: idx = 0; break;
    case CC_NE: idx = 1; break;
    case CC_LT: idx = i->signed_ops ? 2 : 3; break;
    case CC_LE: idx = i->signed_ops ? 6 : 7; break;
    case CC_GT: idx = i->signed_ops ? 4 : 5; break;
    case CC_GE: idx = i->signed_ops ? 8 : 9; break;
    default: idx = 0;
    }
    w8(c->body, (unsigned char)(base + idx));
  }
  lset(c, i->dst);
}

// integer div/mod with the zero and MIN/-1 guards the spec promises:
// divisor == 0 -> __panic_div; signed divisor == -1 -> 0 - a (wraps)
static void emit_int_divmod(WFnCtx *c, IRIns *i) {
  bool is64 = w_is64(i->a->ty);
  bool is_mod = i->op == IR_MOD;
  lget(c, i->b);
  w8(c->body, is64 ? 0x50 : 0x45); // eqz
  w8(c->body, 0x04);               // if void
  w8(c->body, 0x40);
  {
    const char *sym = prelude_symbol("__panic_div");
    WFn *t = map_get(&W.fn_by_symbol, str_from(sym));
    if (t) {
      w8(c->body, 0x10); // call
      wuleb(c->body, t->index);
    }
    w8(c->body, 0x00); // unreachable — panic never returns
  }
  w8(c->body, 0x05); // else
  if (i->signed_ops) {
    lget(c, i->b);
    if (is64) {
      w8(c->body, 0x42); // i64.const -1
      wsleb(c->body, -1);
      w8(c->body, 0x51); // i64.eq
    } else {
      w8(c->body, 0x41); // i32.const -1
      wsleb(c->body, -1);
      w8(c->body, 0x46); // i32.eq
    }
    w8(c->body, 0x04); // if void
    w8(c->body, 0x40);
    // dst = 0 - a (wraps: MIN / -1 -> MIN)
    if (is64)
      w8(c->body, 0x42), wsleb(c->body, 0);
    else
      w8(c->body, 0x41), wsleb(c->body, 0);
    lget(c, i->a);
    w8(c->body, is64 ? 0x7D : 0x6B); // sub
    lset(c, i->dst);
    w8(c->body, 0x05); // else
    lget(c, i->a);
    lget(c, i->b);
    w8(c->body, arith_op(i->op, i->a->ty, i->signed_ops, false));
    lset(c, i->dst);
    w8(c->body, 0x0B); // end
  } else {
    lget(c, i->a);
    lget(c, i->b);
    w8(c->body, arith_op(i->op, i->a->ty, i->signed_ops, false));
    lset(c, i->dst);
  }
  w8(c->body, 0x0B); // end if
}

static void table_index_of(const char *symbol) {
  if (map_has(&W.table_idx, str_from(symbol)))
    return;
  vec_push(&W.table_fns, (void *)symbol);
  map_put(&W.table_idx, str_from(symbol), (void *)(long)W.table_fns.n);
}

static void emit_ins(WFnCtx *c, IRIns *i) {
  switch (i->op) {
  case IR_CONST:
    if (w_is64(i->dst->ty)) {
      w8(c->body, 0x42);
      wsleb(c->body, (int64_t)i->imm);
    } else {
      w8(c->body, 0x41);
      wsleb(c->body, (int32_t)i->imm);
    }
    lset(c, i->dst);
    break;
  case IR_FCONST: {
    if (i->dst->ty == IT_F32) {
      float f = (float)i->fimm;
      uint32_t bits;
      memcpy(&bits, &f, 4);
      w8(c->body, 0x43);
      wbytes(c->body, &bits, 4);
    } else {
      uint64_t bits;
      memcpy(&bits, &i->fimm, 8);
      w8(c->body, 0x44);
      wbytes(c->body, &bits, 8);
    }
    lset(c, i->dst);
    break;
  }
  case IR_DIV: case IR_MOD:
    if (!i->is_float) {
      emit_int_divmod(c, i);
      break;
    }
    // fall through to the float path
  case IR_ADD: case IR_SUB: case IR_MUL:
  case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    lget(c, i->a);
    lget(c, i->b);
    // pointer(+/-)usize mixes i32 and i64 on wasm32: narrow the wide side
    // (wrap semantics; addresses live in the 32-bit memory space)
    if (!w_is64(i->a->ty) && w_is64(i->b->ty) && !i->is_float)
      w8(c->body, 0xA7); // i32.wrap_i64
    else if (w_is64(i->a->ty) && !w_is64(i->b->ty) && !i->is_float)
      w8(c->body, 0xAD); // i64.extend_i32_u — zero-extend the narrow side
    w8(c->body, arith_op(i->op, i->a->ty, i->signed_ops, i->is_float));
    lset(c, i->dst);
    break;
  case IR_CMP:
    emit_cmp(c, i);
    break;
  case IR_LOAD:
    emit_load(c, i);
    break;
  case IR_STORE:
    emit_store(c, i);
    break;
  case IR_ADDRC:
    if (i->lit == -1 && i->callee) {
      if (ir_fn_for_symbol(i->callee)) {
        table_index_of(i->callee);
        i32c(c, (long)map_get(&W.table_idx, str_from(i->callee)) - 1);
      } else {
        i32c(c, (long)map_get(&W.global_addr, str_from(i->callee)));
      }
      lset(c, i->dst);
    } else {
      // frame slot address: $fb + off
      w8(c->body, 0x20);
      wuleb(c->body, (uint64_t)c->fn->params.n); // local.get $fb
      i32c(c, slot_offset(c, i->slot));
      w8(c->body, 0x6A);
      lset(c, i->dst);
    }
    break;
  case IR_LITADDR: {
    IRLiteral *lit = c->fn->literals.items[i->lit];
    i32c(c, (long)map_get(&W.lit_addr,
                          str_from(arena_printf("%d:%s", c->fn->uid, lit->label))));
    lset(c, i->dst);
    break;
  }
  case IR_ADDI:
    lget(c, i->a);
    i32c(c, i->imm);
    w8(c->body, 0x6A);
    lset(c, i->dst);
    break;
  case IR_CALL: {
    if (i->callee) {
      // resolve first: a body-less rho fn (empty {}) is a no-op, and its
      // arg drops must not follow an already-emitted call opcode
      long import = strcmp(i->callee, "fd_write") == 0 ? 0
                    : strcmp(i->callee, "proc_exit") == 0 ? 1 : -1;
      WFn *t = NULL;
      if (import < 0) {
        t = map_get(&W.fn_by_symbol, str_from(i->callee));
        if (!t) {
          // args are never evaluated on this path (their vregs simply go
          // unconsumed), so only a expected result needs synthesizing
          if (i->dst) {
            if (i->dst->ty == IT_F32 || i->dst->ty == IT_F64) {
              w8(c->body, i->dst->ty == IT_F32 ? 0x43 : 0x44);
              for (int z = 0; z < (i->dst->ty == IT_F32 ? 4 : 8); z++)
                w8(c->body, 0);
            } else {
              w8(c->body, w_is64(i->dst->ty) ? 0x42 : 0x41);
              w8(c->body, 0x00);
            }
            lset(c, i->dst);
          }
          break;
        }
      }
      for (size_t k = 0; k < i->args.n; k++)
        lget(c, ((IRArg *)i->args.items[k])->vreg);
      w8(c->body, 0x10);
      if (import >= 0)
        wuleb(c->body, import);
      else
        wuleb(c->body, t->index);
    } else {
      for (size_t k = 0; k < i->args.n; k++)
        lget(c, ((IRArg *)i->args.items[k])->vreg);
      lget(c, i->callee_vreg);
      w8(c->body, 0x11); // call_indirect
      wuleb(c->body, w_type_of_call(i));
      wuleb(c->body, 0);
    }
    if (i->dst)
      lset(c, i->dst);
    break;
  }
  case IR_CAST: {
    lget(c, i->a);
    IRType from = i->cast_from, to = i->cast_to;
    bool sgn = ty_is_signed_int(from);
    switch (i->cast) {
    case CAST_TRUNC:
      if (w_is64(from) && !w_is64(to))
        w8(c->body, 0xA7); // i32.wrap_i64
      break;
    case CAST_SEXT:
      if (!w_is64(from) && w_is64(to))
        w8(c->body, 0xAC);
      break;
    case CAST_ZEXT:
      if (!w_is64(from) && w_is64(to))
        w8(c->body, 0xAD);
      break;
    case CAST_I2F:
      if (to == IT_F32)
        w8(c->body, w_is64(from) ? (sgn ? 0xBE : 0xBF) : (sgn ? 0xB4 : 0xB5));
      else
        w8(c->body, w_is64(from) ? (sgn ? 0xC0 : 0xC1) : (sgn ? 0xB7 : 0xB8));
      break;
    case CAST_F2I: {
      // saturating truncation (spec: out-of-range saturates)
      bool sgn_to = ty_is_signed_int(to);
      int op;
      if (w_is64(to))
        op = (from == IT_F32 ? 0x04 : 0x06) + (sgn_to ? 0 : 1);
      else
        op = (from == IT_F32 ? 0x00 : 0x02) + (sgn_to ? 0 : 1);
      w8(c->body, 0xFC);
      w8(c->body, (unsigned char)op);
      break;
    }
    case CAST_F32_F64:
      w8(c->body, 0xB9); // f64.promote_f32
      break;
    case CAST_F64_F32:
      w8(c->body, 0xB6); // f32.demote_f64
      break;
    case CAST_BITCOPY:
      // same-memory-size reinterpretations; across the wasm32 split the
      // pointer half is i32, so 64-bit ints need a width hop
      if (from == IT_F32 && to == IT_I32)
        w8(c->body, 0xBA);
      else if (from == IT_F64 && (to == IT_I64 || to == IT_USIZE))
        w8(c->body, 0xBB);
      else if (from == IT_I32 && to == IT_F32)
        w8(c->body, 0xBC);
      else if ((from == IT_I64 || from == IT_USIZE) && to == IT_F64)
        w8(c->body, 0xBD);
      else if (from == IT_PTR && w_is64(to))
        w8(c->body, 0xAD); // i64.extend_i32_u
      else if (w_is64(from) && to == IT_PTR)
        w8(c->body, 0xA7); // i32.wrap_i64
      break;
    }
    lset(c, i->dst);
    break;
  }
  case IR_COPYMEM:
    lget(c, i->addr);
    lget(c, i->a);
    i32c(c, i->size);
    w8(c->body, 0xFC);
    w8(c->body, 0x0A); // memory.copy
    wuleb(c->body, 0);
    wuleb(c->body, 0);
    break;
  case IR_ZERO:
    lget(c, i->addr);
    i32c(c, 0);
    i32c(c, i->size);
    w8(c->body, 0xFC);
    w8(c->body, 0x0B); // memory.fill
    wuleb(c->body, 0);
    break;
  }
}

// ---- structured control flow -----------------------------------------------------
// Each branch/join gets exactly ONE label for its lifetime: the first diamond
// that targets it pushes it, arms br to it, and its code is emitted once after
// the outermost construct that referenced it. Loop headers carry explicit
// flags from the lowering (structural guessing mis-fires on rc glue CFGs).

static size_t pred_count(WFnCtx *c, IRBlock *t) {
  size_t n = 0;
  for (size_t i = 0; i < c->fn->blocks.n; i++) {
    IRBlock *b = c->fn->blocks.items[i];
    if (!b->term)
      continue;
    if (b->term->op == (IROp)OP_BR && (IRBlock *)b->term->dst == t)
      n++;
    else if (b->term->op == (IROp)OP_CBR) {
      if ((IRBlock *)b->term->dst == t)
        n++;
      if ((IRBlock *)b->term->b == t)
        n++;
    }
  }
  return n;
}

static bool has_claim(WFnCtx *c, IRBlock *b) {
  if (!b)
    return false;
  for (size_t i = 0; i < c->claims.n; i++)
    if (((WClaim *)c->claims.items[i])->b == b)
      return true;
  return false;
}

static bool has_label(WFnCtx *c, IRBlock *b) {
  if (!b)
    return false;
  for (size_t i = 0; i < c->labels.n; i++)
    if (((WLabel *)c->labels.items[i])->b == b)
      return true;
  return false;
}

static bool has_target(WFnCtx *c, IRBlock *b) {
  return has_claim(c, b) || has_label(c, b);
}

static bool inlineable(WFnCtx *c, IRBlock *b) {
  return !b->loop_header && !has_target(c, b) && !b->emitted && pred_count(c, b) == 1;
}

// can block `target` be reached from `from` following terminators through
// unlabeled, single-pred blocks? (small CFGs; bounded walk)
static bool reaches(WFnCtx *c, IRBlock *from, IRBlock *target, int depth) {
  if (!from || depth > 32)
    return false;
  while (from) {
    if (from == target)
      return true;
    if (!from->term)
      return false;
    if (from->term->op == (IROp)OP_BR)
      from = (IRBlock *)from->term->dst;
    else if (from->term->op == (IROp)OP_RET)
      return false;
    else
      return reaches(c, (IRBlock *)from->term->dst, target, depth + 1) ||
             reaches(c, (IRBlock *)from->term->b, target, depth + 1);
  }
  return false;
}

// terminal br target of the chain starting at b (peek only), NULL on return
static IRBlock *chain_terminal(WFnCtx *c, IRBlock *b) {
  while (b) {
    if (getenv("RHO_WASM_DEBUG"))
      fprintf(stderr, "  ct(B%d) inlineable=%d pred=%u\n", b->id,
              inlineable(c, b), (unsigned)pred_count(c, b));
    if (!inlineable(c, b))
      return b; // join, loop header, or labeled target
    if (!b->term)
      return NULL;
    if (b->term->op == (IROp)OP_BR) {
      IRBlock *t = (IRBlock *)b->term->dst;
      if (inlineable(c, t))
        b = t;
      else
        return t;
    } else if (b->term->op == (IROp)OP_RET) {
      return b; // a shared RET block is a join like any other
    } else {
      IRBlock *r1 = chain_terminal(c, (IRBlock *)b->term->dst);
      IRBlock *r2 = chain_terminal(c, (IRBlock *)b->term->b);
      if (r1 == r2)
        return r1;
      if (r1 && r2) {
        // the arms' terminals differ: the true join is the deeper one the
        // other side eventually reaches
        int f12 = reaches(c, r1, r2, 0), f21 = reaches(c, r2, r1, 0);
        if (getenv("RHO_WASM_DEBUG"))
          fprintf(stderr, "  ct r1=B%d r2=B%d r12=%d r21=%d\n", r1->id, r2->id,
                  f12, f21);
        if (f12)
          return r2;
        if (f21)
          return r1;
      }
      return r1 ? r1 : r2;
    }
  }
  return NULL;
}

static void push_label(WFnCtx *c, int kind, IRBlock *b) {
  WLabel *l = arena_alloc(sizeof(WLabel));
  l->kind = kind;
  l->b = b;
  vec_push(&c->labels, l);
}

static void pop_label(WFnCtx *c) { c->labels.n--; }

static int br_depth(WFnCtx *c, IRBlock *t) {
  // claims first: br lands on the claiming diamond's own if — the last
  // construct opened since the claim — one below the raw count
  for (size_t i = c->claims.n; i > 0; i--) {
    WClaim *cl = c->claims.items[i - 1];
    if (cl->b == t)
      return (int)(c->labels.n - cl->base - 1);
  }
  for (size_t i = c->labels.n; i > 0; i--)
    if (((WLabel *)c->labels.items[i - 1])->b == t)
      return (int)(c->labels.n - i);
  return -1;
}

static void edge_copies(WFnCtx *c, IRBlock *from, IRBlock *t) {
  for (size_t p = 0; p < t->phis.n; p++) {
    IRPhi *phi = t->phis.items[p];
    for (size_t k = 0; k < phi->preds.n; k++)
      if (phi->preds.items[k] == from) {
        lget(c, phi->args.items[k]);
        lset(c, phi->dst);
      }
  }
}

static void emit_br(WFnCtx *c, IRBlock *from, IRBlock *t) {
  edge_copies(c, from, t);
  int d = br_depth(c, t);
  if (getenv("RHO_WASM_DEBUG"))
    fprintf(stderr, "  br %s->B%d depth=%d labels=%u\n", c->fn->symbol,
            t ? t->id : -1, d, (unsigned)c->labels.n);
  if (d < 0)
    d = 0;
  w8(c->body, 0x0C);
  wuleb(c->body, d);
}

// emits blocks from `b` (reached from `from`) until the region branches
// out or returns; returns the block the region branched to, or NULL.
// `from` is the phi-edge predecessor for the first block.
static IRBlock *emit_region(WFnCtx *c, IRBlock *b, IRBlock *from) {
  int started_loop = 0; // this invocation opened the loop labels
  IRBlock *my_header = NULL, *my_exit = NULL;
  while (b) {
    if (b->emitted || has_target(c, b)) {
      emit_br(c, from, b);
      return b;
    }
    if (b->loop_header) {
      // block $exit { loop $h { <body>; } } — the header's own term is
      // dispatched generically below; the body's terminal br back to the
      // header closes the loop, br to the exit breaks out
      my_exit = b->loop_exit;
      my_header = b;
      push_label(c, 0, my_exit); // block $exit
      push_label(c, 1, my_header); // loop $h
      w8(c->body, 0x02); // block
      w8(c->body, 0x40);
      w8(c->body, 0x03); // loop
      w8(c->body, 0x40);
      started_loop = 1;
    }
    b->emitted = true;
    for (size_t k = 0; k < b->ins.n; k++)
      emit_ins(c, b->ins.items[k]);
    if (!b->term) {
      if (started_loop) {
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
      }
      return NULL;
    }
    if (b->term->op == (IROp)OP_BR) {
      IRBlock *t = (IRBlock *)b->term->dst;
      // forward entry into a not-yet-emitted loop becomes the construct
      if (t->loop_header && !t->emitted) {
        from = b;
        b = t;
        continue;
      }
      if (t == my_header && started_loop) {
        // back edge: repeat the loop
        emit_br(c, b, t);
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
        if (!my_exit)
          return NULL;
        from = b;
        b = my_exit;
        started_loop = 0;
        my_header = my_exit = NULL;
        continue;
      }
      if (inlineable(c, t)) {
        from = b;
        b = t;
        continue;
      }
      emit_br(c, b, t);
      if (started_loop && t == my_exit) {
        // break: leave the loop, the exit's code follows in the caller
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
        return t;
      }
      return t;
    }
    if (b->term->op == (IROp)OP_RET) {
      // restore the shadow stack, then return (value stays on the stack)
      w8(c->body, 0x20);
      wuleb(c->body, (uint64_t)c->fn->params.n); // local.get $fb
      w8(c->body, 0x24);                         // global.set 0 ($sp)
      wuleb(c->body, 0);
      if (b->term->a)
        lget(c, b->term->a);
      w8(c->body, 0x0F); // return
      if (started_loop) {
        w8(c->body, 0x0B); // end loop (lexically required)
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
      }
      return NULL;
    }
    // cbr: if { then } else { else }; both arms land on the join via br,
    // whose label/code lives with the first diamond that claimed it
    IRBlock *t = (IRBlock *)b->term->dst;
    IRBlock *f = (IRBlock *)b->term->b;
    IRBlock *join = chain_terminal(c, t);
    IRBlock *join2 = chain_terminal(c, f);
    if (join != join2) {
      // the true join is the deeper terminal the other side reaches
      if (join && join2 && reaches(c, join, join2, 0))
        join = join2;
      else
        join = join ? join : join2; // one arm may return outright
    }
    bool claimed = false;
    if (join && !has_target(c, join) && !join->emitted) {
      WClaim *cl = arena_alloc(sizeof(WClaim));
      cl->b = join;
      cl->base = (int)c->labels.n;
      vec_push(&c->claims, cl);
      claimed = true;
    }
    push_label(c, 2, NULL); // the anonymous if occupies a br depth
    lget(c, b->term->a);
    w8(c->body, 0x04); // if
    w8(c->body, 0x40); // void blocktype
    emit_region(c, t, b);
    w8(c->body, 0x05); // else
    emit_region(c, f, b);
    w8(c->body, 0x0B); // end if
    pop_label(c);      // drop the if placeholder
    if (!join) {
      if (started_loop) {
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
      }
      return NULL; // both arms returned
    }
    if (join->emitted) {
      if (claimed)
        c->claims.n--; // ours went stale the moment it was emitted
      if (started_loop && join == my_exit) {
        // a break-if: both arms already br'd; close the loop, emit exit
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
        started_loop = 0;
        my_header = my_exit = NULL;
        from = b;
        b = join;
        continue;
      }
      return join; // an earlier construct emitted it
    }
    if (!claimed) {
      if (started_loop && join == my_exit) {
        // break-if against this loop's exit: close and continue at exit
        w8(c->body, 0x0B); // end loop
        w8(c->body, 0x0B); // end block
        pop_label(c);
        pop_label(c);
        started_loop = 0;
        my_header = my_exit = NULL;
        from = b;
        b = join;
        continue;
      }
      return join; // an enclosing construct owns the join and its code
    }
    c->claims.n--; // our claim: the join's code follows right here
    from = b;
    b = join;
  }
  return NULL;
}


static SB *emit_fn_body(WFn *wf) {
  IRFn *fn = wf->fn;
  WFnCtx c = {0};
  c.fn = fn;
  c.body = arena_alloc(sizeof(SB));

  // frame layout first (slots), then locals for every vreg
  for (size_t i = 0; i < fn->slots.n; i++)
    slot_offset(&c, fn->slots.items[i]);

  // prologue: $fb = $sp; $sp -= frame
  w8(c.body, 0x23); // global.get 0
  wuleb(c.body, 0);
  w8(c.body, 0x21); // local.set $fb
  wuleb(c.body, (uint64_t)fn->params.n);
  w8(c.body, 0x23); // global.get 0
  wuleb(c.body, 0);
  i32c(&c, c.frame);
  w8(c.body, 0x6B); // i32.sub
  w8(c.body, 0x24); // global.set 0
  wuleb(c.body, 0);

  // incoming params: store each wasm param local into its frame slot
  for (size_t i = 0; i < fn->params.n; i++) {
    IRSlot *ps = fn->params.items[i];
    Type *pt = fn->param_types.n > i ? fn->param_types.items[i] : NULL;
    bool agg = ty_travels_as_ptr(pt);
    // wasm store operand order: address first, value second
    w8(c.body, 0x20); // local.get $fb
    wuleb(c.body, (uint64_t)fn->params.n);
    i32c(&c, slot_offset(&c, ps));
    w8(c.body, 0x6A); // i32.add — address = $fb + off
    w8(c.body, 0x20);
    wuleb(c.body, i); // local.get <param i>
    if (agg || true) {
      // the pointer/aggregate handle itself lives in the slot
      IRType it = agg ? IT_PTR : w_ir_type(pt);
      if (it == IT_F64)
        w8(c.body, 0x39), wuleb(c.body, 3);
      else if (it == IT_F32)
        w8(c.body, 0x38), wuleb(c.body, 2);
      else if (w_is64(it))
        w8(c.body, 0x37), wuleb(c.body, 3);
      else
        w8(c.body, 0x36), wuleb(c.body, 2); // i32.store (also pointers)
      wuleb(c.body, 0);
    }
  }

  if (getenv("RHO_WASM_DEBUG")) {
    for (size_t bi = 0; bi < fn->blocks.n; bi++) {
      IRBlock *blk = fn->blocks.items[bi];
      fprintf(stderr, "FN %s B%d loop=%d exit=%s term=%d\n", fn->symbol, blk->id,
              (int)blk->loop_header,
              blk->loop_exit ? (char *)arena_printf("B%d", ((IRBlock *)blk->loop_exit)->id)
                             : "-",
              blk->term ? (int)blk->term->op : -1);
    }
  }
  emit_region(&c, fn->entry, fn->entry);
  w8(c.body, 0x0B); // end function body
  return c.body;
}

// collect each vreg's IRType (every vreg has exactly one def instruction)
static IRType vreg_type(IRFn *fn, int id) {
  for (size_t b = 0; b < fn->blocks.n; b++) {
    IRBlock *blk = fn->blocks.items[b];
    for (size_t k = 0; k < blk->ins.n; k++) {
      IRIns *i = blk->ins.items[k];
      if (i->dst && i->dst->id == id)
        return i->dst->ty;
    }
  }
  return IT_I32;
}

void emit_wasm(Target target, SB *out) {
  (void)target;
  if (g_ir_fns.n == 0)
    return;

  memset(&W, 0, sizeof(W));

  // register the two import signatures FIRST: sections are serialized in
  // order, so anything w_type_for adds during import assembly would land
  // past the already-written type section
  {
    Vec p = {0};
    for (int k = 0; k < 4; k++)
      vec_push(&p, (void *)(long)IT_I32);
    w_type_for(p, true, IT_I32); // fd_write
    p.n = 0;
    vec_push(&p, (void *)(long)IT_I32);
    w_type_for(p, false, IT_I32); // proc_exit
  }

  // function table (indices 0,1 are the wasi imports)
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    WFn *wf = arena_alloc(sizeof(WFn));
    wf->fn = fn;
    wf->index = (int)(2 + i);
    wf->type = w_type_of_fn(fn);
    vec_push(&W.fns, wf);
    map_put(&W.fn_by_symbol, str_from(fn->symbol), wf);
  }
  w_layout_data();

  // address-taken functions need table entries: scan every instruction
  for (size_t i = 0; i < g_ir_fns.n; i++) {
    IRFn *fn = g_ir_fns.items[i];
    for (size_t b = 0; b < fn->blocks.n; b++) {
      IRBlock *blk = fn->blocks.items[b];
      for (size_t k = 0; k < blk->ins.n; k++) {
        IRIns *ins = blk->ins.items[k];
        if (ins->op == IR_ADDRC && ins->lit == -1 && ins->callee &&
            ir_fn_for_symbol(ins->callee))
          table_index_of(ins->callee);
      }
    }
  }

  // ---- assemble the module ----
  SB typesec = {0}, importsec = {0}, funcsec = {0}, tablesec = {0},
     memsec = {0}, globsec = {0}, exportsec = {0}, elemsec = {0},
     codesec = {0}, datasec = {0};

  // type section
  {
    SB body = {0};
    wuleb(&body, W.types.n);
    for (size_t i = 0; i < W.types.n; i++) {
      WType *t = W.types.items[i];
      w8(&body, 0x60);
      wuleb(&body, t->params.n);
      for (size_t k = 0; k < t->params.n; k++)
        w8(&body, w_vt((IRType)(long)t->params.items[k]));
      if (t->has_ret) {
        wuleb(&body, 1);
        w8(&body, w_vt(t->ret));
      } else {
        wuleb(&body, 0);
      }
    }
    wsection(&typesec, 1, &body);
  }

  // import section: fd_write, proc_exit
  {
    SB body = {0};
    wuleb(&body, 2);
    wstr(&body, "wasi_snapshot_preview1");
    wstr(&body, "fd_write");
    w8(&body, 0x00); // func
    {
      Vec p = {0};
      for (int k = 0; k < 4; k++)
        vec_push(&p, (void *)(long)IT_I32);
      wuleb(&body, w_type_for(p, true, IT_I32));
    }
    wstr(&body, "wasi_snapshot_preview1");
    wstr(&body, "proc_exit");
    w8(&body, 0x00);
    {
      Vec p = {0};
      vec_push(&p, (void *)(long)IT_I32);
      wuleb(&body, w_type_for(p, false, IT_I32));
    }
    wsection(&importsec, 2, &body);
  }

  // function section
  {
    SB body = {0};
    wuleb(&body, W.fns.n + 1); // + _start
    for (size_t i = 0; i < W.fns.n; i++)
      wuleb(&body, ((WFn *)W.fns.items[i])->type);
    // _start: () -> ()
    {
      Vec p = {0};
      wuleb(&body, w_type_for(p, false, IT_I32));
    }
    wsection(&funcsec, 3, &body);
  }

  // table section: always present — rc_dec's drop-glue call_indirect needs
  // table 0 to exist even in programs that never take a function address
  bool have_table = true;
  {
    SB body = {0};
    wuleb(&body, 1);
    w8(&body, 0x70); // funcref
    w8(&body, 0x00); // min only
    wuleb(&body, W.table_fns.n > 0 ? W.table_fns.n : 1);
    wsection(&tablesec, 4, &body);
  }

  // memory section
  {
    SB body = {0};
    uint64_t min_pages = (uint64_t)((W.stack_top + 0xFFFF) / 0x10000 + 1);
    wuleb(&body, 1);
    w8(&body, 0x00);
    wuleb(&body, min_pages);
    wsection(&memsec, 5, &body);
  }

  // global section: $sp (mut i32) = stack top
  {
    SB body = {0};
    wuleb(&body, 1);
    w8(&body, 0x7F); // i32
    w8(&body, 0x01); // mutable
    w8(&body, 0x41); // i32.const
    wsleb(&body, W.stack_top);
    w8(&body, 0x0B); // end
    wsection(&globsec, 6, &body);
  }

  // export section: memory + _start
  int start_index = (int)(2 + W.fns.n);
  {
    SB body = {0};
    wuleb(&body, 2);
    wstr(&body, "memory");
    w8(&body, 0x02); // memory
    wuleb(&body, 0);
    wstr(&body, "_start");
    w8(&body, 0x00); // func
    wuleb(&body, start_index);
    wsection(&exportsec, 7, &body);
  }

  // element section
  if (have_table) {
    SB body = {0};
    wuleb(&body, 1);
    wuleb(&body, 0); // table 0
    w8(&body, 0x41); // i32.const 0
    wsleb(&body, 0);
    w8(&body, 0x0B);
    wuleb(&body, W.table_fns.n);
    for (size_t i = 0; i < W.table_fns.n; i++) {
      const char *sym = W.table_fns.items[i];
      WFn *t = map_get(&W.fn_by_symbol, str_from(sym));
      wuleb(&body, t ? (uint64_t)t->index : 0);
    }
    wsection(&elemsec, 9, &body);
  }

  // code section: [locals decl][body] per function, size-prefixed
  {
    SB body = {0};
    wuleb(&body, W.fns.n + 1);
    for (size_t i = 0; i < W.fns.n; i++) {
      WFn *wf = W.fns.items[i];
      IRFn *fn = wf->fn;
      SB *code = emit_fn_body(wf);
      // locals: one group per local so indices stay params, $fb, then vregs
      // in id order. Params occupy locals 0..n-1 via the SIGNATURE (not the
      // decl); the decl covers $fb and the vregs only.
      SB entry = {0};
      wuleb(&entry, 1 + fn->next_vreg);
      wuleb(&entry, 1);
      w8(&entry, 0x7F); // $fb
      for (int v = 0; v < fn->next_vreg; v++) {
        wuleb(&entry, 1);
        w8(&entry, w_vt(vreg_type(fn, v)));
      }
      Str cd = sb_finish(code);
      sb_append(&entry, cd);
      Str e = sb_finish(&entry);
      wuleb(&body, e.n);
      sb_append(&body, e);
    }
    // _start: call main; call proc_exit
    {
      SB entry = {0};
      wuleb(&entry, 0); // no locals
      WFn *mainf = map_get(&W.fn_by_symbol, str_from(g_main_symbol));
      w8(&entry, 0x10);
      wuleb(&entry, mainf ? (uint64_t)mainf->index : 0);
      w8(&entry, 0x10);
      wuleb(&entry, 1); // proc_exit
      w8(&entry, 0x0B); // (unreachable after proc_exit, but valid)
      Str e = sb_finish(&entry);
      wuleb(&body, e.n);
      sb_append(&body, e);
    }
    wsection(&codesec, 10, &body);
  }

  // data section: statics + literals (24B immortal header + bytes)
  {
    SB body = {0};
    size_t count = 0;
    for (size_t g = 0; g < g_ir_globals.n; g++) {
      IRGlobal *gl = g_ir_globals.items[g];
      if (!gl->is_extern && gl->init.n)
        count++;
    }
    for (size_t f = 0; f < g_ir_fns.n; f++)
      count += ((IRFn *)g_ir_fns.items[f])->literals.n;
    wuleb(&body, count);
    for (size_t g = 0; g < g_ir_globals.n; g++) {
      IRGlobal *gl = g_ir_globals.items[g];
      if (gl->is_extern || !gl->init.n)
        continue;
      wuleb(&body, 0); // memory 0
      w8(&body, 0x41);
      wsleb(&body, (long)map_get(&W.global_addr, str_from(gl->symbol)));
      w8(&body, 0x0B);
      wuleb(&body, gl->init.n);
      wvec_bytes(&body, &gl->init);
    }
    for (size_t f = 0; f < g_ir_fns.n; f++) {
      IRFn *fn = g_ir_fns.items[f];
      for (size_t l = 0; l < fn->literals.n; l++) {
        IRLiteral *lit = fn->literals.items[l];
        wuleb(&body, 0);
        w8(&body, 0x41);
        wsleb(&body, (long)map_get(&W.lit_addr,
                                   str_from(arena_printf("%d:%s", fn->uid, lit->label))));
        w8(&body, 0x0B);
        wuleb(&body, 24 + lit->bytes.n);
        // 24-byte immortal rc header: {sentinel, 0, 0}
        for (int k = 0; k < 24; k++) {
          unsigned char b = (k == 7) ? 0x80 : 0; // little-endian sentinel top byte
          w8(&body, b);
        }
        wvec_bytes(&body, &lit->bytes);
      }
    }
    wsection(&datasec, 11, &body);
  }

  // module header + sections in order
  wbytes(out, "\0asm", 4);
  w8(out, 1);
  w8(out, 0);
  w8(out, 0);
  w8(out, 0);
  Str s;
  s = sb_finish(&typesec);
  sb_append(out, s);
  s = sb_finish(&importsec);
  sb_append(out, s);
  s = sb_finish(&funcsec);
  sb_append(out, s);
  s = sb_finish(&tablesec);
  sb_append(out, s);
  s = sb_finish(&memsec);
  sb_append(out, s);
  s = sb_finish(&globsec);
  sb_append(out, s);
  s = sb_finish(&exportsec);
  sb_append(out, s);
  s = sb_finish(&elemsec);
  sb_append(out, s);
  s = sb_finish(&codesec);
  sb_append(out, s);
  s = sb_finish(&datasec);
  sb_append(out, s);
}
