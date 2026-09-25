// emit.c — the wasm32-wasi emitter (T1.7): emits WAT text; the pinned
// wat2wasm assembles it. The WAT is a pure function of the checked
// program (byte-identical determinism, spec §8).
//
// ABI: a rho value occupies one or more wasm locals (a vreg names the
// first). Params/results use the same flattened shapes (multi-value).
// Managed values follow the pure-local rc rules: retain per copy, one
// release per destruction; drop runs at rc==0 (spec §1).
#include "sem.h"

#include <stdarg.h>

extern const char *kernel_wat_src(void); // kernel_wat.c

// ================================================================ text

typedef struct Buf {
  char *p;
  size_t n, cap;
} Buf;

static void buf_init(Buf *b) {
  b->cap = 1 << 14;
  b->n = 0;
  b->p = malloc(b->cap);
}
static void tneed(Buf *b, size_t k) {
  if (b->n + k > b->cap) {
    while (b->cap < b->n + k)
      b->cap *= 2;
    b->p = realloc(b->p, b->cap);
  }
}
static void tputs(Buf *b, const char *s) {
  size_t n = strlen(s);
  tneed(b, n);
  memcpy(b->p + b->n, s, n);
  b->n += n;
}
static void tprintf(Buf *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char stackbuf[512];
  char *dyn = NULL;
  char *buf = (size_t)n < sizeof stackbuf ? stackbuf
                                          : (dyn = malloc((size_t)n + 1));
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  tneed(b, (size_t)n);
  memcpy(b->p + b->n, buf, (size_t)n);
  b->n += (size_t)n;
  free(dyn);
}

// ================================================================ shapes

typedef enum { W_I32 = 0, W_I64 = 1, W_F32 = 2, W_F64 = 3 } WTy;

static const char *wty_s(WTy w) {
  switch (w) {
  case W_I64: return "i64";
  case W_F32: return "f32";
  case W_F64: return "f64";
  default: return "i32";
  }
}

static WTy scalar_wty(Type *t) {
  switch (t->kind) {
  case TY_I64: case TY_U64: return W_I64;
  case TY_F32: return W_F32;
  case TY_F64: return W_F64;
  default: return W_I32;
  }
}

__attribute__((unused)) static bool ty_narrow32(Type *t) {
  return t->kind == TY_I8 || t->kind == TY_U8 || t->kind == TY_I16 ||
         t->kind == TY_U16 || t->kind == TY_I32 || t->kind == TY_U32;
}

static size_t shape_nlocals(Type *t) {
  switch (t->kind) {
  case TY_PTR: return 1;
  case TY_STRING: case TY_SLICE: case TY_DYN: case TY_FN: return 2;
  case TY_STRUCT: {
    size_t n = 0;
    for (size_t i = 0; i < t->sdef->nfields; i++)
      n += shape_nlocals(t->sdef->fields[i].ty);
    return n ? n : 1;
  }
  case TY_ENUM: {
    size_t max = 0;
    for (size_t i = 0; i < t->edef->nvariants; i++) {
      size_t n = 0;
      for (size_t k = 0; k < t->edef->variants[i].nfields; k++)
        n += shape_nlocals(t->edef->variants[i].fields[k].ty);
      if (n > max) max = n;
    }
    return 1 + max;
  }
  default: return 1;
  }
}

static WTy local_wty_rec(Type *t, size_t j, size_t *base) {
  switch (t->kind) {
  case TY_STRUCT:
    for (size_t i = 0; i < t->sdef->nfields; i++) {
      WTy w = local_wty_rec(t->sdef->fields[i].ty, j, base);
      if (*base > j) return w;
    }
    return W_I32;
  case TY_ENUM: {
    if (j == *base) { (*base)++; return W_I32; }
    (*base)++;
    for (size_t i = 0; i < t->edef->nvariants; i++)
      for (size_t k = 0; k < t->edef->variants[i].nfields; k++) {
        WTy w = local_wty_rec(t->edef->variants[i].fields[k].ty, j, base);
        if (*base > j) return w;
      }
    return W_I32;
  }
  default:
    if (j == *base) { (*base)++; return scalar_wty(t); }
    (*base)++;
    return W_I32;
  }
}

static WTy local_wty(Type *t, size_t j) {
  size_t base = 0;
  return local_wty_rec(t, j, &base);
}

// ================================================================ module

#define DATA_BASE 4096   // data literals start here
#define FMT_LO 64

typedef struct DataEnt {
  const char *bytes; // arena copy
  size_t n;
  size_t at;         // assigned address
} DataEnt;

typedef struct EFn {
  const char *wat; // full "(func …)" text, arena/malloc owned
  size_t wat_len;
} EFn;

typedef struct Em {
  Buf o;               // assembled module text
  Buf fnbuf;           // all "(func …)" texts concatenated
  Buf datasegs;        // "(data …)" texts
  Vec data;            // of DataEnt, dedup by content
  Vec fns;             // of EFn, emission order
  size_t data_off;     // next free data address
  Program *p;
  Vec dropfns;         // of Type* (struct types needing a drop fn)
  Vec emitted;         // of FnDef* already emitted (idempotence)
  size_t table_next;   // next funcref table index (drop fns first)
  bool debug;
  size_t heap_min;     // pages
} Em;

static Em *em_cur; // current emitter (single-shot compiler)

static void em_init(Em *em, Program *p, bool debug) {
  memset(em, 0, sizeof *em);
  (void)p; (void)debug;
  buf_init(&em->o);
  buf_init(&em->fnbuf);
  buf_init(&em->datasegs);
  vec_init(&em->data, sizeof(DataEnt));
  vec_init(&em->fns, sizeof(EFn));
  vec_init(&em->dropfns, sizeof(Type *));
  vec_init(&em->emitted, sizeof(FnDef *));
  em->data_off = DATA_BASE;
  em->p = p;
  em->debug = debug;
  em->table_next = 4; // kernel drop table reserves 0..3
}

