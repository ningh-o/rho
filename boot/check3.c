// check3.c — T1.6: errors and folding. Comptime evaluation (one `as`
// semantics for constants and variables), const inference, --set with
// the widened type face, ? compatibility, dead-branch folding and use
// pruning.
#include "sem.h"

#include <stdarg.h>
#include <errno.h>

// ---------------------------------------------------------------- values

#include "cval.h"

// ---------------------------------------------------------------- fold

// A minimal, deterministic comptime evaluator over the expression
// forms the design allows in const initializers and build-parameter
// conditions: literals, names from `env`, unary/binary operators, `as`
// with the unified truncating semantics.
typedef struct FoldEnv {
  SymTab *consts; // name → SYM_CONST with a resolved CVal
  struct FoldEnv *up;
} FoldEnv;

static CVal g_bad; // kind sentinel for failure; zeroed = CV_INT

static bool cv_ok(CVal v) { return v.ty != NULL; }

static CVal cv_int(Type *t, uint64_t raw) {
  CVal v;
  memset(&v, 0, sizeof v);
  v.kind = type_is_int(t) && t->kind >= TY_U8 ? CV_UINT : CV_INT;
  v.ty = t;
  v.i = (int64_t)raw;
  v.u = raw;
  return v;
}

static CVal fold_expr(FoldEnv *env, NodeRef er);

static uint64_t trunc_to(uint64_t v, Type *t) {
  switch (t->kind) {
  case TY_I8:
  case TY_U8:
    return (uint8_t)v;
  case TY_I16:
  case TY_U16:
    return (uint16_t)v;
  case TY_I32:
  case TY_U32:
    return (uint32_t)v;
  default:
    return v;
  }
}

static CVal fold_as(CVal v, Type *to) {
  if (!cv_ok(v))
    return v;
  CVal r;
  memset(&r, 0, sizeof r);
  r.ty = to;
  switch (to->kind) {
  case TY_I8: case TY_I16: case TY_I32: case TY_I64:
  case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE: {
    uint64_t raw;
    if (v.kind == CV_FLOAT) {
      // truncate toward zero, saturate out of range
      double d = v.f;
      if (d != d)
        raw = 0; // NaN → 0
      else if (d >= 9223372036854775807.0)
        raw = UINT64_MAX;
      else if (d <= -9223372036854775808.0)
        raw = 0x8000000000000000ULL;
      else
        raw = (uint64_t)(int64_t)d;
    } else {
      raw = v.u;
    }
    // wrap/truncate two's complement to the target width — the ONE
    // semantics, constants exactly like variables
    raw = trunc_to(raw, to);
    if (to->kind >= TY_I8 && to->kind <= TY_I64) {
      // sign-extend view
      int bits = to->kind == TY_I8 ? 8
                 : to->kind == TY_I16 ? 16
                 : to->kind == TY_I32 ? 32 : 64;
      if (bits < 64 && (raw >> (bits - 1)) & 1)
        raw |= UINT64_MAX << bits;
    }
    r.kind = to->kind >= TY_U8 ? CV_UINT : CV_INT;
    r.u = raw;
    r.i = (int64_t)raw;
    return r;
  }
  case TY_F32:
  case TY_F64: {
    double d;
    if (v.kind == CV_FLOAT)
      d = v.f;
    else
      d = (double)((int64_t)v.u);
    r.kind = CV_FLOAT;
    r.f = to->kind == TY_F32 ? (double)(float)d : d;
    return r;
  }
  default:
    return g_bad;
  }
}

