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
  case TY_I64: case TY_U64: case TY_USIZE: return W_I64;
  case TY_F32: return W_F32;
  case TY_F64: return W_F64;
  default: return W_I32;
  }
}

__attribute__((unused)) static bool ty_narrow32(Type *t) {
  return t->kind == TY_I8 || t->kind == TY_U8 || t->kind == TY_I16 ||
         t->kind == TY_U16 || t->kind == TY_I32 || t->kind == TY_U32;
}

// substitute raw field/payload types through an instance's args
typedef struct EBind {
  const char **names;
  Type **tys;
  size_t n;
} EBind;

static Type *inst_ty(Type *inst, Type *raw) {
  if (!inst || inst->nargs == 0 || !raw)
    return raw;
  EnumDef *ed = inst->kind == TY_ENUM ? inst->edef : NULL;
  StructDef *sd = inst->kind == TY_STRUCT ? inst->sdef : NULL;
  NodeRef gw = ed   ? node_get(ed->decl)->a
               : sd ? node_get(sd->decl)->a
                    : NO_REF;
  if (gw == NO_REF)
    return raw;
  RefList *g = node_get(gw)->list;
  size_t n = inst->nargs;
  const char **names = arena_alloc(g_arena, (n ? n : 1) * sizeof(char *), 8);
  for (size_t i = 0; i < n && g; i++)
    names[i] = node_get(reflist_at(g, i))->name;
  EBind b = {names, inst->args, n};
  return tsubst(raw, &b);
}