// intern a data literal; returns its address
static size_t data_intern(const void *bytes, size_t n) {
  Em *em = em_cur;
  for (size_t i = 0; i < VLEN(em->data); i++) {
    DataEnt *d = VAT(em->data, DataEnt, i);
    if (d->n == n && memcmp(d->bytes, bytes, n) == 0)
      return d->at;
  }
  DataEnt *d = vec_push(&em->data);
  char *copy = arena_alloc(g_arena, n ? n : 1, 1);
  memcpy(copy, bytes, n);
  d->bytes = copy;
  d->n = n;
  d->at = em->data_off;
  em->data_off += (n + 15) & ~(size_t)15; // 16-align entries
  return d->at;
}

// escape a data string for WAT "(data (i32.const N) \"…\")"
static void wat_escape(Buf *b, const void *bytes, size_t n) {
  const uint8_t *p = bytes;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (c >= 32 && c < 127 && c != '"' && c != '\\')
      tprintf(b, "%c", c);
    else
      tprintf(b, "\\%02x", c);
  }
}

// ================================================================ fn ctx

typedef struct VarInfo {
  NodeRef node;   // let/param decl node (name on it)
  size_t vreg;
  bool managed;   // needs release at scope exit
  size_t scope;   // scope level
  Type *ty;       // the value type (release/retain need it)
} VarInfo;

typedef struct DeferEnt {
  NodeRef stmt;
  size_t scope;
} DeferEnt;

typedef struct LabEnt {
  const char *name;  // NULL = innermost unnamed
  int brk_depth;
  int cont_depth;
  size_t scope;      // scope level inside the loop body
} LabEnt;

typedef struct FnCx {
  Buf b;              // body text
  Em *em;
  Program *p;
  FnDef *fn;
  const char *wname;  // wat symbol ($name or mangled)
  Vec vars;           // of VarInfo
  Vec defers;         // of DeferEnt
  Vec labels;         // of LabEnt
  size_t scope;
  int depth;          // wasm block depth counter
  size_t nlocals;     // locals allocated (fresh vreg counter)
  Vec localtypes;     // of WTy — every local's wasm type
  Type *ret;
} FnCx;

static void cx_init(FnCx *cx, Em *em, FnDef *fn, const char *wname) {
  memset(cx, 0, sizeof *cx);
  buf_init(&cx->b);
  cx->em = em;
  cx->p = em->p;
  cx->fn = fn;
  cx->wname = wname;
  vec_init(&cx->vars, sizeof(VarInfo));
  vec_init(&cx->defers, sizeof(DeferEnt));
  vec_init(&cx->labels, sizeof(LabEnt));
  vec_init(&cx->localtypes, sizeof(WTy));
  cx->ret = fn ? fn->sig->ret : ty_unit;
}

static size_t cx_fresh(FnCx *cx, Type *t) {
  size_t n = shape_nlocals(t);
  size_t at = VLEN(cx->localtypes);
  for (size_t i = 0; i < n; i++)
    *VPUSH(cx->localtypes, WTy) = local_wty(t, i);
  return at;
}

// variable lookup by name
static VarInfo *cx_var(FnCx *cx, const char *name) {
  for (size_t i = VLEN(cx->vars); i > 0; i--) {
    VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
    if (strcmp(node_get(v->node)->name, name) == 0)
      return v;
  }
  return NULL;
}

// ================================================================ helpers

static void op(FnCx *cx, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char stackbuf[512];
  char *dyn = NULL;
  char *buf = (size_t)n < sizeof stackbuf ? stackbuf
                                          : (dyn = malloc((size_t)n + 1));
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  tprintf(&cx->b, "%s", buf);
  free(dyn);
}

// local access text
static const char *L(FnCx *cx, size_t v) {
  (void)cx;
  static _Thread_local char bufs[8][32];
  static _Thread_local int alt;
  alt = (alt + 1) & 7;
  snprintf(bufs[alt], sizeof bufs[alt], "(local.get %zu)", v);
  return bufs[alt];
}

// ============================================================ emit exprs

static void emit_expr(FnCx *cx, NodeRef er, size_t dst);
size_t static_slot(Module *m, const char *name);
static void emit_printf(FnCx *cx, Node *call, bool err);
static void emit_call_args(FnCx *cx, FnDef *f, RefList *args);
static void emit_stmt(FnCx *cx, NodeRef sr);
static void emit_scope_exit(FnCx *cx, size_t to_scope);
static const char *fn_wat_name(FnDef *f) __attribute__((unused));

// move/copy helpers for managed values
__attribute__((unused)) static void retain(FnCx *cx, Type *t, size_t vreg) {
  if (!t)
    return;
  switch (t->kind) {
  case TY_PTR: case TY_STRING: case TY_SLICE: case TY_DYN:
    op(cx, "(call $rho_retain %s)\n", L(cx, vreg));
    break;
  default:
    break;
  }
}
static void release(FnCx *cx, Type *t, size_t vreg) {
  if (!t)
    return; // checker already reported; keep emitting
  switch (t->kind) {
  case TY_PTR: case TY_STRING: case TY_SLICE: case TY_DYN:
    op(cx, "(call $rho_release %s)\n", L(cx, vreg));
    break;
  default:
    break;
  }
}

// narrow-int truncation after arithmetic (values stay in range)
static void truncate_after(FnCx *cx, Type *t, size_t v) {
  switch (t->kind) {
  case TY_I8: case TY_U8:
    op(cx, "(local.set %zu (i32.and (local.get %zu) (i32.const 255)))\n",
       v, v);
    break;
  case TY_I16: case TY_U16:
    op(cx, "(local.set %zu (i32.and (local.get %zu) (i32.const 65535)))\n",
       v, v);
    break;
  default:
    break;
  }
}