static CVal fold_binop(int op, CVal a, CVal b) {
  if (!cv_ok(a) || !cv_ok(b))
    return g_bad;
  // canonicalize integer operands to the LEFT operand's width/signedness
  // (the same domain the runtime uses — spec §4)
  if (a.kind != CV_FLOAT && b.kind != CV_FLOAT && a.kind != CV_STR &&
      type_is_int(a.ty)) {
    uint64_t r = trunc_to(b.u, a.ty);
    b.u = r;
    b.i = (int64_t)r;
    b.ty = a.ty;
    a.u = trunc_to(a.u, a.ty);
    a.i = (int64_t)a.u;
  }
  // string concatenation folds
  if (op == OP_ADD && a.kind == CV_STR && b.kind == CV_STR) {
    CVal r;
    memset(&r, 0, sizeof r);
    r.ty = ty_string;
    r.kind = CV_STR;
    char *buf = arena_alloc(g_arena, a.s.n + b.s.n + 1, 1);
    memcpy(buf, a.s.p, a.s.n);
    memcpy(buf + a.s.n, b.s.p, b.s.n);
    r.s = str_slice(buf, a.s.n + b.s.n);
    return r;
  }
  if (a.kind == CV_STR || b.kind == CV_STR)
    return g_bad;
  bool isf = a.kind == CV_FLOAT || b.kind == CV_FLOAT;
  Type *rt = isf ? (a.ty->kind == TY_F64 || b.ty->kind == TY_F64 ? ty_f64
                                                                  : ty_f32)
                 : a.ty;
  if (isf) {
    double x = a.kind == CV_FLOAT ? a.f : (double)a.i;
    double y = b.kind == CV_FLOAT ? b.f : (double)b.i;
    double v = 0;
    switch (op) {
    case OP_ADD: v = x + y; break;
    case OP_SUB: v = x - y; break;
    case OP_MUL: v = x * y; break;
    case OP_DIV: v = x / y; break;
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
      CVal r;
      memset(&r, 0, sizeof r);
      r.ty = ty_bool;
      r.kind = CV_BOOL;
      switch (op) {
      case OP_LT: r.b = x < y; break;
      case OP_LE: r.b = x <= y; break;
      case OP_GT: r.b = x > y; break;
      case OP_GE: r.b = x >= y; break;
      case OP_EQ: r.b = x == y; break;
      default: r.b = x != y; break;
      }
      return r;
    }
    default: return g_bad;
    }
    CVal r;
    memset(&r, 0, sizeof r);
    r.ty = rt;
    r.kind = CV_FLOAT;
    r.f = v;
    return r;
  }
  // integer domain (u64 two's complement arithmetic = wrap semantics)
  uint64_t x = a.u, y = b.u;
  uint64_t v = 0;
  switch (op) {
  case OP_ADD: v = x + y; break;
  case OP_SUB: v = x - y; break;
  case OP_MUL: v = x * y; break;
  case OP_DIV:
    if (y == 0)
      return g_bad; // /0 is a runtime panic; not foldable here
    v = (uint64_t)((int64_t)x / (int64_t)y);
    break;
  case OP_MOD:
    if (y == 0)
      return g_bad;
    v = (uint64_t)((int64_t)x % (int64_t)y);
    break;
  case OP_BAND: v = x & y; break;
  case OP_BOR: v = x | y; break;
  case OP_BXOR: v = x ^ y; break;
  case OP_SHL: {
    int bits = a.ty->kind == TY_I8 || a.ty->kind == TY_U8 ? 8
               : a.ty->kind == TY_I16 || a.ty->kind == TY_U16 ? 16
               : a.ty->kind == TY_I32 || a.ty->kind == TY_U32 ? 32 : 64;
    v = x << (y & (bits - 1)); // shift masks by the left width
    break;
  }
  case OP_SHR: {
    int bits = a.ty->kind == TY_I8 || a.ty->kind == TY_U8 ? 8
               : a.ty->kind == TY_I16 || a.ty->kind == TY_U16 ? 16
               : a.ty->kind == TY_I32 || a.ty->kind == TY_U32 ? 32 : 64;
    uint64_t m = y & (bits - 1);
    if (a.ty->kind >= TY_I8 && a.ty->kind <= TY_I64 && bits < 64 &&
        ((x >> (bits - 1)) & 1))
      v = (x >> m) | (UINT64_MAX << (bits - m)); // arithmetic shift
    else
      v = x >> m;
    break;
  }
  case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
    // signed types compare at their own width (sign-extended)
    int64_t sx = (int64_t)x, sy = (int64_t)y;
    if (a.ty->kind == TY_I8) {
      sx = (int8_t)x;
      sy = (int8_t)y;
    } else if (a.ty->kind == TY_I16) {
      sx = (int16_t)x;
      sy = (int16_t)y;
    } else if (a.ty->kind == TY_I32) {
      sx = (int32_t)x;
      sy = (int32_t)y;
    }
    // unsigned types compare unsigned
    bool uu = a.ty->kind >= TY_U8;
    bool bv;
    switch (op) {
    case OP_EQ: bv = x == y; break;
    case OP_NE: bv = x != y; break;
    case OP_LT: bv = uu ? x < y : sx < sy; break;
    case OP_LE: bv = uu ? x <= y : sx <= sy; break;
    case OP_GT: bv = uu ? x > y : sx > sy; break;
    default: bv = uu ? x >= y : sx >= sy; break;
    }
    CVal r;
    memset(&r, 0, sizeof r);
    r.ty = ty_bool;
    r.kind = CV_BOOL;
    r.b = bv;
    return r;
  }
  case OP_AND: {
    CVal r;
    memset(&r, 0, sizeof r);
    r.ty = ty_bool;
    r.kind = CV_BOOL;
    r.b = x && y;
    return r;
  }
  case OP_OR: {
    CVal r;
    memset(&r, 0, sizeof r);
    r.ty = ty_bool;
    r.kind = CV_BOOL;
    r.b = x || y;
    return r;
  }
  default:
    return g_bad;
  }
  // wrap to the operand type width
  Type *wide = a.ty->kind >= TY_I8 && a.ty->kind <= TY_I64
                   ? a.ty
                   : (rt->kind >= TY_I8 && rt->kind <= TY_I64 ? rt : a.ty);
  v = trunc_to(v, wide ? wide : ty_i64);
  CVal r;
  memset(&r, 0, sizeof r);
  r.ty = wide ? wide : ty_i64;
  r.kind = r.ty->kind >= TY_U8 ? CV_UINT : CV_INT;
  r.u = v;
  r.i = (int64_t)v;
  return r;
}