static size_t shape_nlocals(Type *t) {
  switch (t->kind) {
  case TY_PTR: return 1;
  case TY_STRING: case TY_SLICE: case TY_DYN: case TY_FN: return 2;
  case TY_STRUCT: {
    size_t n = 0;
    for (size_t i = 0; i < t->sdef->nfields; i++)
      n += shape_nlocals(inst_ty(t, t->sdef->fields[i].ty));
    return n ? n : 1;
  }
  case TY_ENUM: {
    size_t max = 0;
    for (size_t i = 0; i < t->edef->nvariants; i++) {
      size_t n = 0;
      for (size_t k = 0; k < t->edef->variants[i].nfields; k++)
        n += shape_nlocals(inst_ty(t, t->edef->variants[i].fields[k].ty));
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
      WTy w = local_wty_rec(inst_ty(t, t->sdef->fields[i].ty), j, base);
      if (*base > j) return w;
    }
    return W_I32;
  case TY_ENUM: {
    if (j == *base) { (*base)++; return W_I32; }
    (*base)++;
    for (size_t i = 0; i < t->edef->nvariants; i++)
      for (size_t k = 0; k < t->edef->variants[i].nfields; k++) {
        WTy w = local_wty_rec(inst_ty(t, t->edef->variants[i].fields[k].ty),
                              j, base);
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
  size_t label_n;      // unique loop-label ids
  size_t closure_n;    // synthesized closure counter
  Vec closures;        // of const char* (wat names) — table order
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
  vec_init(&em->closures, sizeof(const char *));
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
  const char *name; // binding name (let/param/binder)
  NodeRef node;     // originating decl node (0 for binders)
  size_t vreg;
  bool managed;   // needs release at scope exit
  bool moved;     // ownership transferred (returned); no release
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
    if (strcmp(v->name, name) == 0)
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
static void pass_arg_own(FnCx *cx, NodeRef arg, Type *pt, size_t v);
static void emit_match(FnCx *cx, NodeRef er, size_t dst);
static void bind_pattern(FnCx *cx, Node *pat, Type *st, size_t *slot);
static void register_pattern_binders(FnCx *cx, Node *pat, Type *st);
#define MATCH_DISCARD ((size_t)-1)
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
static void release(FnCx *cx, Type *t, size_t vreg); // fwd

static void release_var(FnCx *cx, VarInfo *v) {
  if (v->moved)
    return;
  release(cx, v->ty, v->vreg);
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
  case TY_I8:
    // sign-extend the low 8 bits so prints/arithmetic see -128..127
    op(cx, "(local.set %zu (i32.shr_s (i32.shl (local.get %zu) "
           "(i32.const 24)) (i32.const 24)))\n", v, v);
    break;
  case TY_U8:
    op(cx, "(local.set %zu (i32.and (local.get %zu) (i32.const 255)))\n",
       v, v);
    break;
  case TY_I16:
    op(cx, "(local.set %zu (i32.shr_s (i32.shl (local.get %zu) "
           "(i32.const 16)) (i32.const 16)))\n", v, v);
    break;
  case TY_U16:
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
  op(cx, "(if (%s.eqz (local.get %zu)) (then\n", w, b);
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
  case TY_I64: case TY_U64: case TY_USIZE: {
    uint64_t v = 0;
    if (e->kind == NT_INT) v = e->ival;
    else if (e->kind == NT_UNARY) {
      Node *op0 = node_get(e->a);
      uint64_t base = op0->kind == NT_INT   ? op0->ival
                      : op0->kind == NT_FLOAT ? (uint64_t)(int64_t)op0->fval
                                              : 0;
      v = (uint64_t)(0 - (int64_t)base);
    } else if (e->kind == NT_FLOAT) v = (uint64_t)(int64_t)e->fval;
    op(cx, "(local.set %zu (i64.const %llu))\n", dst,
       (unsigned long long)v);
    break;
  }
  case TY_F32: {
    double dv = e->kind == NT_FLOAT  ? e->fval
                : e->kind == NT_UNARY ? -(double)(int64_t)node_get(e->a)->ival
                                      : (double)(int64_t)e->ival;
    op(cx, "(local.set %zu (f32.const %.9g))\n", dst, dv);
    break;
  }
  case TY_F64: {
    double dv = e->kind == NT_FLOAT  ? e->fval
                : e->kind == NT_UNARY ? -(double)(int64_t)node_get(e->a)->ival
                                      : (double)(int64_t)e->ival;
    op(cx, "(local.set %zu (f64.const %.17g))\n", dst, dv);
    break;
  }
  default: { // 32-bit ints (wrap into range)
    uint64_t v = 0;
    if (e->kind == NT_INT) v = e->ival;
    else if (e->kind == NT_UNARY) {
      Node *op0 = node_get(e->a);
      uint64_t base = op0->kind == NT_INT   ? op0->ival
                      : op0->kind == NT_FLOAT ? (uint64_t)(int64_t)op0->fval
                                              : 0;
      v = (uint64_t)(0 - (int64_t)base);
    } else if (e->kind == NT_FLOAT) v = (uint64_t)(int64_t)e->fval;
    truncate_after(cx, t, dst);
    op(cx, "(local.set %zu (i32.const %d))\n", dst, (int32_t)(uint32_t)v);
    truncate_after(cx, t, dst);
  }
  }
}

// register a struct type's drop fn (deterministic first-use order)
// drop-fn registry: one walker per struct type that owns managed
// fields; table index assigned at first use (deterministic)
static size_t dropfn_for(Type *t) {
  Em *em = em_cur;
  for (size_t i = 0; i < VLEN(em->dropfns); i++)
    if (*VAT(em->dropfns, Type *, i) == t)
      return 4 + i;
  *VPUSH(em->dropfns, Type *) = t;
  return 4 + VLEN(em->dropfns) - 1;
}

void em_queue_text(Em *em, const char *text) {
  size_t n = strlen(text);
  tneed(&em->fnbuf, n);
  memcpy(em->fnbuf.p + em->fnbuf.n, text, n);
  em->fnbuf.n += n;
}

// wasm type section entries for closure signatures (params + capt ptr)
typedef struct CloTy {
  FnSig *sig;
  const char *tname;
} CloTy;
static Vec g_clotypes; // of CloTy

size_t clofn_type_idx(FnCx *cx, FnSig *sig) {
  (void)cx;
  if (!g_clotypes.data)
    vec_init(&g_clotypes, sizeof(CloTy));
  for (size_t i = 0; i < VLEN(g_clotypes); i++)
    if (VAT(g_clotypes, CloTy, i)->sig == sig)
      return i;
  CloTy *ct = VPUSH(g_clotypes, CloTy);
  ct->sig = sig;
  ct->tname = aprintf(g_arena, "$cloty_%zu", VLEN(g_clotypes) - 1);
  return VLEN(g_clotypes) - 1;
}

// trampoline for passing a plain fn where a closure value is expected
typedef struct FnWrap {
  FnDef *fn;
  const char *tname;
} FnWrap;
static Vec g_fnwraps;

size_t fnvalue_wrap(FnCx *cx, FnDef *pf) {
  if (!g_fnwraps.data)
    vec_init(&g_fnwraps, sizeof(FnWrap));
  for (size_t i = 0; i < VLEN(g_fnwraps); i++)
    if (VAT(g_fnwraps, FnWrap, i)->fn == pf)
      return 4 + VLEN(cx->em->dropfns) + VLEN(cx->em->closures) + i;
  FnWrap *w = VPUSH(g_fnwraps, FnWrap);
  w->fn = pf;
  w->tname = aprintf(g_arena, "$tramp_%zu", VLEN(g_fnwraps) - 1);
  // the trampoline body: params..., capt -> call f(params...)
  Buf b;
  buf_init(&b);
  tprintf(&b, "  (func %s", w->tname);
  for (size_t i = 0; i < pf->sig->nparams; i++) {
    size_t n = shape_nlocals(pf->sig->params[i].ty);
    for (size_t k = 0; k < n; k++)
      tprintf(&b, " (param %s)", wty_s(local_wty(pf->sig->params[i].ty, k)));
  }
  tprintf(&b, " (param i32)");
  if (pf->sig->ret->kind != TY_UNIT) {
    size_t n = shape_nlocals(pf->sig->ret);
    for (size_t k = 0; k < n; k++)
      tprintf(&b, " (result %s)", wty_s(local_wty(pf->sig->ret, k)));
  }
  tprintf(&b, "\n");
  size_t base = 0;
  for (size_t i = 0; i < pf->sig->nparams; i++) {
    size_t n = shape_nlocals(pf->sig->params[i].ty);
    for (size_t k = 0; k < n; k++)
      tprintf(&b, " (local.get %zu)\n", base + k);
    base += n;
  }
  tprintf(&b, " (call $%s)\n", pf->name);
  tprintf(&b, "  )\n");
  em_queue_text(cx->em, aprintf(g_arena, "%.*s", (int)b.n, b.p));
  return 4 + VLEN(cx->em->dropfns) + VLEN(cx->em->closures) +
         VLEN(g_fnwraps) - 1;
}

size_t closure_table_idx(Em *em, const char *cname) {
  for (size_t i = 0; i < VLEN(em->closures); i++)
    if (strcmp(*VAT(em->closures, const char *, i), cname) == 0)
      return 4 + VLEN(em->dropfns) + i;
  *VPUSH(em->closures, const char *) = cname;
  return 4 + VLEN(em->dropfns) + VLEN(em->closures) - 1;
}

// walk a closure body collecting outer-variable references (in
// deterministic traversal order; duplicates kept per reference site —
// dedup happens at the copy step)
typedef struct CapSet {
  Vec list; // of VarInfo* — unique by vreg
} CapSet;

void closure_collect_captures(FnCx *cx, NodeRef br, Vec *caps) {
  Node *b = node_get(br);
  if (b->kind != NT_EXPRSTMT || b->op != 3)
    return;
  extern void cap_stmt(FnCx * cx, Node * s, Vec * caps);
  for (size_t i = 0; i < reflist_len(b->list); i++)
    cap_stmt(cx, node_get(reflist_at(b->list, i)), caps);
}

static void cap_add(FnCx *cx, Node *pathnode, Vec *caps) {
  VarInfo *v = cx_var(cx, pathnode->name);
  if (!v || !v->ty)
    return; // own params/undeclared have nothing to capture
  for (size_t i = 0; i < VLEN(*caps); i++)
    if (*VAT(*caps, VarInfo *, i) == v)
      return;
  *VPUSH(*caps, VarInfo *) = v;
}

static void cap_expr(FnCx *cx, NodeRef er, Vec *caps) {
  if (er == NO_REF)
    return;
  Node *e = node_get(er);
  if (e->kind == NT_PATH) {
    cap_add(cx, e, caps);
    return;
  }
  if (e->kind == NT_CALL) {
    cap_expr(cx, e->a, caps);
    for (size_t i = 0; i < reflist_len(e->list); i++)
      cap_expr(cx, node_get(reflist_at(e->list, i))->a, caps);
    return;
  }
  if (e->kind == NT_METHOD) {
    cap_expr(cx, e->a, caps);
    for (size_t i = 0; i < reflist_len(e->list); i++)
      cap_expr(cx, node_get(reflist_at(e->list, i))->a, caps);
    return;
  }
  cap_expr(cx, e->a, caps);
  cap_expr(cx, e->b, caps);
  cap_expr(cx, e->c, caps);
  cap_expr(cx, e->d, caps);
  if (e->list)
    for (size_t i = 0; i < reflist_len(e->list); i++)
      cap_expr(cx, reflist_at(e->list, i), caps);
}

void cap_stmt(FnCx *cx, Node *s, Vec *caps) {
  cap_expr(cx, s->a, caps);
  cap_expr(cx, s->b, caps);
  cap_expr(cx, s->c, caps);
  cap_expr(cx, s->d, caps);
  if (s->list && s->kind != NT_NEW && s->kind != NT_METHOD &&
      s->kind != NT_CALL)
    for (size_t i = 0; i < reflist_len(s->list); i++)
      cap_stmt(cx, node_get(reflist_at(s->list, i)), caps);
  // nested closures capture transitively (their bodies run in their own
  // context; outer refs still capture here)
  if (s->kind == NT_CLOSURE)
    cap_expr(cx, s->c, caps);
}

// emit every registered drop walker + the funcref table
static void emit_dropfns_and_table(Em *em) {
  for (size_t i = 0; i < VLEN(em->dropfns); i++) {
    Type *st = *VAT(em->dropfns, Type *, i);
    // name deterministic from the struct + its module path order
    const char *nm = aprintf(g_arena, "$drop_%s_%zu", st->sdef->name, i);
    tprintf(&em->fnbuf, "  (func %s (param $p i32)\n", nm);
    for (size_t f = 0; f < st->sdef->nfields; f++) {
      Type *ft = inst_ty(st, st->sdef->fields[f].ty);
      size_t off = field_offset(st, f);
      if (!type_is_managed(ft))
        continue;
      if (ft->kind == TY_PTR || ft->kind == TY_STRING ||
          ft->kind == TY_SLICE || ft->kind == TY_DYN)
        tprintf(&em->fnbuf,
                "    (call $rho_release (i32.load (i32.add (local.get $p) "
                "(i32.const %zu))))\n", off);
    }
    tprintf(&em->fnbuf, "  )\n");
  }
  // table: 4 nodrop slots + one per registered walker
  tprintf(&em->fnbuf, "  (table %zu funcref)\n",
          4 + VLEN(em->dropfns) + VLEN(em->closures) + VLEN(g_fnwraps));
  tprintf(&em->fnbuf,
          "  (elem (i32.const 0) $rho_nodrop $rho_nodrop $rho_nodrop "
          "$rho_nodrop");
  for (size_t i = 0; i < VLEN(em->dropfns); i++) {
    Type *st = *VAT(em->dropfns, Type *, i);
    tprintf(&em->fnbuf, " $drop_%s_%zu", st->sdef->name, i);
  }
  for (size_t i = 0; i < VLEN(em->closures); i++)
    tprintf(&em->fnbuf, " %s", *VAT(em->closures, const char *, i));
  for (size_t i = 0; i < VLEN(g_fnwraps); i++)
    tprintf(&em->fnbuf, " %s", VAT(g_fnwraps, FnWrap, i)->tname);
  tprintf(&em->fnbuf, ")\n");
  (void)dropfn_for;
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
    Sym *s = cx->p->entry->syms ? symtab_get(cx->p->entry->syms, e->name)
                                : NULL;
    Module *m = cx->fn ? cx->fn->mod : cx->p->entry;
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
    // a plain fn referenced as a value (spec: non-variadic fns are
    // first-class): wrap it — trampoline takes the trailing capt param
    if (s && s->kind == SYM_FN && !s->u.fns->next_overload) {
      FnDef *pf = s->u.fns;
      Type *ft = (Type *)e->sem;
      if (ft && ft->kind == TY_FN) {
        extern size_t fnvalue_wrap(FnCx * cx, FnDef * pf);
        size_t ti = fnvalue_wrap(cx, pf);
        op(cx, "(local.set %zu (i32.const %zu))\n", dst, ti);
        op(cx, "(local.set %zu (i32.const 0))\n", dst + 1); // no captures
        return;
      }
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
    if (is_pre && strcmp(callee->name, "make") == 0) {
      // make([]T, n): zeroed block; slice = (payload, n)
      Node *aw1 = reflist_len(e->list) > 1
                      ? node_get(reflist_at(e->list, 1))
                      : NULL;
      Type *st = (Type *)e->sem; // the slice type
      size_t esz = st && st->kind == TY_SLICE ? type_size(st->base) : 4;
      size_t n = cx_fresh(cx, ty_usize);
      if (aw1 && aw1->kind == NT_POSARG)
        emit_expr(cx, aw1->a, n);
      else
        op(cx, "(local.set %zu (i64.const 0))\n", n);
      // byte size = n * esz (i64 math, wrap to i32 for alloc)
      size_t bytes = cx_fresh(cx, ty_i64);
      op(cx, "(local.set %zu (i64.mul (local.get %zu) (i64.const %zu)))\n",
         bytes, n, esz);
      size_t b32 = cx_fresh(cx, ty_i32);
      op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", b32,
         bytes);
      size_t blk = cx_fresh(cx, ty_i32);
      op(cx, "(local.set %zu (call $rho_alloc (local.get %zu)))\n", blk,
         b32);
      op(cx, "(local.set %zu (i32.add (local.get %zu) (i32.const 24)))\n",
         dst, blk);
      {
        size_t n32 = cx_fresh(cx, ty_i32);
        op(cx, "(local.set %zu (i32.wrap_i64 %s))\n", n32, L(cx, n));
        op(cx, "(local.set %zu %s)\n", dst + 1, L(cx, n32));
      }
      return;
    }
    if (is_pre && strcmp(callee->name, "len") == 0) {
      Node *aw = reflist_len(e->list) ? node_get(reflist_at(e->list, 0))
                                      : NULL;
      if (aw && aw->kind == NT_POSARG) {
        Type *at = (Type *)node_get(aw->a)->sem;
        size_t v = cx_fresh(cx, at);
        emit_expr(cx, aw->a, v);
        if (at->kind == TY_STRING || at->kind == TY_SLICE)
          op(cx, "(local.set %zu (i64.extend_i32_u %s))\n", dst,
             L(cx, v + 1));
        else {
          op(cx, "(local.set %zu (i32.const 0))\n", dst);
        }
      }
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
    // fn value held in a local: call through the table
    VarInfo *fvv = cx_var(cx, callee->name);
    if (fvv && fvv->ty && fvv->ty->kind == TY_FN) {
      FnSig *sig = fvv->ty->sig;
      size_t argregs2[16];
      for (size_t i = 0; i < sig->nparams; i++) {
        size_t v = cx_fresh(cx, sig->params[i].ty);
        argregs2[i] = v;
        Node *aw = i < reflist_len(e->list)
                       ? node_get(reflist_at(e->list, i))
                       : NULL;
        if (aw && aw->kind == NT_POSARG)
          emit_expr(cx, aw->a, v);
      }
      for (size_t i = 0; i < sig->nparams; i++) {
        size_t n = shape_nlocals(sig->params[i].ty);
        for (size_t k = 0; k < n; k++)
          op(cx, "%s", L(cx, argregs2[i] + k));
      }
      op(cx, "%s\n", L(cx, fvv->vreg + 1)); // capture ptr
      extern size_t clofn_type_idx(FnCx * cx, FnSig * sig);
      op(cx, "(call_indirect (type $cloty_%zu) %s)\n",
         clofn_type_idx(cx, sig), L(cx, fvv->vreg));
      if (sig->ret->kind != TY_UNIT) {
        size_t n = shape_nlocals(sig->ret);
        for (size_t i = n; i > 0; i--)
          op(cx, "(local.set %zu)\n", dst + i - 1);
      }
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
    case OP_DEREF: {
      // *p: copy the pointee value out of the block (payload ptr)
      size_t n = shape_nlocals(t);
      for (size_t i = 0; i < n; i++) {
        WTy w = local_wty(t, i);
        const char *ld = w == W_I64   ? "i64.load"
                         : w == W_F32 ? "f32.load"
                         : w == W_F64 ? "f64.load"
                                      : "i32.load";
        op(cx, "(local.set %zu (%s (i32.add %s (i32.const %zu))))\n",
           dst + i, ld, L(cx, v), i * 4);
      }
      return;
    }
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
    if (e->op == OP_ADD && lt->kind == TY_STRING) {
      op(cx, "(call $rho_cat2 (local.get %zu) (local.get %zu) "
             "(local.get %zu) (local.get %zu))\n", a, a + 1, b, b + 1);
      op(cx, "(local.set %zu (global.get $cat_ptr))\n", dst);
      op(cx, "(local.set %zu (global.get $cat_len))\n", dst + 1);
      return;
    }
    if ((e->op == OP_EQ || e->op == OP_NE) && lt->kind == TY_STRING) {
      op(cx, "(local.set %zu (call $rho_streq (local.get %zu) "
             "(local.get %zu) (local.get %zu) (local.get %zu)))\n", dst,
         a, a + 1, b, b + 1);
      if (e->op == OP_NE)
        op(cx, "(local.set %zu (i32.eqz (local.get %zu)))\n", dst, dst);
      return;
    }
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
      int bits = scalar_wty(lt) == W_I64 ? 64 : 32; // storage width
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
      int bits = scalar_wty(lt) == W_I64 ? 64 : 32;
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

  case NT_METHOD: {
    // op==2 marks a real method call (sem2 = FnDef); otherwise a
    // non-NULL sem2 is a variant constructor
    EnumVariant *var = e->op == 2 ? NULL : (EnumVariant *)e->sem2;
    if (var) {
      // Enum.Variant(…) construction: tag + payload slots
      op(cx, "(local.set %zu (i32.const %d))\n", dst, var->tag);
      size_t slot = dst + 1;
      for (size_t i = 0; i < reflist_len(e->list); i++) {
        Node *aw = node_get(reflist_at(e->list, i));
        Type *ft = NULL;
        // positional or named — both map by field order
        const char *want = NULL;
        if (aw->kind == NT_FIELDINIT)
          want = aw->name;
        for (size_t k = 0; k < var->nfields; k++) {
          if (want && strcmp(var->fields[k].name, want) != 0)
            continue;
          extern Type *variant_field_ty(EnumVariant *, Type *, size_t);
          ft = variant_field_ty(var, t, k);
          if (aw->kind == NT_FIELDINIT) {
            size_t tmp = cx_fresh(cx, ft);
            emit_expr(cx, aw->a, tmp);
            size_t n = shape_nlocals(ft);
            for (size_t j = 0; j < n; j++)
              op(cx, "(local.set %zu %s)\n", slot + j, L(cx, tmp + j));
          } else {
            size_t n = shape_nlocals(ft);
            emit_expr(cx, aw->a, slot);
            (void)n;
          }
          slot += shape_nlocals(ft);
          if (want)
            break;
        }
      }
      return;
    }
    if (e->op == 2) {
      // real method: push self, then args, direct call
      FnDef *f = (FnDef *)e->sem2;
      if (!f)
        return;
      Type *rt = (Type *)node_get(e->a)->sem;
      size_t selfv = cx_fresh(cx, rt);
      emit_expr(cx, e->a, selfv);
      // args map to params[1..]
      size_t nfixed = f->sig->nparams;
      size_t argregs[16];
      size_t nregs = 0;
      for (size_t i = 1; i < nfixed; i++) {
        Type *pt = f->sig->params[i].ty;
        Node *aw = (i - 1) < reflist_len(e->list)
                       ? node_get(reflist_at(e->list, i - 1))
                       : NULL;
        size_t v = cx_fresh(cx, pt);
        argregs[nregs++] = v;
        if (aw && aw->kind == NT_POSARG) {
          emit_expr(cx, aw->a, v);
          pass_arg_own(cx, aw->a, pt, v);
        }
      }
      // methods on the concrete type are direct calls
      if (node_get(e->a)->kind == NT_PATH && cx_var(cx, node_get(e->a)->name))
        retain(cx, rt, selfv);
      op(cx, "%s", L(cx, selfv));
      for (size_t i = 0; i < nregs; i++)
        op(cx, "%s", L(cx, argregs[i]));
      op(cx, "(call $%s)\n", f->name);
      Type *tres = f->sig->ret;
      if (tres->kind != TY_UNIT) {
        size_t n = shape_nlocals(tres);
        for (size_t i = n; i > 0; i--)
          op(cx, "(local.set %zu)\n", dst + i - 1);
      }
      (void)rt;
      return;
    }
    op(cx, ";; variant ctor fallback\n");
    return;
  }

  case NT_FIELD_E: {
    EnumVariant *var = (EnumVariant *)e->sem2;
    if (var) { // unit variant value
      op(cx, "(local.set %zu (i32.const %d))\n", dst, var->tag);
      // payload slots zeroed by local default
      return;
    }
    Type *bt = (Type *)node_get(e->a)->sem;
    bool viaptr = bt->kind == TY_PTR;
    Type *st = viaptr ? bt->base : bt;
    int idx = e->op; // the checker's field index
    size_t off = field_offset(st, (size_t)idx);
    Type *ft = inst_ty(st, st->sdef->fields[idx].ty);
    if (viaptr) {
      // load the field run from the block payload (ptr + 24 + off)
      size_t p = cx_fresh(cx, bt);
      emit_expr(cx, e->a, p);
      size_t n = shape_nlocals(ft);
      for (size_t i = 0; i < n; i++) {
        size_t fo = off + i * 4;
        WTy w = local_wty(ft, i);
        const char *ld = w == W_I64   ? "i64.load"
                         : w == W_F32 ? "f32.load"
                         : w == W_F64 ? "f64.load"
                                      : "i32.load";
        op(cx, "(local.set %zu (%s (i32.add (local.get %zu) "
               "(i32.const %zu))))\n", dst + i, ld, p, fo);
      }
      return;
    }
    // value struct: the field's locals sit at the base run's offset
    size_t base = cx_fresh(cx, bt);
    emit_expr(cx, e->a, base);
    size_t skip = 0;
    for (size_t i = 0; i < (size_t)idx; i++)
      skip += shape_nlocals(inst_ty(st, st->sdef->fields[i].ty));
    size_t n = shape_nlocals(ft);
    for (size_t i = 0; i < n; i++)
      op(cx, "(local.set %zu %s)\n", dst + i, L(cx, base + skip + i));
    return;
  }

  case NT_NEW: {
    // new T { fields } → heap block (header 24B, payload at +24)
    Type *st = t->base; // result type is *T
    size_t sz = type_size(st);
    op(cx, "(local.set %zu (call $rho_alloc (i32.const %zu)))\n", dst,
       sz);
    op(cx, "(local.set %zu (i32.add (local.get %zu) (i32.const 24)))\n",
       dst, dst);
    { // the block's destructor (rc==0 walks owned fields, spec §1.4)
      size_t di = dropfn_for(st);
      op(cx, "(i32.store (i32.sub (local.get %zu) (i32.const 12)) "
             "(i32.const %zu))\n", dst, di);
    }
    RefList *inits = e->b != NO_REF ? node_get(e->b)->list : NULL;
    for (size_t i = 0; i < st->sdef->nfields; i++) {
      const char *fname = st->sdef->fields[i].name;
      Type *ft = inst_ty(st, st->sdef->fields[i].ty);
      size_t off = field_offset(st, i);
      NodeRef found = NO_REF;
      if (inits) {
        for (size_t j = 0; j < reflist_len(inits); j++) {
          Node *fi = node_get(reflist_at(inits, j));
          if (fi->kind == NT_FIELDINIT && strcmp(fi->name, fname) == 0)
            found = reflist_at(inits, j);
        }
      }
      size_t n = shape_nlocals(ft);
      if (found != NO_REF) {
        size_t tmp = cx_fresh(cx, ft);
        emit_expr(cx, node_get(found)->a, tmp);
        for (size_t k = 0; k < n; k++) {
          WTy w = local_wty(ft, k);
          const char *str_op = w == W_I64   ? "i64.store"
                               : w == W_F32 ? "f32.store"
                               : w == W_F64 ? "f64.store"
                                            : "i32.store";
          op(cx, "(%s (i32.add (local.get %zu) (i32.const %zu)) %s)\n",
             str_op, dst, off + k * 4, L(cx, tmp + k));
        }
        // the block owns managed field values (fresh retain at store)
        retain(cx, ft, tmp);
      }
      // unset fields read 0 (zeroed allocation law)
    }
    return;
  }

  case NT_INDEX: {
    Type *bt = (Type *)node_get(e->a)->sem;
    size_t b = cx_fresh(cx, bt);
    emit_expr(cx, e->a, b);
    size_t i = cx_fresh(cx, ty_usize);
    emit_expr(cx, e->b, i);
    size_t i32v = cx_fresh(cx, ty_i32);
    op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", i32v, i);
    // bounds check: i >=u len → panic (spec §3)
    size_t msg = data_intern("index out of bounds", 19);
    op(cx, "(if (i32.ge_u (local.get %zu) %s) (then\n", i32v,
       L(cx, b + 1));
    op(cx, "  (call $rho_panic (i32.const %zu) (i32.const 19))))\n", msg);
    if (bt->kind == TY_STRING) {
      op(cx, "(local.set %zu (i32.load8_u (i32.add %s (local.get %zu))))\n",
         dst, L(cx, b), i32v);
    } else {
      Type *el = bt->base;
      size_t esz = type_size(el);
      size_t addr = cx_fresh(cx, ty_i32);
      if (esz == 1)
        op(cx, "(local.set %zu (i32.add %s (local.get %zu)))\n", addr,
           L(cx, b), i32v);
      else
        op(cx, "(local.set %zu (i32.add %s (i32.mul (local.get %zu) "
               "(i32.const %zu))))\n", addr, L(cx, b), i32v, esz);
      size_t n = shape_nlocals(el);
      for (size_t k = 0; k < n; k++) {
        WTy w = local_wty(el, k);
        const char *ld = w == W_I64   ? "i64.load"
                         : w == W_F32 ? "f32.load"
                         : w == W_F64 ? "f64.load"
                                      : "i32.load";
        op(cx, "(local.set %zu (%s (i32.add (local.get %zu) "
               "(i32.const %zu))))\n", dst + k, ld, addr, k * 4);
      }
    }
    return;
  }

  case NT_SLICE_E: {
    // s[a..b]: a view (ptr+a*esz, b-a); bounds-checked both ends
    Type *bt = (Type *)node_get(e->a)->sem;
    size_t b = cx_fresh(cx, bt);
    emit_expr(cx, e->a, b);
    size_t lo = cx_fresh(cx, ty_i32);
    size_t hi = cx_fresh(cx, ty_i32);
    if (e->b != NO_REF) {
      size_t t = cx_fresh(cx, ty_usize);
      emit_expr(cx, e->b, t);
      op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", lo, t);
    } else {
      op(cx, "(local.set %zu (i32.const 0))\n", lo);
    }
    if (e->c != NO_REF) {
      size_t t = cx_fresh(cx, ty_usize);
      emit_expr(cx, e->c, t);
      op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", hi, t);
    } else {
      op(cx, "(local.set %zu %s)\n", hi, L(cx, b + 1));
    }
    size_t msg = data_intern("index out of bounds", 19);
    op(cx, "(if (i32.gt_u (local.get %zu) (local.get %zu)) (then\n", lo,
       hi);
    op(cx, "  (call $rho_panic (i32.const %zu) (i32.const 19))))\n", msg);
    op(cx, "(if (i32.gt_u (local.get %zu) %s) (then\n", hi, L(cx, b + 1));
    op(cx, "  (call $rho_panic (i32.const %zu) (i32.const 19))))\n", msg);
    size_t esz = bt->kind == TY_STRING ? 1 : type_size(bt->base);
    if (esz == 1)
      op(cx, "(local.set %zu (i32.add %s (local.get %zu)))\n", dst,
         L(cx, b), lo);
    else
      op(cx, "(local.set %zu (i32.add %s (i32.mul (local.get %zu) "
             "(i32.const %zu))))\n", dst, L(cx, b), lo, esz);
    op(cx, "(local.set %zu (i32.sub (local.get %zu) (local.get %zu)))\n",
       dst + 1, hi, lo);
    return;
  }

  case NT_SLICE_LIT: {
    Type *st = t;
    size_t esz = type_size(st->base);
    size_t n = reflist_len(e->list);
    size_t blk = cx_fresh(cx, ty_i32);
    op(cx, "(local.set %zu (call $rho_alloc (i32.const %zu)))\n", blk,
       n * esz);
    op(cx, "(local.set %zu (i32.add (local.get %zu) (i32.const 24)))\n",
       dst, blk);
    op(cx, "(local.set %zu (i32.const %zu))\n", dst + 1, n);
    for (size_t i2 = 0; i2 < n; i2++) {
      Node *el = node_get(reflist_at(e->list, i2));
      Type *elt = (Type *)el->sem;
      size_t v = cx_fresh(cx, elt);
      emit_expr(cx, reflist_at(e->list, i2), v);
      size_t nl = shape_nlocals(elt);
      for (size_t k = 0; k < nl; k++) {
        WTy w = local_wty(elt, k);
        const char *strop = w == W_I64   ? "i64.store"
                            : w == W_F32 ? "f32.store"
                            : w == W_F64 ? "f64.store"
                                         : "i32.store";
        op(cx, "(%s (i32.add %s (i32.const %zu)) %s)\n", strop,
           L(cx, dst), i2 * esz + k * 4, L(cx, v + k));
      }
    }
    return;
  }

  case NT_CLOSURE: {
    // captures by copy (immutables only — the checker enforces); the
    // closure fn takes the capture payload as a trailing param
    Type *ft = (Type *)e->sem;
    FnSig *sig = ft->sig;
    // capture set: outer locals referenced inside the body — walk
    Vec caps; // of VarInfo*
    vec_init(&caps, sizeof(VarInfo *));
    {
      extern void closure_collect_captures(FnCx * cx, NodeRef body,
                                           Vec * caps);
      closure_collect_captures(cx, e->c, &caps);
    }
    size_t ncap = VLEN(caps);
    if (getenv("RHO_DEBUG_CLO")) fprintf(stderr, "[clo] ncap=%zu\n", ncap);
    // capture struct payload: each capture's shape inline
    size_t cap_size = 0;
    for (size_t i = 0; i < ncap; i++)
      cap_size += type_size((*VAT(caps, VarInfo *, i))->ty);
    size_t blk = cx_fresh(cx, ty_i32);
    op(cx, "(local.set %zu (call $rho_alloc (i32.const %zu)))\n", blk,
       cap_size ? cap_size : 4);
    op(cx, "(local.set %zu (i32.add (local.get %zu) (i32.const 24)))\n",
       blk, blk);
    // store captures (managed ones retained: the box owns them)
    size_t coff = 0;
    for (size_t i = 0; i < ncap; i++) {
      VarInfo *cv = *VAT(caps, VarInfo *, i);
      size_t n = shape_nlocals(cv->ty);
      for (size_t k = 0; k < n; k++) {
        WTy w = local_wty(cv->ty, k);
        const char *strop = w == W_I64   ? "i64.store"
                            : w == W_F32 ? "f32.store"
                            : w == W_F64 ? "f64.store"
                                         : "i32.store";
        op(cx, "(%s (i32.add (local.get %zu) (i32.const %zu)) %s)\n",
           strop, blk, coff + k * 4, L(cx, cv->vreg + k));
      }
      retain(cx, cv->ty, cv->vreg);
      coff += type_size(cv->ty);
    }
    // synthesize the closure fn: params + trailing capture ptr
    size_t cid = cx->em->closure_n++;
    const char *cname = aprintf(g_arena, "$clofn_%zu", cid);
    {
      Buf b2;
      buf_init(&b2);
      tprintf(&b2, "  (func %s", cname);
      for (size_t i = 0; i < sig->nparams; i++) {
        size_t n = shape_nlocals(sig->params[i].ty);
        for (size_t k = 0; k < n; k++)
          tprintf(&b2, " (param %s)",
                  wty_s(local_wty(sig->params[i].ty, k)));
      }
      tprintf(&b2, " (param i32)"); // capture payload ptr
      if (sig->ret->kind != TY_UNIT) {
        size_t n = shape_nlocals(sig->ret);
        for (size_t k = 0; k < n; k++)
          tprintf(&b2, " (result %s)", wty_s(local_wty(sig->ret, k)));
      }
      tprintf(&b2, "\n");
      // synthesize the body with a nested FnCx sharing locals space
      FnCx cc;
      cx_init(&cc, cx->em, NULL, cname);
      // params registered first
      for (size_t i = 0; i < sig->nparams; i++) {
        size_t v = cx_fresh(&cc, sig->params[i].ty);
        VarInfo *vi = VPUSH(cc.vars, VarInfo);
        vi->name = sig->params[i].name;
        vi->vreg = v;
        vi->managed = type_is_managed(sig->params[i].ty);
        vi->scope = 0;
        vi->ty = sig->params[i].ty;
      }
      size_t capt_local = cx_fresh(&cc, ty_i32);
      (void)capt_local;
      // captures become locals loaded from the capture payload
      size_t off2 = 0;
      for (size_t i = 0; i < ncap; i++) {
        VarInfo *cv = *VAT(caps, VarInfo *, i);
        size_t n = shape_nlocals(cv->ty);
        size_t v = cx_fresh(&cc, cv->ty);
        for (size_t k = 0; k < n; k++) {
          WTy w = local_wty(cv->ty, k);
          const char *ld = w == W_I64   ? "i64.load"
                           : w == W_F32 ? "f32.load"
                           : w == W_F64 ? "f64.load"
                                        : "i32.load";
          op(&cc, "(local.set %zu (%s (i32.add (local.get %zu) "
                  "(i32.const %zu))))\n", v + k, ld, capt_local,
             off2 + k * 4);
        }
        VarInfo *vi = VPUSH(cc.vars, VarInfo);
        vi->name = cv->name;
        vi->vreg = v;
        vi->managed = false; // the box owns them; body treats as borrowed
        vi->scope = 0;
        vi->ty = cv->ty;
        off2 += type_size(cv->ty);
      }
      cc.ret = sig->ret;
      op(&cc, "(loop $tco\n");
      cc.depth++;
      emit_stmt(&cc, e->c);
      cc.depth--;
      op(&cc, ")\n");
      if (cc.ret->kind != TY_UNIT)
        op(&cc, "(unreachable)\n");
      // locals skip the param + capture slots (declared as params)
      size_t nparam_locals2 = 0;
      for (size_t i = 0; i < sig->nparams; i++)
        nparam_locals2 += shape_nlocals(sig->params[i].ty);
      nparam_locals2 += 1; // capture ptr param
      tprintf(&b2, "  (local");
      for (size_t i = nparam_locals2; i < VLEN(cc.localtypes); i++)
        tprintf(&b2, " %s", wty_s(*VAT(cc.localtypes, WTy, i)));
      tprintf(&b2, ")\n");
      tneed(&b2, cc.b.n);
      memcpy(b2.p + b2.n, cc.b.p, cc.b.n);
      b2.n += cc.b.n;
      tprintf(&b2, "  )\n");
      // queue for assembly
      extern void em_queue_text(Em * em, const char *text);
      em_queue_text(cx->em, aprintf(g_arena, "%.*s", (int)b2.n, b2.p));
    }
    // value = (table idx, capture payload)
    size_t ti = closure_table_idx(cx->em, cname);
    op(cx, "(local.set %zu (i32.const %zu))\n", dst, ti);
    op(cx, "(local.set %zu (local.get %zu))\n", dst + 1, blk);
    return;
  }

  case NT_MATCH_EXPR: {
    emit_match(cx, er, dst);
    return;
  }

  case NT_QMARK: {
    // operand Option/Result: tag at vreg; zero-payload = absence
    Type *ot = (Type *)node_get(e->a)->sem;
    size_t v = cx_fresh(cx, ot);
    emit_expr(cx, e->a, v);
    // find the "absent" variant tag (None / Err) — declared order: the
    // prelude's second variant in both cases
    EnumDef *ed = ot->edef;
    int absent_tag = -1;
    for (size_t i = 0; i < ed->nvariants; i++) {
      const char *vn = ed->variants[i].name;
      if (strcmp(vn, "None") == 0 || strcmp(vn, "Err") == 0)
        absent_tag = ed->variants[i].tag;
    }
    op(cx, "(if (i32.eq (local.get %zu) (i32.const %d)) (then\n", v,
       absent_tag);
    // propagate: build the fn's error value from the operand payload
    if (cx->ret && cx->ret->kind == TY_ENUM) {
      size_t ev = cx_fresh(cx, cx->ret);
      // copy the whole operand shape; retag with the absent tag
      size_t n = shape_nlocals(ot);
      for (size_t i = 0; i < n; i++)
        op(cx, "(local.set %zu %s)\n", ev + i, L(cx, v + i));
      op(cx, "(local.set %zu (i32.const %d))\n", ev, absent_tag);
      emit_scope_exit(cx, 0);
      n = shape_nlocals(cx->ret);
      for (size_t i = 0; i < n; i++)
        op(cx, "%s", L(cx, ev + i));
      op(cx, "(return))\n");
    } else {
      op(cx, "(unreachable))\n");
    }
    op(cx, ")\n");
    // value = payload slots
    size_t n = shape_nlocals(ot);
    for (size_t i = 1; i < n; i++)
      op(cx, "(local.set %zu %s)\n", dst + i - 1, L(cx, v + i));
    return;
  }

  default:
    op(cx, ";; UNEMITTED %s\n", node_kind_name(e->kind));
    return;
  }
}

// match: if-chain over arms by tag (br_table is optimizer backlog);
// literal/string arms compare values
static void emit_match(FnCx *cx, NodeRef er, size_t dst) {
  Node *m = node_get(er);
  Type *st = (Type *)node_get(m->a)->sem;
  size_t v = cx_fresh(cx, st);
  emit_expr(cx, m->a, v);
  bool is_enum = st->kind == TY_ENUM;
  (void)is_enum;
  for (size_t i = 0; i < reflist_len(m->list); i++) {
    Node *arm = node_get(reflist_at(m->list, i));
    Node *pat = node_get(arm->a);
    bool wildcard = pat->kind == NT_PWILD;
    if (!wildcard) {
      if (pat->kind == NT_PVAR) {
        EnumVariant *var = NULL;
        for (size_t k = 0; k < st->edef->nvariants; k++)
          if (strcmp(st->edef->variants[k].name,
                     strchr(pat->name, '.') + 1) == 0)
            var = &st->edef->variants[k];
        op(cx, "(if (i32.eq (local.get %zu) (i32.const %d)) (then\n", v,
           var ? var->tag : -1);
      } else if (pat->kind == NT_PLIT) {
        // literal arm: guard on the subject value
        if (pat->op == 0) { // integer
          op(cx, "(if (i32.eq (local.get %zu) (i32.const %d)) (then\n",
             v, (int32_t)(uint32_t)pat->ival);
          if (scalar_wty(st) == W_I64)
            op(cx, ";; i64 literal arm refined below\n");
        } else if (pat->op == 2) { // bool
          op(cx, "(if (i32.eq (local.get %zu) (i32.const %d)) (then\n",
             v, pat->bval ? 1 : 0);
        } else if (pat->op == 3) { // string content
          size_t at = data_intern(pat->sval.p, pat->sval.n);
          op(cx, "(if (call $rho_streq (local.get %zu) (local.get %zu) "
                 "(i32.const %zu) (i32.const %zu)) (then\n", v, v + 1,
             at, pat->sval.n);
        } else {
          op(cx, ";; float literal arm (with the prelude to_str era)\n");
          continue;
        }
      } else {
        continue;
      }
    }
    // bind pattern locals from the payload run
    cx->scope++;
    size_t slot = v + 1;
    register_pattern_binders(cx, pat, st);
    bind_pattern(cx, pat, st, &slot);
    // arm value
    if (dst != SIZE_MAX) {
      emit_expr(cx, arm->b, dst);
    } else {
      Node *ab = node_get(arm->b);
      if (ab->kind == NT_EXPRSTMT && ab->op == 3)
        emit_stmt(cx, arm->b);
      else {
        Type *at = (Type *)ab->sem;
        size_t tmp = cx_fresh(cx, at);
        emit_expr(cx, arm->b, tmp);
        release(cx, at, tmp);
      }
    }
    cx->scope--;
    if (!wildcard)
      op(cx, "))\n");
  }
}

// register binder variables (payload shapes per the variant)
static void register_pattern_binders(FnCx *cx, Node *pat, Type *st) {
  if (pat->kind == NT_PBIND) {
    // binder typed by the checker's sem annotation
    Type *bt = (Type *)pat->sem;
    if (!bt)
      bt = ty_unit;
    size_t v = cx_fresh(cx, bt);
    VarInfo *vi = VPUSH(cx->vars, VarInfo);
    vi->name = pat->name;
    vi->node = NO_REF; // pattern binder
    vi->vreg = v;
    vi->managed = type_is_managed(bt);
    vi->scope = cx->scope;
    vi->ty = bt;
    return;
  }
  if (pat->kind == NT_PVAR) {
    for (size_t i = 0; i < reflist_len(pat->list); i++) {
      Node *sub = node_get(reflist_at(pat->list, i));
      if (pat->op == VAR_STRUCT && sub->kind == NT_FIELD)
        register_pattern_binders(cx, node_get(sub->a), st);
      else
        register_pattern_binders(cx, sub, st);
    }
    return;
  }
}

// bind pattern locals to the subject's payload slots
static void bind_pattern(FnCx *cx, Node *pat, Type *st, size_t *slot) {
  if (pat->kind == NT_PWILD)
    return;
  if (pat->kind == NT_PBIND) {
    VarInfo *v = cx_var(cx, pat->name);
    if (v) {
      size_t n = shape_nlocals(v->ty);
      for (size_t i = 0; i < n; i++)
        op(cx, "(local.set %zu %s)\n", v->vreg + i, L(cx, *slot + i));
      *slot += n;
    }
    return;
  }
  if (pat->kind == NT_PVAR) {
    // variant pattern: sub-patterns bind the variant's fields in order
    for (size_t i = 0; i < reflist_len(pat->list); i++) {
      Node *sub = node_get(reflist_at(pat->list, i));
      if (pat->op == VAR_STRUCT && sub->kind == NT_FIELD)
        bind_pattern(cx, node_get(sub->a), st, slot);
      else if (sub->kind == NT_PBIND || sub->kind == NT_PWILD ||
               sub->kind == NT_PVAR)
        bind_pattern(cx, sub, st, slot);
      // literal sub-patterns consume their slots without binding
      else {
        extern size_t pattern_slot_size(Node *);
        *slot += pattern_slot_size(sub);
      }
    }
    return;
  }
}

size_t pattern_slot_size(Node *p) {
  (void)p;
  return 1; // refined with variant field typing (T1.7c cont)
}

// the payload field type inside a CONSTRUCTED instance (params
// substituted by the instance's args)
Type *variant_field_ty(EnumVariant *var, Type *inst, size_t k) {
  return inst_ty(inst, var->fields[k].ty);
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
  const char *sink = err ? "$eprint_mem" : "$print_mem";
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
      case TY_I64:
        op(cx, "(call $print_i64 %s)\n", L(cx, v));
        break;
      case TY_U64: case TY_USIZE:
        op(cx, "(call $print_u64 %s)\n", L(cx, v));
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
        op(cx, "(if (local.get %zu) (then (call %s (i32.const %zu)"
               " (i32.const 4))) (else (call %s (i32.const %zu)"
               " (i32.const 5))))\n", v, sink, t1, sink, t0);
        break;
      }
      case TY_STRING:
        op(cx, "(call %s %s %s)\n", sink, L(cx, v), L(cx, v + 1));
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
      // literal run up to the next hole; {{ }} collapse to one brace
      size_t j = i;
      while (j < fmt.n &&
             !(j + 1 < fmt.n && fmt.p[j] == '{' && fmt.p[j + 1] == '}')) {
        if (j + 1 < fmt.n && fmt.p[j] == '{' && fmt.p[j + 1] == '{')
          j += 2;
        else if (j + 1 < fmt.n && fmt.p[j] == '}' && fmt.p[j + 1] == '}')
          j += 2;
        else
          j++;
      }
      // materialize the collapsed literal
      char *collapsed = arena_alloc(g_arena, (j - i) + 1, 1);
      size_t cn = 0;
      for (size_t q = i; q < j; q++) {
        if (q + 1 < j && fmt.p[q] == '{' && fmt.p[q + 1] == '{') {
          collapsed[cn++] = '{';
          q++;
        } else if (q + 1 < j && fmt.p[q] == '}' && fmt.p[q + 1] == '}') {
          collapsed[cn++] = '}';
          q++;
        } else {
          collapsed[cn++] = fmt.p[q];
        }
      }
      size_t at = data_intern(collapsed, cn);
      op(cx, "(call %s (i32.const %zu) (i32.const %zu))\n", sink, at, cn);
      i = j;
    }
  }
  (void)err;
}

// ============================================================ calls

// scalar-only arg passing for the T1.7a slice: managed/struct args
// arrive with T1.7b
// pass +1 when the argument is a bare variable (the callee owns its
// params); expression results move their single reference in
static void pass_arg_own(FnCx *cx, NodeRef arg, Type *pt, size_t v) {
  Node *a = node_get(arg);
  if (a->kind == NT_PATH && cx_var(cx, a->name))
    retain(cx, pt, v);
}

static void emit_call_args(FnCx *cx, FnDef *f, RefList *args) {
  size_t nfixed = f->sig->nparams;
  bool variadic = nfixed > 0 && f->sig->params[nfixed - 1].variadic;
  size_t nfixedp = variadic ? nfixed - 1 : nfixed;
  size_t argregs_run[16];
  for (size_t i = 0; i < nfixedp; i++) {
    Type *pt = f->sig->params[i].ty;
    Node *aw = i < reflist_len(args) ? node_get(reflist_at(args, i)) : NULL;
    size_t v = cx_fresh(cx, pt);
    argregs_run[i] = v;
    if (aw && aw->kind == NT_POSARG) {
      emit_expr(cx, aw->a, v);
      pass_arg_own(cx, aw->a, pt, v);
    }
  }
  if (variadic) {
    // materialize the trailing args as a fresh slice (spec §12)
    Type *et = f->sig->params[nfixed - 1].ty->base;
    size_t esz = type_size(et);
    size_t extra = reflist_len(args) > nfixedp ? reflist_len(args) - nfixedp
                                               : 0;
    // spread: one trailing arg already a slice passes through
    if (extra == 1 &&
        node_get(reflist_at(args, nfixedp))->kind == NT_POSARG &&
        node_get(reflist_at(args, nfixedp))->bval) {
      Type *st = f->sig->params[nfixed - 1].ty;
      size_t v = cx_fresh(cx, st);
      argregs_run[nfixedp] = v;
      emit_expr(cx, node_get(reflist_at(args, nfixedp))->a, v);
    } else {
      Type *st = type_slice(et);
      size_t v = cx_fresh(cx, st);
      size_t blk = cx_fresh(cx, ty_i32);
      op(cx, "(local.set %zu (call $rho_alloc (i32.const %zu)))\n", blk,
         extra * esz);
      op(cx, "(local.set %zu (i32.add (local.get %zu) (i32.const 24)))\n",
         v, blk);
      op(cx, "(local.set %zu (i32.const %zu))\n", v + 1, extra);
      for (size_t j = 0; j < extra; j++) {
        Node *aw = node_get(reflist_at(args, nfixedp + j));
        Type *at = (Type *)node_get(aw->a)->sem;
        size_t ev = cx_fresh(cx, at);
        emit_expr(cx, aw->a, ev);
        size_t nl = shape_nlocals(et);
        for (size_t k = 0; k < nl; k++) {
          WTy w = local_wty(et, k);
          const char *strop = w == W_I64   ? "i64.store"
                              : w == W_F32 ? "f32.store"
                              : w == W_F64 ? "f64.store"
                                           : "i32.store";
          op(cx, "(%s (i32.add %s (i32.const %zu)) %s)\n", strop,
             L(cx, v), j * esz + k * 4, L(cx, ev + k));
        }
      }
      argregs_run[nfixedp] = v;
    }
  }
  // push every argument local in order (fat values own runs)
  for (size_t i = 0; i < nfixedp; i++) {
    size_t n = shape_nlocals(f->sig->params[i].ty);
    for (size_t k = 0; k < n; k++)
      op(cx, "%s", L(cx, argregs_run[i] + k));
  }
  if (variadic) {
    op(cx, "%s", L(cx, argregs_run[nfixedp]));
    op(cx, "%s", L(cx, argregs_run[nfixedp] + 1));
  }
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
    vi->name = s->name;
    vi->node = sr;
    vi->vreg = v;
    vi->managed = type_is_managed(t);
    vi->scope = cx->scope;
    vi->ty = t;
    return;
  }
  case NT_RETURN: {
    if (cx->ret->kind == TY_UNIT || s->a == NO_REF) {
      emit_scope_exit(cx, 0);
      op(cx, "(return)\n");
      return;
    }
    // returning a bare variable MOVES it: no release at scope exit
    if (node_get(s->a)->kind == NT_PATH) {
      VarInfo *rv = cx_var(cx, node_get(s->a)->name);
      if (rv)
        rv->moved = true;
    }
    // TCO: a direct self tail call becomes param reassignment + a
    // branch to the function top (spec §9)
    Node *rv = node_get(s->a);
    if (rv->kind == NT_CALL && node_get(rv->a)->kind == NT_PATH &&
        (FnDef *)rv->sem2 == cx->fn) {
      FnDef *f = cx->fn;
      // release old param values, then move the new ones in
      for (size_t i = 0; i < f->sig->nparams; i++) {
        VarInfo *pv = VAT(cx->vars, VarInfo, i);
        release(cx, pv->ty, pv->vreg);
      }
      emit_call_args(cx, f, rv->list);
      // pop results into param locals in reverse
      for (size_t i = f->sig->nparams; i > 0; i--) {
        VarInfo *pv = VAT(cx->vars, VarInfo, i - 1);
        op(cx, "(local.set %zu)\n", pv->vreg);
      }
      op(cx, "(br $tco)\n");
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
  case NT_DEFER: {
    DeferEnt *dnt = VPUSH(cx->defers, DeferEnt);
    dnt->stmt = sr;
    dnt->scope = cx->scope;
    return;
  }

  case NT_EXPRSTMT:
    if (s->op == 3) { // block: defers run LIFO, then vars release
      cx->scope++;
      size_t base_vars = VLEN(cx->vars);
      size_t base_defers = VLEN(cx->defers);
      for (size_t i = 0; i < reflist_len(s->list); i++)
        emit_stmt(cx, reflist_at(s->list, i));
      for (size_t i = VLEN(cx->defers); i > base_defers;) {
        DeferEnt *d = VAT(cx->defers, DeferEnt, --i);
        NodeRef act = node_get(d->stmt)->a;
        Node *an = node_get(act);
        if (an->kind == NT_ASSIGN)
          emit_stmt(cx, act);
        else {
          Type *at2 = (Type *)an->sem;
          size_t tv = cx_fresh(cx, at2 ? at2 : ty_unit);
          emit_expr(cx, act, tv);
          release(cx, at2, tv);
        }
      }
      cx->defers.len = base_defers;
      for (size_t i = VLEN(cx->vars); i > base_vars; i--) {
        VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
        release(cx, v->ty, v->vreg);
      }
      cx->vars.len = base_vars;
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
  case NT_ASSIGN: {
    Type *lt = s->a != NO_REF ? (Type *)node_get(s->a)->sem : NULL;
    if (s->a == NO_REF) {
      // defer-assignment form stores the target in the node's b chain
      return;
    }
    // lvalue: plain local, field (through pointer), or index
    Node *lv = node_get(s->a);
    if (lv->kind == NT_FIELD_E) {
      Type *bt = (Type *)node_get(lv->a)->sem;
      if (bt->kind == TY_PTR && bt->base->kind == TY_STRUCT) {
        Type *st = bt->base;
        int idx = lv->op;
        size_t off = field_offset(st, (size_t)idx);
        Type *ft = inst_ty(st, st->sdef->fields[idx].ty);
        size_t rhs = cx_fresh(cx, lt);
        emit_expr(cx, s->b, rhs);
        size_t p = cx_fresh(cx, bt);
        emit_expr(cx, lv->a, p);
        if (s->op != OP_NONE) {
          // compound: load old, op, store
          size_t oldv = cx_fresh(cx, ft);
          size_t nv = cx_fresh(cx, ft);
          const char *w = scalar_wty(ft) == W_I64 ? "i64" : "i32";
          const char *instr = NULL;
          switch (s->op) {
          case OP_ADD: instr = "add"; break;
          case OP_SUB: instr = "sub"; break;
          case OP_MUL: instr = "mul"; break;
          case OP_BAND: instr = "and"; break;
          case OP_BOR: instr = "or"; break;
          case OP_BXOR: instr = "xor"; break;
          default: break;
          }
          const char *ld = scalar_wty(ft) == W_I64 ? "i64.load" : "i32.load";
          const char *strop = scalar_wty(ft) == W_I64 ? "i64.store"
                                                       : "i32.store";
          op(cx, "(local.set %zu (%s (i32.add (local.get %zu) "
                 "(i32.const %zu))))\n", oldv, ld, p, off);
          if (instr) {
            op(cx, "(local.set %zu (%s.%s (local.get %zu) "
                   "(local.get %zu)))\n", nv, w, instr, oldv, rhs);
            truncate_after(cx, ft, nv);
            op(cx, "(%s (i32.add (local.get %zu) (i32.const %zu)) "
                   "(local.get %zu))\n", strop, p, off, nv);
            return;
          }
        }
        // plain store of the (already computed) rhs
        size_t n = shape_nlocals(ft);
        for (size_t k = 0; k < n; k++) {
          WTy w = local_wty(ft, k);
          const char *strop = w == W_I64   ? "i64.store"
                              : w == W_F32 ? "f32.store"
                              : w == W_F64 ? "f64.store"
                                           : "i32.store";
          op(cx, "(%s (i32.add (local.get %zu) (i32.const %zu)) %s)\n",
             strop, p, off + k * 4, L(cx, rhs + k));
        }
        return;
      }
      return; // inline-value field stores arrive with copies (later)
    }
    if (lv->kind == NT_INDEX) {
      // s[i] = v: bounds-checked store at ptr + i*esz
      Type *bt = (Type *)node_get(lv->a)->sem;
      size_t b = cx_fresh(cx, bt);
      emit_expr(cx, lv->a, b);
      size_t i = cx_fresh(cx, ty_i32);
      {
        size_t iw = cx_fresh(cx, ty_usize);
        emit_expr(cx, lv->b, iw);
        op(cx, "(local.set %zu (i32.wrap_i64 (local.get %zu)))\n", i, iw);
      }
      size_t msg = data_intern("index out of bounds", 19);
      op(cx, "(if (i32.ge_u (local.get %zu) %s) (then\n", i,
         L(cx, b + 1));
      op(cx, "  (call $rho_panic (i32.const %zu) (i32.const 19))))\n", msg);
      Type *el = bt->kind == TY_STRING ? ty_u8 : bt->base;
      size_t esz = type_size(el);
      size_t rhs = cx_fresh(cx, lt);
      emit_expr(cx, s->b, rhs);
      size_t addr = cx_fresh(cx, ty_i32);
      if (esz == 1)
        op(cx, "(local.set %zu (i32.add %s (local.get %zu)))\n", addr,
           L(cx, b), i);
      else
        op(cx, "(local.set %zu (i32.add %s (i32.mul (local.get %zu) "
               "(i32.const %zu))))\n", addr, L(cx, b), i, esz);
      size_t nl = shape_nlocals(el);
      for (size_t k = 0; k < nl; k++) {
        WTy w = local_wty(el, k);
        const char *strop2 = w == W_I64   ? "i64.store"
                             : w == W_F32 ? "f32.store"
                             : w == W_F64 ? "f64.store"
                                          : "i32.store";
        op(cx, "(%s (i32.add (local.get %zu) (i32.const %zu)) %s)\n",
           strop2, addr, k * 4, L(cx, rhs + k));
      }
      return;
    }
    if (lv->kind == NT_PATH) {
      VarInfo *v = cx_var(cx, lv->name);
      if (!v) {
        // module static: load-modify-store on its memory slot
        Sym *ss = NULL;
        for (Module *m2 = cx->p->modules; m2; m2 = m2->next)
          if (m2->syms && (ss = symtab_get(m2->syms, lv->name)) &&
              ss->kind == SYM_STATIC)
            break;
        if (!ss)
          return;
        size_t slot = static_slot(cx->fn ? cx->fn->mod : cx->p->entry,
                                  lv->name);
        size_t rhs = cx_fresh(cx, lt);
        emit_expr(cx, s->b, rhs);
        if (s->op == OP_NONE) {
          size_t n = shape_nlocals(lt);
          for (size_t i = 0; i < n; i++)
            op(cx, "(i32.store (i32.const %zu) %s)\n", slot + i * 4,
               L(cx, rhs + i));
          return;
        }
        size_t oldv = cx_fresh(cx, lt);
        size_t nv = cx_fresh(cx, lt);
        size_t n = shape_nlocals(lt);
        for (size_t i = 0; i < n; i++)
          op(cx, "(local.set %zu (i32.load (i32.const %zu)))\n", oldv + i,
             slot + i * 4);
        const char *w = scalar_wty(lt) == W_I64 ? "i64" : "i32";
        const char *instr = NULL;
        switch (s->op) {
        case OP_ADD: instr = "add"; break;
        case OP_SUB: instr = "sub"; break;
        case OP_MUL: instr = "mul"; break;
        case OP_BAND: instr = "and"; break;
        case OP_BOR: instr = "or"; break;
        case OP_BXOR: instr = "xor"; break;
        default: break;
        }
        if (instr) {
          op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
             nv, w, instr, oldv, rhs);
          truncate_after(cx, lt, nv);
        } else {
          emit_div(cx, s->op, lt, nv, oldv, rhs);
          truncate_after(cx, lt, nv);
        }
        for (size_t i = 0; i < n; i++)
          op(cx, "(i32.store (i32.const %zu) %s)\n", slot + i * 4,
             L(cx, nv + i));
        return;
      }
      size_t rhs = cx_fresh(cx, lt);
      emit_expr(cx, s->b, rhs);
      if (s->op != OP_NONE) {
        // compound: op(old, rhs) into a fresh result
        size_t res = cx_fresh(cx, lt);
        const char *w = scalar_wty(lt) == W_I64 ? "i64" : "i32";
        const char *instr = NULL;
        bool uns = lt->kind >= TY_U8;
        switch (s->op) {
        case OP_ADD: instr = "add"; break;
        case OP_SUB: instr = "sub"; break;
        case OP_MUL: instr = "mul"; break;
        case OP_BAND: instr = "and"; break;
        case OP_BOR: instr = "or"; break;
        case OP_BXOR: instr = "xor"; break;
        default: break;
        }
        if (instr) {
          op(cx, "(local.set %zu (%s.%s (local.get %zu) (local.get %zu)))\n",
             res, w, instr, v->vreg, rhs);
          truncate_after(cx, lt, res);
          size_t n = shape_nlocals(lt);
          for (size_t i = 0; i < n; i++)
            op(cx, "(local.set %zu %s)\n", v->vreg + i, L(cx, res + i));
          return;
        }
        if (s->op == OP_DIV || s->op == OP_MOD) {
          emit_div(cx, s->op, lt, res, v->vreg, rhs);
          truncate_after(cx, lt, res);
          size_t n = shape_nlocals(lt);
          for (size_t i = 0; i < n; i++)
            op(cx, "(local.set %zu %s)\n", v->vreg + i, L(cx, res + i));
          return;
        }
        // shifts mask by the storage width (spec §4)
        int bits = scalar_wty(lt) == W_I64 ? 64 : 32;
        if (scalar_wty(lt) == W_I64)
          op(cx, "(local.set %zu (i64.%s (local.get %zu) (i64.and "
                 "(local.get %zu) (i64.const %d))))\n", res,
             s->op == OP_SHL ? "shl" : (uns ? "shr_u" : "shr_s"), v->vreg,
             rhs, bits - 1);
        else
          op(cx, "(local.set %zu (i32.%s (local.get %zu) (i32.and "
                 "(local.get %zu) (i32.const %d))))\n", res,
             s->op == OP_SHL ? "shl" : (uns ? "shr_u" : "shr_s"), v->vreg,
             rhs, bits - 1);
        truncate_after(cx, lt, res);
        op(cx, "(local.set %zu (local.get %zu))\n", v->vreg, res);
        return;
      }
      size_t n = shape_nlocals(lt);
      // overwrite: release the old managed value, move the new one in
      release(cx, lt, v->vreg);
      for (size_t i = 0; i < n; i++)
        op(cx, "(local.set %zu %s)\n", v->vreg + i, L(cx, rhs + i));
      return;
    }
    return; // field/index lvalues arrive with T1.7b
  }

  case NT_IF: {
    if (s->op == 1) {
      // comptime-folded: only the live branch exists
      cx->scope++;
      size_t base = VLEN(cx->vars);
      if (s->ival)
        emit_stmt(cx, s->b);
      else if (s->c != NO_REF)
        emit_stmt(cx, s->c);
      for (size_t i = VLEN(cx->vars); i > base; i--) {
        VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
        release(cx, v->ty, v->vreg);
      }
      cx->vars.len = base;
      cx->scope--;
      return;
    }
    size_t c = cx_fresh(cx, ty_bool);
    emit_expr(cx, s->a, c);
    op(cx, "(if (local.get %zu) (then\n", c);
    cx->depth++;
    emit_stmt(cx, s->b);
    if (s->c != NO_REF) {
      op(cx, ") (else\n");
      emit_stmt(cx, s->c);
    }
    op(cx, "))\n");
    cx->depth--;
    return;
  }

  case NT_WHILE: {
    // (block $exit (loop $top (br_if $exit !(cond)) body (br $top)))
    size_t c = cx_fresh(cx, ty_bool);
    op(cx, "(block $b%zu\n", cx->em->label_n);
    int exit_d = ++cx->depth;
    LabEnt *lab = VPUSH(cx->labels, LabEnt);
    lab->name = s->name;
    lab->brk_depth = exit_d;
    lab->scope = cx->scope + 1;
    op(cx, "(loop $l%zu\n", cx->em->label_n);
    lab->cont_depth = ++cx->depth;
    size_t loop_id = cx->em->label_n++;
    emit_expr(cx, s->a, c);
    op(cx, "(br_if $b%zu (i32.eqz (local.get %zu)))\n", loop_id, c);
    cx->scope++;
    size_t base = VLEN(cx->vars);
    emit_stmt(cx, s->b);
    for (size_t i = VLEN(cx->vars); i > base; i--) {
      VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
      release(cx, v->ty, v->vreg);
    }
    cx->vars.len = base;
    cx->scope--;
    op(cx, "(br $l%zu))\n", loop_id);
    op(cx, ")\n");
    cx->depth -= 2;
    cx->labels.len--;
    return;
  }

  case NT_LOOP: {
    op(cx, "(block $b%zu\n", cx->em->label_n);
    int exit_d = ++cx->depth;
    LabEnt *lab = VPUSH(cx->labels, LabEnt);
    lab->name = s->name;
    lab->brk_depth = exit_d;
    lab->scope = cx->scope + 1;
    op(cx, "(loop $l%zu\n", cx->em->label_n);
    lab->cont_depth = ++cx->depth;
    size_t loop_id = cx->em->label_n++;
    cx->scope++;
    size_t base = VLEN(cx->vars);
    emit_stmt(cx, s->b);
    for (size_t i = VLEN(cx->vars); i > base; i--) {
      VarInfo *v = VAT(cx->vars, VarInfo, i - 1);
      release(cx, v->ty, v->vreg);
    }
    cx->vars.len = base;
    cx->scope--;
    op(cx, "(br $l%zu))\n", loop_id);
    op(cx, ")\n");
    cx->depth -= 2;
    cx->labels.len--;
    return;
  }

  case NT_BREAK:
  case NT_CONTINUE: {
    // find the target loop (innermost unnamed, or by label)
    LabEnt *target = NULL;
    if (s->name) {
      for (size_t i = VLEN(cx->labels); i > 0; i--) {
        LabEnt *l = VAT(cx->labels, LabEnt, i - 1);
        if (l->name && strcmp(l->name, s->name) == 0) {
          target = l;
          break;
        }
      }
    } else {
      for (size_t i = VLEN(cx->labels); i > 0; i--) {
        LabEnt *l = VAT(cx->labels, LabEnt, i - 1);
        target = l;
        break;
      }
    }
    if (!target)
      return;
    // exits crossing scopes run their defers (T1.7b completes the
    // defers part; releases below keep memory honest)
    emit_scope_exit(cx, target->scope);
    int want = s->kind == NT_BREAK ? target->brk_depth : target->cont_depth;
    int rel = cx->depth - want;
    op(cx, "(br %d)\n", rel);
    return;
  }

  case NT_MATCH: {
    emit_match(cx, sr, MATCH_DISCARD);
    return;
  }

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
    NodeRef act = node_get(d->stmt)->a;
    Node *an = node_get(act);
    if (an->kind == NT_ASSIGN)
      emit_stmt(cx, act);
    else {
      Type *at2 = (Type *)an->sem;
      size_t tv = cx_fresh(cx, at2 ? at2 : ty_unit);
      emit_expr(cx, act, tv);
      release(cx, at2, tv);
    }
  }
  for (size_t i = VLEN(cx->vars); i > 0;) {
    VarInfo *v = VAT(cx->vars, VarInfo, --i);
    if (v->scope < to_scope)
      break;
    release_var(cx, v);
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
    vi->name = node_get(f->sig->params[i].decl)->name;
    vi->node = f->sig->params[i].decl;
    vi->vreg = v;
    vi->managed = type_is_managed(pt);
    vi->scope = 0;
    vi->ty = pt;
  }
  op(&cx, "(loop $tco\n");
  cx.depth++;
  emit_stmt(&cx, f->body);
  cx.depth--;
  op(&cx, ")\n");
  if (cx.ret->kind != TY_UNIT)
    op(&cx, "(unreachable)\n"); // every path returns explicitly

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
  // wasm declares params separately: the local list starts after them
  size_t nparam_locals = 0;
  for (size_t i = 0; i < f->sig->nparams; i++) {
    Type *pt = f->sig->params[i].ty ? f->sig->params[i].ty : ty_unit;
    nparam_locals += shape_nlocals(pt);
  }
  for (size_t i = nparam_locals; i < VLEN(cx.localtypes); i++)
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
  // statics initialize from comptime-folded values (module × symbol
  // order, deterministic)
  for (Module *m = em->p->modules; m; m = m->next) {
    if (!m->syms)
      continue;
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_STATIC)
        continue;
      size_t slot = static_slot(m, s->name);
      CVal *cv = s->u.konst->cval;
      if (!cv)
        continue;
      switch (cv->kind) {
      case CV_INT: case CV_UINT:
        if (cv->ty && scalar_wty(cv->ty) == W_I64)
          tprintf(&b, "    (i64.store (i32.const %zu) (i64.const %lld))\n",
                  slot, (long long)cv->u);
        else
          tprintf(&b, "    (i32.store (i32.const %zu) (i32.const %d))\n",
                  slot, (int32_t)cv->u);
        break;
      case CV_FLOAT:
        if (cv->ty && cv->ty->kind == TY_F32)
          tprintf(&b, "    (f32.store (i32.const %zu) (f32.const %.9g))\n",
                  slot, cv->f);
        else
          tprintf(&b, "    (f64.store (i32.const %zu) (f64.const %.17g))\n",
                  slot, cv->f);
        break;
      case CV_BOOL:
        tprintf(&b, "    (i32.store (i32.const %zu) (i32.const %d))\n",
                slot, cv->b ? 1 : 0);
        break;
      case CV_STR: {
        size_t at = data_intern(cv->s.p, cv->s.n);
        tprintf(&b, "    (i32.store (i32.const %zu) (i32.const %zu))\n",
                slot, at);
        tprintf(&b, "    (i32.store (i32.const %zu) (i32.const %zu))\n",
                slot + 4, cv->s.n);
        break;
      }
      }
    }
  }
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
  // string concat: returns a fresh block {ptr,len} via two globals
  tprintf(o, "  (global $cat_ptr (mut i32) (i32.const 0))\n");
  tprintf(o, "  (global $cat_len (mut i32) (i32.const 0))\n");
  tprintf(o, "  (func $rho_cat2 (param $a i32) (param $al i32) "
             "(param $b i32) (param $bl i32)\n");
  tprintf(o, "    (local $p i32) (local $i i32)\n");
  tprintf(o, "    (local.set $p (call $rho_alloc (i32.add (local.get $al) "
             "(local.get $bl))))\n");
  tprintf(o, "    (local.set $i (i32.const 0))\n");
  tprintf(o, "    (block $d1 (loop $c1 (br_if $d1 (i32.ge_u (local.get $i) "
             "(local.get $al)))\n");
  tprintf(o, "      (i32.store8 (i32.add (i32.add (local.get $p) "
             "(i32.const 24)) (local.get $i))\n");
  tprintf(o, "        (i32.load8_u (i32.add (local.get $a) "
             "(local.get $i))))\n");
  tprintf(o, "      (local.set $i (i32.add (local.get $i) (i32.const 1))) "
             "(br $c1)))\n");
  tprintf(o, "    (local.set $i (i32.const 0))\n");
  tprintf(o, "    (block $d2 (loop $c2 (br_if $d2 (i32.ge_u (local.get $i) "
             "(local.get $bl)))\n");
  tprintf(o, "      (i32.store8 (i32.add (i32.add (i32.add (local.get $p) "
             "(i32.const 24)) (local.get $al)) (local.get $i))\n");
  tprintf(o, "        (i32.load8_u (i32.add (local.get $b) "
             "(local.get $i))))\n");
  tprintf(o, "      (local.set $i (i32.add (local.get $i) (i32.const 1))) "
             "(br $c2)))\n");
  tprintf(o, "    (global.set $cat_ptr (i32.add (local.get $p) "
             "(i32.const 24)))\n");
  tprintf(o, "    (global.set $cat_len (i32.add (local.get $al) "
             "(local.get $bl))))\n");
  tprintf(o, "  (func $rho_streq (param $a i32) (param $al i32) "
             "(param $b i32) (param $bl i32) (result i32)\n");
  tprintf(o, "    (local $i i32)\n");
  tprintf(o, "    (if (i32.ne (local.get $al) (local.get $bl)) "
             "(then (return (i32.const 0))))\n");
  tprintf(o, "    (local.set $i (i32.const 0))\n");
  tprintf(o, "    (block $d (loop $c (br_if $d (i32.ge_u (local.get $i) "
             "(local.get $al)))\n");
  tprintf(o, "      (if (i32.ne (i32.load8_u (i32.add (local.get $a) "
             "(local.get $i)))\n");
  tprintf(o, "              (i32.load8_u (i32.add (local.get $b) "
             "(local.get $i))))\n");
  tprintf(o, "        (then (return (i32.const 0))))\n");
  tprintf(o, "      (local.set $i (i32.add (local.get $i) (i32.const 1))) "
             "(br $c)))\n");
  tprintf(o, "    (i32.const 1))\n");
  // functions (user fns first — drop walkers registered during their
  // emission — then the walkers + funcref table)
  for (size_t i = 0; i < VLEN(em.fns); i++) {
    EFn *f = VAT(em.fns, EFn, i);
    tneed(o, f->wat_len);
    memcpy(o->p + o->n, f->wat, f->wat_len);
    o->n += f->wat_len;
  }
  // closure fn-type section
  for (size_t i = 0; i < VLEN(g_clotypes); i++) {
    CloTy *ct = VAT(g_clotypes, CloTy, i);
    FnSig *sg = ct->sig;
    tprintf(&em.fnbuf, "  (type %s (func", ct->tname);
    for (size_t k = 0; k < sg->nparams; k++) {
      size_t n = shape_nlocals(sg->params[k].ty);
      for (size_t j = 0; j < n; j++)
        tprintf(&em.fnbuf, " (param %s)",
                wty_s(local_wty(sg->params[k].ty, j)));
    }
    tprintf(&em.fnbuf, " (param i32)");
    if (sg->ret->kind != TY_UNIT) {
      size_t n = shape_nlocals(sg->ret);
      for (size_t j = 0; j < n; j++)
        tprintf(&em.fnbuf, " (result %s)", wty_s(local_wty(sg->ret, j)));
    }
    tprintf(&em.fnbuf, "))\n");
  }

  // drop walkers + the funcref table (fnbuf), after all functions
  emit_dropfns_and_table(&em);
  tneed(o, em.fnbuf.n);
  memcpy(o->p + o->n, em.fnbuf.p, em.fnbuf.n);
  o->n += em.fnbuf.n;

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