// signedness-aware division with the panic catalog (MIN/-1 = MIN, /0 %0
// panic per spec §4)
__attribute__((unused)) static void emit_div(FnCx *cx, int opkind, Type *t, size_t out, size_t a,
                     size_t b) {
  bool is64 = scalar_wty(t) == W_I64;
  const char *w = is64 ? "i64" : "i32";
  bool uns = t->kind >= TY_U8;
  // divisor zero → panic "division by zero"
  op(cx, "(if %s.eqz (local.get %zu) (then\n", w, b);
  size_t msg = data_intern("division by zero", 18);
  op(cx, "  (call $rho_panic (i32.const %zu) (i32.const 18))))\n", msg);
  if (opkind == OP_DIV) {
    if (uns)
      op(cx, "(local.set %zu (%s.div_u (local.get %zu) (local.get %zu)))\n",
         out, w, a, b);
    else
      op(cx, "(local.set %zu (%s.div_s (local.get %zu) (local.get %zu)))\n",
         out, w, a, b);
  } else {
    if (uns)
      op(cx, "(local.set %zu (%s.rem_u (local.get %zu) (local.get %zu)))\n",
         out, w, a, b);
    else
      op(cx, "(local.set %zu (%s.rem_s (local.get %zu) (local.get %zu)))\n",
         out, w, a, b);
  }
  if (is64 && !uns) {
    // wasm traps on MIN/-1: convert to the defined wrap (MIN)
    // -- check divisor == -1: result = MIN
    op(cx, "(if (%s.eq (local.get %zu) (%s.const -1)) (then\n", w, b, w);
    op(cx, "  (local.set %zu (%s.const 0x8000000000000000))))\n", out, w);
  }
}

// struct field memory offsets (boxed layout)
typedef struct Layout {
  size_t size; // payload size (excl. the 24-byte header)
} Layout;

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static size_t type_align(Type *t) {
  switch (t->kind) {
  case TY_I64: case TY_U64: case TY_F64: return 8;
  case TY_STRUCT: {
    size_t a = 4;
    for (size_t i = 0; i < t->sdef->nfields; i++) {
      size_t fa = type_align(t->sdef->fields[i].ty);
      if (fa > a) a = fa;
    }
    return a;
  }
  default: return 4;
  }
}

static size_t type_size(Type *t) {
  switch (t->kind) {
  case TY_I8: case TY_U8: case TY_I16: case TY_U16: case TY_I32:
  case TY_U32: case TY_USIZE: case TY_BOOL: case TY_F32:
  case TY_PTR: return 4;
  case TY_I64: case TY_U64: case TY_F64: return 8;
  case TY_STRING: case TY_SLICE: case TY_DYN: case TY_FN: return 8;
  case TY_STRUCT: {
    size_t off = 0;
    for (size_t i = 0; i < t->sdef->nfields; i++) {
      off = align_up(off, type_align(t->sdef->fields[i].ty));
      off += type_size(t->sdef->fields[i].ty);
    }
    return align_up(off ? off : 4, type_align(t));
  }
  case TY_ENUM: {
    size_t off = 4; // tag
    size_t max = 0;
    for (size_t i = 0; i < t->edef->nvariants; i++)
      for (size_t k = 0; k < t->edef->variants[i].nfields; k++)
        max += type_size(t->edef->variants[i].fields[k].ty);
    return off + max;
  }
  default: return 4;
  }
}

__attribute__((unused)) static size_t field_offset(Type *st, size_t idx) {
  size_t off = 0;
  for (size_t i = 0; i < idx; i++) {
    off = align_up(off, type_align(st->sdef->fields[i].ty));
    off += type_size(st->sdef->fields[i].ty);
  }
  return align_up(off, type_align(st->sdef->fields[idx].ty));
}

// numeric const from a folded value stored in a node's type-checked
// literal — emit constants directly
static void emit_const_to(FnCx *cx, Node *e, Type *t, size_t dst) {
  switch (t->kind) {
  case TY_BOOL:
    op(cx, "(local.set %zu (i32.const %d))\n", dst, e->kind == NT_BOOL
                       ? (e->bval ? 1 : 0) : 0);
    break;
  case TY_I64: case TY_U64: {
    uint64_t v = 0;
    if (e->kind == NT_INT) v = e->ival;
    else if (e->kind == NT_UNARY) v = (uint64_t)(0 - (int64_t)e->ival);
    else if (e->kind == NT_FLOAT) v = (uint64_t)(int64_t)e->fval;
    op(cx, "(local.set %zu (i64.const %llu))\n", dst,
       (unsigned long long)v);
    break;
  }
  case TY_F32:
    op(cx, "(local.set %zu (f32.const %.9g))\n", dst,
       e->kind == NT_FLOAT ? e->fval : (double)(int64_t)e->ival);
    break;
  case TY_F64:
    op(cx, "(local.set %zu (f64.const %.17g))\n", dst,
       e->kind == NT_FLOAT ? e->fval : (double)(int64_t)e->ival);
    break;
  default: { // 32-bit ints (wrap into range)
    uint64_t v = 0;
    if (e->kind == NT_INT) v = e->ival;
    else if (e->kind == NT_UNARY) v = (uint64_t)(0 - (int64_t)e->ival);
    else if (e->kind == NT_FLOAT) v = (uint64_t)(int64_t)e->fval;
    truncate_after(cx, t, dst);
    op(cx, "(local.set %zu (i32.const %d))\n", dst, (int32_t)(uint32_t)v);
    truncate_after(cx, t, dst);
  }
  }
}