static CVal fold_expr(FoldEnv *env, NodeRef er) {
  if (er == NO_REF)
    return g_bad;
  Node *e = node_get(er);
  switch (e->kind) {
  case NT_INT:
    return cv_int(ty_i32, e->ival);
  case NT_FLOAT: {
    CVal v;
    memset(&v, 0, sizeof v);
    v.ty = ty_f64;
    v.kind = CV_FLOAT;
    v.f = e->fval;
    return v;
  }
  case NT_BOOL: {
    CVal v;
    memset(&v, 0, sizeof v);
    v.ty = ty_bool;
    v.kind = CV_BOOL;
    v.b = e->bval;
    return v;
  }
  case NT_STR: {
    CVal v;
    memset(&v, 0, sizeof v);
    v.ty = ty_string;
    v.kind = CV_STR;
    v.s = e->sval;
    return v;
  }
  case NT_PATH: {
    for (FoldEnv *fe = env; fe; fe = fe->up) {
      if (!fe->consts)
        continue;
      Sym *s = symtab_get(fe->consts, e->name);
      if (s && s->kind == SYM_CONST && s->u.konst->cval)
        return *(CVal *)s->u.konst->cval;
    }
    return g_bad;
  }
  case NT_UNARY: {
    CVal v = fold_expr(env, e->a);
    if (!cv_ok(v))
      return g_bad;
    if (e->op == OP_NEG) {
      if (v.kind == CV_FLOAT) {
        v.f = -v.f;
        return v;
      }
      return cv_int(v.ty, (uint64_t)(0 - (int64_t)v.u));
    }
    if (e->op == OP_BITNOT)
      return cv_int(v.ty, ~v.u);
    if (e->op == OP_NOT) {
      v.b = !v.b;
      return v;
    }
    return g_bad;
  }
  case NT_BINARY: {
    CVal a = fold_expr(env, e->a);
    CVal b = fold_expr(env, e->b);
    return fold_binop(e->op, a, b);
  }
  case NT_AS: {
    CVal v = fold_expr(env, e->a);
    Type *to = resolve_type_pub(g_entry_mod, e->b, NULL);
    return fold_as(v, to);
  }
  default:
    return g_bad;
  }
}

// fold a condition against a module's consts (plus the root's)
static bool fold_cond_modules(Module *m, NodeRef er, bool *out) {
  FoldEnv root = {g_entry_mod ? g_entry_mod->syms : NULL, NULL};
  FoldEnv env = {m ? m->syms : NULL, &root};
  CVal v = fold_expr(&env, er);
  if (getenv("RHO_DEBUG_FOLD"))
    fprintf(stderr, "[cond] kind=%d -> ok=%d kind=%d b=%d\n",
            (int)node_get(er)->kind, (int)cv_ok(v), (int)v.kind,
            v.b ? 1 : 0);
  if (!cv_ok(v) || v.kind != CV_BOOL)
    return false;
  *out = v.b;
  return true;
}

// ============================================================ const resolve

// Fixpoint per module: infer types and values for every const whose
// initializer folds from literals and already-resolved consts.
static void resolve_module_consts(Module *m, bool report) {
  bool progress = true;
  size_t resolved = 0, total = 0;
  for (Sym *s = m->syms->order_head; s; s = s->order_next)
    if (s->kind == SYM_CONST)
      total++;
  while (progress && resolved < total) {
    progress = false;
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_CONST || s->u.konst->cval)
        continue;
      ConstDef *cd = s->u.konst;
      FoldEnv root = {g_entry_mod && g_entry_mod != m
                          ? g_entry_mod->syms
                          : NULL, NULL};
      FoldEnv env = {m->syms, &root};
      Node *d = node_get(cd->decl);
      // annotation pinning: fold under the annotation's type face
      CVal v = fold_expr(&env, cd->init);
      if (!cv_ok(v))
        continue;
      if (d->kind == NT_CONST && d->a != NO_REF) {
        Type *ann = resolve_type_pub(m, d->a, NULL);
        if (ann && type_is_num(ann)) {
          v = fold_as(v, ann);
        } else if (ann && ann->kind == TY_STRING) {
          if (v.kind != CV_STR)
            continue;
          v.ty = ty_string;
        } else if (ann && ann->kind == TY_BOOL) {
          if (v.kind != CV_BOOL)
            continue;
          v.ty = ty_bool;
        }
      }
      CVal *slot = arena_alloc(g_arena, sizeof(CVal), 8);
      *slot = v;
      cd->cval = slot;
      if (!cd->ty)
        cd->ty = v.ty;
      resolved++;
      progress = true;
    }
  }
  if (report && resolved < total)
    for (Sym *s = m->syms->order_head; s; s = s->order_next)
      if (s->kind == SYM_CONST && !s->u.konst->cval) {
        Node *d = node_get(s->u.konst->decl);
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "const '%s' initializer is not comptime-evaluable "
                "(or is cyclic)",
                s->name);
      }
}

// entry consts pre-resolve (before the module graph loads, so dead
// branches in the root prune their uses)
void pre_resolve_entry_consts(Module *entry) {
  if (!entry->syms)
    return; // not collected yet: raw parse-level pass below
  resolve_module_consts(entry, false);
}