// register a struct type's drop fn (deterministic first-use order)
__attribute__((unused)) static size_t dropfn_for(Type *t) {
  (void)t;
  return 0; // real drop-fn registry lands with boxed structs (T1.7b)
}

static void emit_expr(FnCx *cx, NodeRef er, size_t dst) {
  if (er == NO_REF)
    return;
  Node *e = node_get(er);
  Type *t = (Type *)e->sem;

  switch (e->kind) {
  case NT_INT:
  case NT_FLOAT:
  case NT_BOOL:
    emit_const_to(cx, e, t, dst);
    return;

  case NT_STR: {
    size_t at = data_intern(e->sval.p, e->sval.n);
    op(cx, "(local.set %zu (i32.const %zu))\n", dst, at);
    op(cx, "(local.set %zu (i32.const %zu))\n", dst + 1, e->sval.n);
    return;
  }

  case NT_PATH: {
    VarInfo *v = cx_var(cx, e->name);
    if (v) {
      size_t n = shape_nlocals(t);
      for (size_t i = 0; i < n; i++)
        op(cx, "(local.set %zu %s)\n", dst + i,
           L(cx, v->vreg + i));
      return;
    }
    // const/static value: emitted as constants (ints) or data (strings)
    // via the folded value in the const table
    Sym *s = symtab_get(cx->p->entry->syms, e->name);
    Module *m = cx->fn->mod;
    if (!s) s = symtab_get(m->syms, e->name);
    if (!s) s = symtab_get(g_prelude_mod->syms, e->name);
    if (s && s->kind == SYM_CONST && s->u.konst->cval) {
      CVal *cv = s->u.konst->cval;
      switch (cv->kind) {
      case 0: case 1: { // ints
        if (scalar_wty(t) == W_I64)
          op(cx, "(local.set %zu (i64.const %lld))\n", dst,
             (long long)cv->i);
        else {
          op(cx, "(local.set %zu (i32.const %d))\n", dst, (int32_t)cv->u);
          truncate_after(cx, t, dst);
        }
        break;
      }
      case 2: // float
        if (t->kind == TY_F32)
          op(cx, "(local.set %zu (f32.const %.9g))\n", dst, cv->f);
        else
          op(cx, "(local.set %zu (f64.const %.17g))\n", dst, cv->f);
        break;
      case 3:
        op(cx, "(local.set %zu (i32.const %d))\n", dst, cv->b ? 1 : 0);
        break;
      default: { // string
        size_t at = data_intern(cv->s.p, cv->s.n);
        op(cx, "(local.set %zu (i32.const %zu))\n", dst, at);
        op(cx, "(local.set %zu (i32.const %zu))\n", dst + 1, cv->s.n);
      }
      }
      return;
    }
    if (s && s->kind == SYM_STATIC) {
      size_t slot = static_slot(m, e->name);
      size_t n = shape_nlocals(t);
      for (size_t i = 0; i < n; i++)
        op(cx, "(local.set %zu (i32.load (i32.const %zu)))\n", dst + i,
           slot + i * 4);
      return;
    }
    // unknown here: the checker has already errored
    return;
  }

  case NT_CALL: {
    Node *callee = node_get(e->a);
    if (callee->kind != NT_PATH) {
      op(cx, ";; call of non-name (T1.7c)\n");
      return;
    }
    Sym *ps = g_prelude_mod->syms ? symtab_get(g_prelude_mod->syms,
                                               callee->name)
                                  : NULL;
    bool is_pre = ps && ps->kind == SYM_FN && ps->u.fns &&
                  ps->u.fns->mod == g_prelude_mod;
    if (is_pre && strcmp(callee->name, "printf") == 0) {
      emit_printf(cx, e, false);
      return;
    }
    if (is_pre && strcmp(callee->name, "eprintf") == 0) {
      emit_printf(cx, e, true);
      return;
    }
    if (is_pre && strcmp(callee->name, "panic") == 0) {
      Node *aw = node_get(reflist_at(e->list, 0));
      size_t v = cx_fresh(cx, ty_string);
      emit_expr(cx, aw->a, v);
      op(cx, "(call $rho_panic %s %s)\n", L(cx, v), L(cx, v + 1));
      return;
    }
    if (is_pre && strcmp(callee->name, "exit") == 0) {
      Node *aw = node_get(reflist_at(e->list, 0));
      size_t v = cx_fresh(cx, ty_i32);
      emit_expr(cx, aw->a, v);
      op(cx, "(call $proc_exit %s)\n", L(cx, v));
      return;
    }
    // plain call: the checker's chosen FnDef rides on ->sem2
    FnDef *f = (FnDef *)e->sem2;
    if (!f) {
      op(cx, ";; unresolved call %s\n", callee->name);
      return;
    }
    emit_call_args(cx, f, e->list);
    op(cx, "(call $%s)\n", f->name);
    // move stack results into dst: multi-value results pop in order;
    // set in reverse so the first result lands at dst
    Type *tres = f->sig->ret;
    if (tres->kind != TY_UNIT) {
      size_t n = shape_nlocals(tres);
      for (size_t i = n; i > 0; i--)
        op(cx, "(local.set %zu)\n", dst + i - 1);
    }
    return;
  }

  case NT_UNARY: {
    if (e->op == OP_NEG &&
        (node_get(e->a)->kind == NT_INT || node_get(e->a)->kind == NT_FLOAT)) {
      emit_const_to(cx, e, t, dst); // -literal folds at emit
      return;
    }
    size_t v = cx_fresh(cx, t);
    emit_expr(cx, e->a, v);
    switch (e->op) {
    case OP_NEG:
      if (t->kind == TY_F32)
        op(cx, "(local.set %zu (f32.neg (local.get %zu)))\n", dst, v);
      else if (t->kind == TY_F64)
        op(cx, "(local.set %zu (f64.neg (local.get %zu)))\n", dst, v);
      else if (scalar_wty(t) == W_I64)
        op(cx, "(local.set %zu (i64.sub (%s.const 0) (local.get %zu)))\n",
           dst, scalar_wty(t) == W_I64 ? "i64" : "i32", v);
      else
        op(cx, "(local.set %zu (i32.sub (i32.const 0) (local.get %zu)))\n",
           dst, v);
      truncate_after(cx, t, dst);
      return;
    case OP_NOT:
      op(cx, "(local.set %zu (i32.eqz (local.get %zu)))\n", dst, v);
      return;
    case OP_BITNOT:
      if (scalar_wty(t) == W_I64)
        op(cx, "(local.set %zu (i64.xor (i64.const -1) (local.get %zu)))\n",
           dst, v);
      else {
        op(cx, "(local.set %zu (i32.xor (i32.const -1) (local.get %zu)))\n",
           dst, v);
        truncate_after(cx, t, dst);
      }
      return;
    default:
      return;
    }
  }

  case NT_BINARY: {
    Type *lt = (Type *)node_get(e->a)->sem;
    // && / || short-circuit
    if (e->op == OP_AND || e->op == OP_OR) {
      size_t a = cx_fresh(cx, ty_bool);
      emit_expr(cx, e->a, a);
      if (e->op == OP_AND) {
        op(cx, "(if (local.get %zu) (then\n", a);
        size_t b = cx_fresh(cx, ty_bool);
        emit_expr(cx, e->b, b);
        op(cx, "  (local.set %zu (local.get %zu)))\n", dst, b);
        op(cx, "(else (local.set %zu (i32.const 0))))\n", dst);
      } else {
        op(cx, "(if (local.get %zu) (then (local.set %zu (i32.const 1)))\n",
           a, dst);
        op(cx, "(else\n");
        size_t b = cx_fresh(cx, ty_bool);
        emit_expr(cx, e->b, b);
        op(cx, "  (local.set %zu (local.get %zu))))\n", dst, b);
      }
      return;
    }
    bool isf = type_is_float(lt);
    const char *w = scalar_wty(lt) == W_I64 ? "i64" : "i32";
    const char *fw = lt->kind == TY_F32 ? "f32" : "f64";
    size_t a = cx_fresh(cx, lt);
    size_t b = cx_fresh(cx, lt);
    emit_expr(cx, e->a, a);
    emit_expr(cx, e->b, b);
    const char *instr = NULL;
    bool cmp = false;
    bool uns = lt->kind >= TY_U8;
    switch (e->op) {
    case OP_ADD: instr = isf ? "add" : "add"; break;
    case OP_SUB: instr = "sub"; break;
    case OP_MUL: instr = "mul"; break;
    case OP_DIV:
      emit_div(cx, OP_DIV, lt, dst, a, b);
      truncate_after(cx, t, dst);
      return;
    case OP_MOD:
      emit_div(cx, OP_MOD, lt, dst, a, b);
      truncate_after(cx, t, dst);
      return;
    case OP_BAND: instr = "and"; break;
    case OP_BOR: instr = "or"; break;
    case OP_BXOR: instr = "xor"; break;
    case OP_SHL: {
      int bits = lt->kind == TY_I8 || lt->kind == TY_U8 ? 8
                 : lt->kind == TY_I16 || lt->kind == TY_U16 ? 16
                 : lt->kind == TY_I32 || lt->kind == TY_U32 ||
                       lt->kind == TY_USIZE ? 32 : 64;
      if (scalar_wty(lt) == W_I64)
        op(cx, "(local.set %zu (i64.shl (local.get %zu) "
               "(i64.and (local.get %zu) (i64.const %d))))\n", dst, a, b,
           bits - 1);
      else {
        op(cx, "(local.set %zu (i32.shl (local.get %zu) "
               "(i32.and (local.get %zu) (i32.const %d))))\n", dst, a, b,
           bits - 1);
        truncate_after(cx, t, dst);
      }
      return;
    }
    case OP_SHR: {
      int bits = lt->kind == TY_I8 || lt->kind == TY_U8 ? 8
                 : lt->kind == TY_I16 || lt->kind == TY_U16 ? 16
                 : lt->kind == TY_I32 || lt->kind == TY_U32 ||
                       lt->kind == TY_USIZE ? 32 : 64;
      const char *k = uns || lt->kind == TY_USIZE
                          ? "shr_u"
                          : (scalar_wty(lt) == W_I64 ? "shr_s" : "shr_u");
      // signed narrow ints live zero-extended in i32: use i32 shifts
      // with manual sign handling via extension
      if (scalar_wty(lt) == W_I64)
        op(cx, "(local.set %zu (i64.%s (local.get %zu) "
               "(i64.and (local.get %zu) (i64.const %d))))\n", dst,
           uns ? "shr_u" : "shr_s", a, b, bits - 1);
      else {
        if (!uns && bits < 32) {
          // sign-extend to the width first, then arithmetic shift
          int sh = 32 - bits;
          op(cx, "(local.set %zu (i32.shr_s (i32.shl (local.get %zu) "
                 "(i32.const %d)) (i32.const %d)))\n", dst, a, sh, sh);
          op(cx, "(local.set %zu (i32.shr_s (local.get %zu) "
                 "(i32.and (local.get %zu) (i32.const %d))))\n", dst, dst,
             b, bits - 1);
        } else {
          op(cx, "(local.set %zu (i32.%s (local.get %zu) "
                 "(i32.and (local.get %zu) (i32.const %d))))\n", dst,
             uns ? "shr_u" : "shr_s", a, b, bits - 1);
        }
        truncate_after(cx, t, dst);
      }
      (void)k;
      return;
    }
    case OP_EQ: instr = "eq"; cmp = true; break;
    case OP_NE: instr = "ne"; cmp = true; break;
    case OP_LT: instr = uns ? "lt_u" : "lt_s"; cmp = true; break;
    case OP_LE: instr = uns ? "le_u" : "le_s"; cmp = true; break;
    case OP_GT: instr = uns ? "gt_u" : "gt_s"; cmp = true; break;
    case OP_GE: instr = uns ? "ge_u" : "ge_s"; cmp = true; break;
    default: return;
    }
    if (isf) {
      if (cmp) {
        op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
           dst, fw, instr, a, b);
      } else {
        op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
           dst, fw, instr, a, b);
      }
    } else if (cmp) {
      op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
         dst, w, instr, a, b);
    } else {
      op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
         dst, w, instr, a, b);
      truncate_after(cx, t, dst);
    }
    return;
  }

  case NT_AS: {
    Type *ft = (Type *)node_get(e->a)->sem;
    size_t v = cx_fresh(cx, ft);
    emit_expr(cx, e->a, v);
    // the unified matrix: wrap/truncate/extend; float→int truncates
    // toward zero saturating; int→float converts
    WTy fw = scalar_wty(ft), tw = scalar_wty(t);
    if (fw == W_F64 && tw == W_F32)
      op(cx, "(local.set %zu (f32.demote_f64 (local.get %zu)))\n", dst, v);
    else if (fw == W_F32 && tw == W_F64)
      op(cx, "(local.set %zu (f64.promote_f32 (local.get %zu)))\n", dst, v);
    else if ((fw == W_F32 || fw == W_F64) && tw == W_I32)
      op(cx, "(local.set %zu (i32.trunc_f32_s (local.get %zu)))\n", dst,
         v);
    else if ((fw == W_F32 || fw == W_F64) && tw == W_I64)
      op(cx, "(local.set %zu (i64.trunc_f64_s (local.get %zu)))\n", dst,
         v);
    else if (fw == W_I32 && (tw == W_F32 || tw == W_F64))
      op(cx, "(local.set %zu (f32.convert_i32_s (local.get %zu)))\n", dst,
         v);
    else if (fw == W_I64 && (tw == W_F32 || tw == W_F64))
      op(cx, "(local.set %zu (f32.convert_i64_s (local.get %zu)))\n", dst,
         v);
    else if (fw == W_I32 && tw == W_I32) {
      op(cx, "(local.set %zu (local.get %zu))\n", dst, v);
      truncate_after(cx, t, dst);
    } else if (fw == W_I64 && tw == W_I32) {
      op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", dst, v);
      truncate_after(cx, t, dst);
    } else if (fw == W_I32 && tw == W_I64) {
      // sign- or zero-extend by the SOURCE's signedness
      if (ft->kind >= TY_U8)
        op(cx, "(local.set %zu (i64.extend_i32_u (local.get %zu)))\n",
           dst, v);
      else
        op(cx, "(local.set %zu (i64.extend_i32_s (local.get %zu)))\n",
           dst, v);
    } else {
      op(cx, "(local.set %zu (local.get %zu))\n", dst, v);
    }
    return;
  }

  case NT_IF_EXPR: {
    size_t c = cx_fresh(cx, ty_bool);
    emit_expr(cx, e->a, c);
    op(cx, "(if (local.get %zu) (then\n", c);
    size_t tv = cx_fresh(cx, t);
    emit_expr(cx, e->b, tv);
    size_t n = shape_nlocals(t);
    for (size_t i = 0; i < n; i++)
      op(cx, "(local.set %zu %s)\n", dst + i, L(cx, tv + i));
    op(cx, ") (else\n");
    size_t fv = cx_fresh(cx, t);
    emit_expr(cx, e->c, fv);
    for (size_t i = 0; i < n; i++)
      op(cx, "(local.set %zu %s)\n", dst + i, L(cx, fv + i));
    op(cx, "))\n");
    return;
  }

  default:
    op(cx, ";; UNEMITTED %s\n", node_kind_name(e->kind));
    return;
  }
}

// statics: memory slots assigned in module symbol order (deterministic);
// layout table filled lazily, data placed after literals
static Vec g_static_slots; // of {const char *mod, const char *name, size_t at}
typedef struct StaticSlot {
  const char *mod;
  const char *name;
  size_t at;
} StaticSlot;

size_t static_slot(Module *m, const char *name) {
  if (!g_static_slots.data)
    vec_init(&g_static_slots, sizeof(StaticSlot));
  // deterministic: module load order × symbol creation order
  for (Module *mod = em_cur->p->modules; mod; mod = mod->next) {
    if (!mod->syms)
      continue;
    for (Sym *s = mod->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_STATIC)
        continue;
      bool present = false;
      for (size_t i = 0; i < VLEN(g_static_slots); i++) {
        StaticSlot *ss = VAT(g_static_slots, StaticSlot, i);
        if (ss->mod == mod->path && strcmp(ss->name, s->name) == 0)
          present = true;
      }
      if (present)
        continue;
      StaticSlot *ss = VPUSH(g_static_slots, StaticSlot);
      ss->mod = mod->path;
      ss->name = s->name;
      ss->at = em_cur->data_off;
      em_cur->data_off += 64; // room for any shape + slack
    }
  }
  for (size_t i = 0; i < VLEN(g_static_slots); i++) {
    StaticSlot *ss = VAT(g_static_slots, StaticSlot, i);
    if (ss->mod == m->path && strcmp(ss->name, name) == 0)
      return ss->at;
  }
  return 0; // checker already reported
}