// parse a --set value against a const's type; false = refusal
static bool parse_set_value(ConstDef *cd, const char *text, CVal *out) {
  memset(out, 0, sizeof *out);
  Type *t = cd->ty;
  if (!t)
    return false;
  char *end = NULL;
  switch (t->kind) {
  case TY_BOOL:
    if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0) {
      out->ty = ty_bool;
      out->kind = CV_BOOL;
      out->b = text[0] == 't';
      return true;
    }
    return false;
  case TY_STRING: {
    // text form: bare or "quoted" (quotes stripped, escapes raw)
    size_t n = strlen(text);
    const char *p = text;
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
      p++;
      n -= 2;
    }
    char *buf = arena_alloc(g_arena, n + 1, 1);
    memcpy(buf, p, n);
    out->ty = ty_string;
    out->kind = CV_STR;
    out->s = str_slice(buf, n);
    return true;
  }
  case TY_F32:
  case TY_F64: {
    double d = strtod(text, &end);
    if (end == text || *end)
      return false;
    out->ty = t;
    out->kind = CV_FLOAT;
    out->f = t->kind == TY_F32 ? (double)(float)d : d;
    return true;
  }
  default: { // all integer widths, range-checked
    if (!type_is_int(t))
      return false;
    errno = 0;
    if (text[0] == '-')
      out->i = strtoll(text, &end, 0);
    else
      out->u = strtoull(text, &end, 0);
    if (end == text || *end || errno == ERANGE)
      return false;
    uint64_t raw = text[0] == '-' ? (uint64_t)out->i : out->u;
    // range check against the width/sign
    uint64_t hi = UINT64_MAX;
    switch (t->kind) {
    case TY_I8: hi = 0x7F; break;
    case TY_I16: hi = 0x7FFF; break;
    case TY_I32: hi = 0x7FFFFFFF; break;
    case TY_I64: hi = UINT64_MAX; break;
    case TY_U8: hi = 0xFF; break;
    case TY_U16: hi = 0xFFFF; break;
    case TY_U32: hi = 0xFFFFFFFF; break;
    default: break;
    }
    if (text[0] == '-' && t->kind >= TY_I8 && t->kind <= TY_I64) {
      int64_t sv = (int64_t)raw;
      int64_t slo = t->kind == TY_I8   ? -128
                    : t->kind == TY_I16 ? -32768
                    : t->kind == TY_I32 ? -2147483648
                                       : INT64_MIN;
      if (sv < slo || (uint64_t)sv > hi)
        return false;
    } else if (raw > hi) {
      return false;
    }
    raw = trunc_to(raw, t);
    if (t->kind >= TY_I8 && t->kind <= TY_I64) {
      int bits = t->kind == TY_I8 ? 8
                 : t->kind == TY_I16 ? 16
                 : t->kind == TY_I32 ? 32 : 64;
      if (bits < 64 && (raw >> (bits - 1)) & 1)
        raw |= UINT64_MAX << bits;
    }
    out->ty = t;
    out->kind = t->kind >= TY_U8 ? CV_UINT : CV_INT;
    out->u = raw;
    out->i = (int64_t)raw;
    return true;
  }
  }
}

bool g_set_refused;

// returns 0 ok, 2 = refusal (a diagnostic is printed)
int sets_apply(Program *p) {
  for (size_t i = 0; i < VLEN(p->sets); i++) {
    SetOverride *so = VAT(p->sets, SetOverride, i);
    Sym *s = p->entry->syms ? symtab_get(p->entry->syms, so->name) : NULL;
    if (!s || s->kind != SYM_CONST) {
      fprintf(stderr, "rho: --set %s: no root const named '%s'\n",
              so->name, so->name);
      return 2;
    }
    ConstDef *cd = s->u.konst;
    CVal v;
    if (!parse_set_value(cd, so->value, &v)) {
      fprintf(stderr,
              "rho: --set %s=%s: value refused for type %s (malformed "
              "or out of range)\n",
              so->name, so->value, type_name(cd->ty));
      return 2;
    }
    CVal *slot = arena_alloc(g_arena, sizeof(CVal), 8);
    *slot = v;
    cd->cval = slot;
    cd->overridden = true;
  }
  return 0;
}

// ============================================================ T1.6 driver