// ============================================================ printf desugar

// printf/eprintf: literal chunks and per-hole to_str pushes, one write
static void emit_printf(FnCx *cx, Node *call, bool err) {
  Node *fmtn = node_get(node_get(reflist_at(call->list, 0))->a);
  Str fmt = fmtn->sval;
  size_t argi = 1;
  size_t i = 0;
  while (i < fmt.n) {
    if (i + 1 < fmt.n && fmt.p[i] == '{' && fmt.p[i + 1] == '}') {
      Node *aw = node_get(reflist_at(call->list, argi++));
      Type *at = (Type *)node_get(aw->a)->sem;
      size_t v = cx_fresh(cx, at);
      emit_expr(cx, aw->a, v);
      switch (at->kind) {
      case TY_I64: case TY_U64:
        op(cx, "(call $print_i64 %s)\n", L(cx, v));
        break;
      case TY_F32: case TY_F64:
        // float printing lands with the rho-source prelude (T1.8);
        // until then floats through {} are a compile error (checker
        // allows; emitter reports) — keep honest:
        op(cx, "(call $print_f64_placeholder %s)\n", L(cx, v));
        break;
      case TY_BOOL: {
        size_t t1 = data_intern("true", 4);
        size_t t0 = data_intern("false", 5);
        op(cx, "(if (local.get %zu) (then (call $print_mem (i32.const %zu)"
               " (i32.const 4))) (else (call $print_mem (i32.const %zu)"
               " (i32.const 5))))\n", v, t1, t0);
        break;
      }
      case TY_STRING:
        op(cx, "(call $print_mem %s %s)\n", L(cx, v), L(cx, v + 1));
        break;
      default: // 32-bit ints widen to i64 prints
        if (scalar_wty(at) == W_I64) {
          op(cx, "(call $print_i64 %s)\n", L(cx, v));
        } else {
          size_t wide = cx_fresh(cx, ty_i64);
          bool uns = at->kind >= TY_U8;
          op(cx, "(local.set %zu (%s.%s (local.get %zu)))\n", wide,
             "i64", uns ? "extend_i32_u" : "extend_i32_s", v);
          op(cx, "(call $print_i64 %s)\n", L(cx, wide));
        }
        break;
      }
      i += 2;
    } else {
      // literal run up to the next hole
      size_t j = i;
      while (j < fmt.n &&
             !(j + 1 < fmt.n && fmt.p[j] == '{' && fmt.p[j + 1] == '}'))
        j++;
      size_t at = data_intern(fmt.p + i, j - i);
      op(cx, "(call $print_mem (i32.const %zu) (i32.const %zu))\n", at,
         j - i);
      i = j;
    }
  }
  (void)err;
}

// ============================================================ calls

// scalar-only arg passing for the T1.7a slice: managed/struct args
// arrive with T1.7b
static void emit_call_args(FnCx *cx, FnDef *f, RefList *args) {
  size_t nfixed = f->sig->nparams;
  size_t argregs[16];
  size_t nregs = 0;
  for (size_t i = 0; i < nfixed; i++) {
    Type *pt = f->sig->params[i].ty;
    Node *aw = i < reflist_len(args) ? node_get(reflist_at(args, i)) : NULL;
    size_t v = cx_fresh(cx, pt);
    argregs[nregs++] = v;
    if (aw && aw->kind == NT_POSARG)
      emit_expr(cx, aw->a, v);
    // leave zeroed on arity mismatch (checker already reported)
  }
  // push all argument locals in order, then the caller emits (call …)
  for (size_t i = 0; i < nregs; i++)
    op(cx, "%s", L(cx, argregs[i]));
}

// ============================================================ statements

static void emit_stmt(FnCx *cx, NodeRef sr) {
  Node *s = node_get(sr);
  switch (s->kind) {
  case NT_LET: {
    Type *t = (Type *)node_get(s->b)->sem;
    size_t v = cx_fresh(cx, t);
    emit_expr(cx, s->b, v);
    VarInfo *vi = VPUSH(cx->vars, VarInfo);
    vi->node = sr;
    vi->vreg = v;
    vi->managed = type_is_managed(t);
    vi->scope = cx->scope;
    return;
  }
  case NT_RETURN: {
    if (cx->ret->kind == TY_UNIT || s->a == NO_REF) {
      emit_scope_exit(cx, 0);
      op(cx, "(return)\n");
      return;
    }
    size_t v = cx_fresh(cx, cx->ret);
    emit_expr(cx, s->a, v);
    emit_scope_exit(cx, 0);
    size_t n = shape_nlocals(cx->ret);
    for (size_t i = 0; i < n; i++)
      op(cx, "%s", L(cx, v + i)); // push results
    op(cx, "(return)\n");
    return;
  }
  case NT_EXPRSTMT:
    if (s->op == 3) { // block
      cx->scope++;
      size_t base = VLEN(cx->vars);
      for (size_t i = 0; i < reflist_len(s->list); i++)
        emit_stmt(cx, reflist_at(s->list, i));
      // scope exit: release this scope's managed vars (reverse)
      for (size_t i = VLEN(cx->vars); i > base; i--) {
        VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
        release(cx, v->ty, v->vreg);
      }
      cx->vars.len = base;
      cx->scope--;
      return;
    }
    if (s->a != NO_REF) {
      Type *t = (Type *)node_get(s->a)->sem;
      size_t v = cx_fresh(cx, t);
      emit_expr(cx, s->a, v);
      release(cx, t, v); // discard: release owned temps
    }
    return;
  default:
    op(cx, ";; STMT-UNEMITTED %s\n", node_kind_name(s->kind));
  }
}