void const_resolve_all(Program *p) {
  // entry first (root build params), then the rest in load order
  resolve_module_consts(p->entry, true);
  for (Module *m = p->modules; m; m = m->next)
    if (m != p->entry && m->syms)
      resolve_module_consts(m, true);
}

// try folding an if condition; on success mark the node and report
// which branch is live (1 then / 0 else, -1 none)
bool fold_if_condition(Module *m, Node *ifnode, int *live) {
  bool b;
  if (!fold_cond_modules(m, ifnode->a, &b)) {
    return false;
  }
  *live = b ? 1 : 0;
  return true;
}

// mark dead uses inside comptime-dead branches of a module (walks fn
// bodies; used at graph-load time so the module is never loaded)
void prune_dead_uses(Module *m);

static void prune_walk(Module *m, NodeRef br);
static void prune_walk_stmt(Module *m, Node *s);

static void prune_walk_stmt(Module *m, Node *s) {
  if (s->kind == NT_IF) {
    int live;
    if (fold_if_condition(m, s, &live)) {
      s->op = 1; // folded marker
      s->ival = live;
      if (live)
        prune_walk(m, s->b);
      else if (s->c != NO_REF)
        prune_walk(m, s->c);
      return; // dead side skipped whole
    }
    prune_walk(m, s->b);
    if (s->c != NO_REF) {
      if (node_get(s->c)->kind == NT_IF)
        prune_walk_stmt(m, node_get(s->c));
      else
        prune_walk(m, s->c);
    }
    return;
  }
  if (s->kind == NT_WHILE || s->kind == NT_LOOP)
    prune_walk(m, s->b);
}

static void prune_walk(Module *m, NodeRef br) {
  Node *b = node_get(br);
  if (b->kind != NT_EXPRSTMT || b->op != 3)
    return;
  for (size_t i = 0; i < reflist_len(b->list); i++)
    prune_walk_stmt(m, node_get(reflist_at(b->list, i)));
}

void prune_dead_uses(Module *m) {
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    Node *d = node_get(reflist_at(m->decls, i));
    if (d->kind != NT_FN || d->d == NO_REF)
      continue;
    prune_walk(m, d->d);
  }
}

// iterate every use declaration that survives comptime folding
static void live_uses_stmt(Module *m, Node *s, bool (*cb)(Module *, NodeRef));
static void live_uses_walk(Module *m, NodeRef br, bool (*cb)(Module *, NodeRef)) {
  Node *b = node_get(br);
  if (b->kind != NT_EXPRSTMT || b->op != 3)
    return;
  for (size_t i = 0; i < reflist_len(b->list); i++)
    live_uses_stmt(m, node_get(reflist_at(b->list, i)), cb);
}

static void live_uses_stmt(Module *m, Node *s, bool (*cb)(Module *, NodeRef)) {
  if (s->kind == NT_IF) {
    if (s->op == 1) { // folded: only the live branch exists
      if (s->ival)
        live_uses_walk(m, s->b, cb);
      else if (s->c != NO_REF)
        live_uses_walk(m, s->c, cb);
      return;
    }
    live_uses_walk(m, s->b, cb);
    if (s->c != NO_REF) {
      if (node_get(s->c)->kind == NT_IF)
        live_uses_stmt(m, node_get(s->c), cb);
      else
        live_uses_walk(m, s->c, cb);
    }
    return;
  }
  if (s->kind == NT_WHILE || s->kind == NT_LOOP)
    live_uses_walk(m, s->b, cb);
}

void for_each_live_use(Module *m, bool (*cb)(Module *, NodeRef)) {
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    NodeRef dr = reflist_at(m->decls, i);
    Node *d = node_get(dr);
    if (d->kind == NT_USE) {
      if (d->op == USE_PLAIN)
        cb(m, dr);
      continue;
    }
    if (d->kind == NT_FN && d->d != NO_REF)
      live_uses_walk(m, d->d, cb);
  }
}

void const_resolve_module_pub(Module *m) { resolve_module_consts(m, false); }