static void emit_scope_exit(FnCx *cx, size_t to_scope) {
  // defers + var releases, inner scopes first, reverse order
  for (size_t i = VLEN(cx->defers); i > 0;) {
    DeferEnt *d = VAT(cx->defers, DeferEnt, --i);
    if (d->scope < to_scope)
      break;
    emit_stmt(cx, d->stmt); // the NT_DEFER's payload
  }
  for (size_t i = VLEN(cx->vars); i > 0;) {
    VarInfo *v = VAT(cx->vars, VarInfo, --i);
    if (v->scope < to_scope)
      break;
    release(cx, v->ty, v->vreg);
  }
}

// ============================================================ fn assembly

static void emit_fndef(Em *em, FnDef *f) {
  // idempotence
  for (size_t i = 0; i < VLEN(em->emitted); i++)
    if (*VAT(em->emitted, FnDef *, i) == f)
      return;
  *VPUSH(em->emitted, FnDef *) = f;

  FnCx cx;
  cx_init(&cx, em, f, f->name);
  // params: each param's shape locals, in order; register as vars
  for (size_t i = 0; i < f->sig->nparams; i++) {
    Type *pt = f->sig->params[i].ty ? f->sig->params[i].ty : ty_unit;
    size_t v = cx_fresh(&cx, pt);
    VarInfo *vi = VPUSH(cx.vars, VarInfo);
    vi->node = f->sig->params[i].decl;
    vi->vreg = v;
    vi->managed = type_is_managed(pt);
    vi->scope = 0;
    vi->ty = pt;
  }
  emit_stmt(&cx, f->body);

  // assemble: (func $name (param…) (result…) (local…) body)
  Buf fn;
  buf_init(&fn);
  tprintf(&fn, "  (func $%s", f->name);
  for (size_t i = 0; i < f->sig->nparams; i++) {
    Type *pt = f->sig->params[i].ty ? f->sig->params[i].ty : ty_unit;
    size_t n = shape_nlocals(pt);
    for (size_t j = 0; j < n; j++)
      tprintf(&fn, " (param %s)", wty_s(local_wty(pt, j)));
  }
  if (cx.ret->kind != TY_UNIT) {
    size_t n = shape_nlocals(cx.ret);
    for (size_t j = 0; j < n; j++)
      tprintf(&fn, " (result %s)", wty_s(local_wty(cx.ret, j)));
  }
  for (size_t i = 0; i < VLEN(cx.localtypes); i++)
    tprintf(&fn, " (local %s)", wty_s(*VAT(cx.localtypes, WTy, i)));
  tprintf(&fn, "\n");
  tneed(&fn, cx.b.n);
  memcpy(fn.p + fn.n, cx.b.p, cx.b.n);
  fn.n += cx.b.n;
  tprintf(&fn, "  )\n");

  EFn *ef = VPUSH(em->fns, EFn);
  ef->wat = fn.p;
  ef->wat_len = fn.n;
}

// ============================================================ module

static void emit_start(Em *em) {
  Sym *main = symtab_get(em->p->entry->syms, "main");
  Buf b;
  buf_init(&b);
  tprintf(&b, "  (func $_start\n");
  if (main && main->kind == SYM_FN) {
    FnDef *mf = main->u.fns;
    if (mf->sig->ret->kind != TY_UNIT)
      tprintf(&b, "    (local $rc i32)\n");
    tprintf(&b, "    (local.set $rc (call $%s))\n", mf->name);
    tprintf(&b, "    (call $proc_exit (local.get $rc))\n");
  } else {
    tprintf(&b, "    (call $proc_exit (i32.const 0))\n");
  }
  tprintf(&b, "  )\n");
  tprintf(&b, "  (export \"_start\" (func $_start))\n");
  EFn *ef = VPUSH(em->fns, EFn);
  ef->wat = b.p;
  ef->wat_len = b.n;
}

static void emit_all_fns(Em *em) {
  // deterministic: module load order, then symbol creation order
  for (Module *m = em->p->modules; m; m = m->next) {
    if (!m->syms)
      continue;
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN)
        continue;
      for (FnDef *f = s->u.fns; f; f = f->next_overload)
        if (f->body != NO_REF)
          emit_fndef(em, f);
    }
  }
}

int emit_program(Program *p, bool debug, char **wat_out, size_t *wat_len) {
  static Em em;
  memset(&em, 0, sizeof em);
  em_init(&em, p, debug);
  em_cur = &em;

  emit_all_fns(&em);
  emit_start(&em);

  // assemble: kernel interior + functions + data segments
  Buf *o = &em.o;
  tputs(o, "(module\n");
  // kernel: strip the (module …) wrapper, take the interior
  const char *kw = kernel_wat_src();
  size_t kn = strlen(kw);
  size_t start = 0;
  // find the first line after "(module"
  const char *ml = strstr(kw, "(module");
  start = (size_t)(ml - kw) + 7;
  // find the LAST ')' — the module close
  size_t end = kn;
  while (end > start && kw[end - 1] != ')')
    end--;
  end--; // drop the ')'
  tneed(o, end - start);
  memcpy(o->p + o->n, kw + start, end - start);
  o->n += end - start;
  tprintf(o, "\n");
  // functions
  for (size_t i = 0; i < VLEN(em.fns); i++) {
    EFn *f = VAT(em.fns, EFn, i);
    tneed(o, f->wat_len);
    memcpy(o->p + o->n, f->wat, f->wat_len);
    o->n += f->wat_len;
  }
  // data segments
  for (size_t i = 0; i < VLEN(em.data); i++) {
    DataEnt *d = VAT(em.data, DataEnt, i);
    if (d->n == 0)
      continue;
    tprintf(o, "  (data (i32.const %zu) \"", d->at);
    wat_escape(o, d->bytes, d->n);
    tprintf(o, "\")\n");
  }
  tputs(o, ")\n");
  *wat_out = em.o.p;
  *wat_len = em.o.n;
  return 0;
}
