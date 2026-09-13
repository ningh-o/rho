#include "rho.h"

// ============================================================ type model ===

typedef struct RecType RecType;
typedef struct Type Type;
typedef struct Sym Sym;
typedef struct Module Module;
typedef struct Scope Scope;

static Map modules_by_path;
Vec g_module_order;
static Map interned_types;
static Map recs_by_key;
static Map interned_types;
static Map recs_by_key;

static Scope *cur_scope;
static Type *cur_ret;
static bool in_defer;
static int loop_depth;

static Type *ty_err_;
static Type *ty_void_;
static Sym *cur_fn;

#define ERR(e, ...) err_at((e)->file, (e)->line, (e)->col, __VA_ARGS__)

typedef struct Scope {
  Map syms;
  Scope *parent;
} Scope;

static Module *cur_module;
static Module *prelude_module;
static CV const_eval(Expr *e, Module *m);

static const char *ty_name(Type *t) { return t->mangled; }

static Type *unify2(Type *a, Type *b, Expr *at);
static Type *check_field_access(Expr *e, Type *expected);
static Type *check_call(Expr *e, Type *expected);
static Type *check_new(Expr *e);
static Type *check_match(Expr *e, Type *expected);
void check_stmt(Stmt *s);

bool ty_is_int(Type *t) {
  switch (t->kind) {
  case TY_I8: case TY_I16: case TY_I32: case TY_I64:
  case TY_U8: case TY_U16: case TY_U32: case TY_U64:
  case TY_USIZE: case TY_ISIZE: case TY_INT_LIT:
    return true;
  default:
    return false;
  }
}

static bool ty_is_float(Type *t) { return t->kind == TY_F32 || t->kind == TY_F64 || t->kind == TY_FLOAT_LIT; }

bool ty_is_signed(Type *t) {
  switch (t->kind) {
  case TY_I8: case TY_I16: case TY_I32: case TY_I64: case TY_ISIZE: return true;
  default: return false;
  }
}

bool ty_is_managed(Type *t) {
  switch (t->kind) {
  case TY_PTR: case TY_WEAK: case TY_SLICE: case TY_STRING: return true;
  case TY_ARRAY: return ty_is_managed(t->elem);
  case TY_STRUCT:
    for (size_t i = 0; i < t->rec->field_types.n; i++)
      if (ty_is_managed(t->rec->field_types.items[i]))
        return true;
    return false;
  case TY_ENUM: {
    Decl *d = t->rec->decl;
    for (size_t i = 0; i < d->variants.n; i++) {
      VariantAst *v = d->variants.items[i];
      if (v->vkind != VAR_UNIT)
        return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ty_eq(Type *a, Type *b) { return a == b; }

// ------------------------------------------------------------ intern types --

static Type *ty_newk(TypeKind k) {
  Type *t = arena_alloc_zeroed(sizeof(Type));
  t->kind = k;
  return t;
}

static Type *ty_intern2(Str k, Type *t) {
  Type *found = map_get(&interned_types, k);
  if (found)
    return found;
  t->mangled = str_to_c(k);
  map_put(&interned_types, k, t);
  return t;
}

static Type *ty_intern(SB key, Type *t) { return ty_intern2(sb_finish(&key), t); }

static Type *ty_prim(int prim) {
  static Type *cache[PRIM_COUNT];
  if (!cache[prim]) {
    TypeKind k[] = {TY_BOOL, TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16,
                    TY_U32, TY_U64, TY_F32, TY_F64, TY_STRING, TY_USIZE,
                    TY_ISIZE, TY_VOID};
    SB sb = {0};
    sb_append_c(&sb, PRIM_NAMES[prim]);
    Type *t = ty_newk(k[prim]);
    cache[prim] = ty_intern(sb, t);
  }
  return cache[prim];
}

static Type *ty_ptr(Type *elem) {
  SB sb = {0};
  sb_printf(&sb, "*%s", elem->mangled);
  Type *t = ty_newk(TY_PTR);
  t->elem = elem;
  return ty_intern(sb, t);
}

static Type *ty_weak(Type *elem) {
  SB sb = {0};
  sb_printf(&sb, "weak[%s]", elem->mangled);
  Type *t = ty_newk(TY_WEAK);
  t->elem = elem;
  return ty_intern(sb, t);
}

static Type *ty_slice(Type *elem) {
  SB sb = {0};
  sb_printf(&sb, "[]%s", elem->mangled);
  Type *t = ty_newk(TY_SLICE);
  t->elem = elem;
  return ty_intern(sb, t);
}

static Type *ty_array(Type *elem, uint64_t len) {
  SB sb = {0};
  sb_printf(&sb, "[%llu]%s", (unsigned long long)len, elem->mangled);
  Type *t = ty_newk(TY_ARRAY);
  t->elem = elem;
  t->len = len;
  return ty_intern(sb, t);
}

static Type *ty_fn(Vec params, Type *ret) {
  SB sb = {0};
  sb_append_c(&sb, "fn(");
  for (size_t i = 0; i < params.n; i++)
    sb_printf(&sb, "%s%s", i ? ";" : "", ((Type *)params.items[i])->mangled);
  sb_printf(&sb, ")->%s", ret->mangled);
  Type *t = ty_newk(TY_FN);
  t->params = params;
  t->ret = ret;
  return ty_intern(sb, t);
}

// ------------------------------------------------------------ scopes ---

static Scope *scope_push(void) {
  Scope *s = arena_alloc_zeroed(sizeof(Scope));
  s->parent = cur_scope;
  cur_scope = s;
  return s;
}

static void scope_pop(void) { cur_scope = cur_scope->parent; }

static Sym *scope_lookup(Str name) {
  for (Scope *s = cur_scope; s; s = s->parent) {
    Sym *sym = map_get(&s->syms, name);
    if (sym)
      return sym;
  }
  return NULL;
}

static Sym *lookup(Str name) {
  Sym *sym = scope_lookup(name);
  if (sym)
    return sym;
  sym = map_get(&cur_module->syms, name);
  if (sym)
    return sym;
  if (prelude_module && prelude_module != cur_module)
    return map_get(&prelude_module->syms, name);
  return NULL;
}

static void scope_decl(Str name, Sym *sym) {
  if (cur_scope && map_has(&cur_scope->syms, name)) {
    err_at(cur_fn ? cur_fn->decl->file : cur_module->path, 0, 0, "duplicate name `%s`",
           str_to_c(name));
    return;
  }
  if (cur_scope)
    map_put(&cur_scope->syms, name, sym);
}

// ============================================================ module loading =

static Type *resolve_type_in_module(Module *m, TypeAst *ta);
static Type *check_expr(Expr *e, Type *expected);
static Type *struct_field_type(RecType *rec, size_t i);
static void check_fn_body(Decl *d, Sym *sym);

static Str dir_of(Str path) {
  size_t cut = 0;
  for (size_t i = 0; i < path.n; i++)
    if (path.p[i] == '/')
      cut = i + 1;
  return str_slice(path, 0, cut);
}

static Str read_file_or_exit(Str path) {
  FILE *f = fopen(str_to_c(path), "rb");
  if (!f) {
    fprintf(stderr, "rho: cannot open %.*s\n", (int)path.n, path.p);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc((size_t)n + 1);
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "rho: short read on %.*s\n", (int)path.n, path.p);
    exit(1);
  }
  buf[n] = 0;
  fclose(f);
  return str_from_len(buf, (size_t)n);
}

static Module *load_module(Str path, Str ns, bool is_prelude) {
  Module *existing = map_get(&modules_by_path, path);
  if (existing)
    return existing;
  Str src = read_file_or_exit(path);
  Decl *root = parse_file(path, src);
  Module *m = arena_alloc_zeroed(sizeof(Module));
  m->path = path;
  m->ns = ns;
  m->root = root;
  m->is_prelude = is_prelude;
  map_put(&modules_by_path, path, m);
  vec_push(&g_module_order, m);
  cur_module = m;

  for (size_t i = 0; i < root->decls.n; i++) {
    Decl *d = root->decls.items[i];
    if (d->kind == DK_USE) {
      SB p = {0};
      sb_append(&p, dir_of(m->path));
      for (size_t j = 0; j < d->path.n; j++) {
        if (j)
          sb_append_c(&p, "/");
        sb_append_c(&p, d->path.items[j]);
      }
      sb_append_c(&p, ".rho");
      Str ns_name = str_from(d->path.items[d->path.n - 1]);
      Module *target = load_module(sb_finish(&p), ns_name, false);
      Sym *sym = arena_alloc_zeroed(sizeof(Sym));
      sym->kind = SY_MODULE;
      sym->name = ns_name;
      sym->module = target;
      sym->owner = m;
      if (map_has(&m->syms, ns_name))
        err_at(d->file, d->line, d->col, "duplicate name `%s`", str_to_c(ns_name));
      else
        map_put(&m->syms, ns_name, sym);
      continue;
    }
    Sym *sym = arena_alloc_zeroed(sizeof(Sym));
    switch (d->kind) {
    case DK_FN: sym->kind = SY_FN; break;
    case DK_EXTERN: sym->kind = SY_EXTERN; break;
    case DK_STRUCT: sym->kind = SY_STRUCT; break;
    case DK_ENUM: sym->kind = SY_ENUM; break;
    case DK_STATIC: sym->kind = SY_STATIC; break;
    case DK_CONST: sym->kind = SY_CONST; break;
    default: continue;
    }
    sym->name = d->name;
    sym->decl = d;
    sym->owner = m;
    sym->mutable = d->kind == DK_STATIC && d->is_mut;
    if (d->kind == DK_FN && d->is_method) {
      // methods register under their `Type.name` key so phase 2 resolves them
      Str key = str_from(arena_printf("%.*s.%.*s", (int)d->recv.n, d->recv.p, (int)d->name.n,
                                      d->name.p));
      if (map_has(&m->syms, key))
        err_at(d->file, d->line, d->col, "duplicate method `%s`", str_to_c(key));
      else
        map_put(&m->syms, key, sym);
      continue;
    }
    if (map_has(&m->syms, d->name)) {
      err_at(d->file, d->line, d->col, "duplicate `%s` in module", str_to_c(d->name));
      continue;
    }
    map_put(&m->syms, d->name, sym);
  }
  return m;
}

static bool prelude_loaded = false;

void check_reset(void) {
  ty_void_ = NULL; // forces type-singleton re-init below
  modules_by_path = (Map){0};
  g_module_order = (Vec){0};
  interned_types = (Map){0};
  recs_by_key = (Map){0};
  cur_module = NULL;
  prelude_module = NULL;
  cur_scope = NULL;
  prelude_loaded = false;
}

void prelude_init(void) {
  if (prelude_loaded)
    return;
  prelude_loaded = true;
  extern const char PRELUDE_SOURCE[];
  Str path = str_from("<prelude>");
  Str src = str_from(PRELUDE_SOURCE);
  Decl *root = parse_file(path, src);
  Module *m = arena_alloc_zeroed(sizeof(Module));
  m->path = path;
  m->ns = str_from("");
  m->root = root;
  m->is_prelude = true;
  map_put(&modules_by_path, path, m);
  vec_push(&g_module_order, m);
  cur_module = m;
  for (size_t i = 0; i < root->decls.n; i++) {
    Decl *d = root->decls.items[i];
    if (d->kind == DK_USE)
      continue;
    Sym *sym = arena_alloc_zeroed(sizeof(Sym));
    switch (d->kind) {
    case DK_FN: sym->kind = SY_FN; break;
    case DK_EXTERN: sym->kind = SY_EXTERN; break;
    case DK_STRUCT: sym->kind = SY_STRUCT; break;
    case DK_ENUM: sym->kind = SY_ENUM; break;
    case DK_STATIC: sym->kind = SY_STATIC; break;
    case DK_CONST: sym->kind = SY_CONST; break;
    default: continue;
    }
    sym->name = d->name;
    sym->decl = d;
    sym->owner = m;
    sym->mutable = d->kind == DK_STATIC && d->is_mut;
    if (d->kind == DK_FN && d->is_method) {
      Str key = str_from(arena_printf("%.*s.%.*s", (int)d->recv.n, d->recv.p, (int)d->name.n,
                                      d->name.p));
      if (!map_has(&m->syms, key))
        map_put(&m->syms, key, sym);
      continue;
    }
    if (!map_has(&m->syms, d->name))
      map_put(&m->syms, d->name, sym);
  }
  prelude_module = m;
}

// ============================================================ type resolution

static RecType *rec_intern(Decl *decl, Module *owner, Str mangled) {
  RecType *found = map_get(&recs_by_key, mangled);
  if (found)
    return found;
  RecType *r = arena_alloc_zeroed(sizeof(RecType));
  r->decl = decl;
  r->owner = owner;
  r->mangled = mangled;
  map_put(&recs_by_key, mangled, r);
  return r;
}

static Type *named_type(Sym *sym, Module *owner, TypeAst *ta) {
  if (ta && ta->targs.n > 0) {
    err_at(ta->file, ta->line, ta->col, "generic types arrive in 0.0.5");
    return ty_err_;
  }
  Decl *d = sym->decl;
  char *mangled = arena_printf("%.*s.%.*s", (int)owner->path.n, owner->path.p, (int)d->name.n,
                               d->name.p);
  RecType *rec = rec_intern(d, owner, str_from(mangled));
  Type *t = ty_newk(sym->kind == SY_STRUCT ? TY_STRUCT : TY_ENUM);
  t->rec = rec;
  return ty_intern2(str_from(mangled), t);
}

static Type *struct_field_type(RecType *rec, size_t i) {
  if (!rec->fields_done) {
    if (rec->resolving)
      return ty_err_; // self-containing struct; layout phase reports it
    rec->resolving = true;
    Decl *d = rec->decl;
    Module *saved = cur_module;
    cur_module = rec->owner;
    for (size_t j = 0; j < d->fields.n; j++) {
      FieldAst *fa = d->fields.items[j];
      vec_push(&rec->field_types, resolve_type_in_module(rec->owner, fa->ty));
    }
    rec->resolving = false;
    rec->fields_done = true;
    cur_module = saved;
    (void)d;
  }
  return rec->field_types.n > i ? rec->field_types.items[i] : ty_err_;
}

static Type *resolve_type_in_module(Module *m, TypeAst *ta) {
  if (!ta)
    return ty_void_;
  Module *saved = cur_module;
  cur_module = m;
  Type *result = ty_err_;
  switch (ta->kind) {
  case TA_PRIM:
    result = ty_prim(ta->prim);
    break;
  case TA_PTR:
    result = ty_ptr(resolve_type_in_module(m, ta->elem));
    break;
  case TA_WEAK:
    result = ty_weak(resolve_type_in_module(m, ta->elem));
    break;
  case TA_ARRAY: {
    Type *elem = resolve_type_in_module(m, ta->elem);
    if (!ta->size) {
      result = ty_slice(elem);
    } else {
      CV v = const_eval(ta->size, m);
      if (!v.ok || !v.is_int) {
        err_at(ta->size->file, ta->size->line, ta->size->col,
               "array size must be a compile-time integer");
        result = ty_slice(elem);
      } else {
        result = ty_array(elem, v.i);
      }
    }
    break;
  }
  case TA_NAMED: {
    Str last = str_from(ta->path.items[ta->path.n - 1]);
    if (ta->path.n == 1) {
      Sym *sym = map_get(&m->syms, last);
      if (!sym || (sym->kind != SY_STRUCT && sym->kind != SY_ENUM)) {
        if (prelude_module && prelude_module != m)
          sym = map_get(&prelude_module->syms, last);
        if (!sym || (sym->kind != SY_STRUCT && sym->kind != SY_ENUM)) {
          err_at(ta->file, ta->line, ta->col, "unknown type `%s`", str_to_c(last));
          break;
        }
        result = named_type(sym, prelude_module, ta);
        break;
      }
      result = named_type(sym, m, ta);
      break;
    }
    if (ta->path.n > 2) {
      err_at(ta->file, ta->line, ta->col, "type paths are `module.Type` at most");
      break;
    }
    Str ns = str_from(ta->path.items[0]);
    Sym *mod = map_get(&m->syms, ns);
    if (!mod || mod->kind != SY_MODULE) {
      err_at(ta->file, ta->line, ta->col, "unknown module `%s`", str_to_c(ns));
      break;
    }
    Sym *sym = map_get(&((Module *)mod->module)->syms, last);
    if (!sym || (sym->kind != SY_STRUCT && sym->kind != SY_ENUM)) {
      err_at(ta->file, ta->line, ta->col, "unknown type `%s` in module `%s`", str_to_c(last),
             str_to_c(ns));
      break;
    }
    result = named_type(sym, mod->module, ta);
    break;
  }
  case TA_FN: {
    Vec ps = {0};
    for (size_t i = 0; i < ta->params.n; i++)
      vec_push(&ps, resolve_type_in_module(m, ta->params.items[i]));
    Type *ret = ta->ret ? resolve_type_in_module(m, ta->ret) : ty_void_;
    result = ty_fn(ps, ret);
    break;
  }
  case TA_INFER:
    result = ty_err_;
    break;
  }
  cur_module = saved;
  return result;
}

// ============================================================ const-eval ====

CV const_eval(Expr *e, Module *m) {
  CV r = {0};
  Module *saved = cur_module;
  cur_module = m;
  switch (e->kind) {
  case EX_INT:
    r.ok = r.is_int = true;
    r.i = e->iv;
    break;
  case EX_FLOAT:
    r.ok = true;
    r.f = e->fv;
    break;
  case EX_STR:
    r.ok = true;
    r.s = e->sv;
    break;
  case EX_BOOL:
    r.ok = true;
    r.b = e->bv;
    break;
  case EX_NAME: {
    Sym *sym = map_get(&m->syms, e->sv);
    if (!sym && prelude_module && prelude_module != m)
      sym = map_get(&prelude_module->syms, e->sv);
    if (sym && (sym->kind == SY_CONST || sym->kind == SY_STATIC)) {
      if (sym->decl->ceval_cache_ok) {
        r = *(CV *)sym->decl->ceval_cache;
        break;
      }
      CV v = const_eval(sym->decl->init, m);
      CV *memo = arena_alloc(sizeof(CV));
      *memo = v;
      sym->decl->ceval_cache = memo;
      sym->decl->ceval_cache_ok = v.ok;
      r = v;
      break;
    }
    break;
  }
  case EX_UN: {
    CV a = const_eval(e->a, m);
    if (!a.ok)
      break;
    r.ok = true;
    if (e->unop == P_MINUS) {
      if (a.is_int) {
        r.is_int = true;
        r.i = (uint64_t)(-(int64_t)a.i);
      } else
        r.f = -a.f;
    } else if (e->unop == P_TILDE && a.is_int) {
      r.is_int = true;
      r.i = ~a.i;
    } else if (e->unop == P_BANG) {
      r.b = !a.b;
    } else {
      r.ok = false;
    }
    break;
  }
  case EX_BIN: {
    CV a = const_eval(e->a, m), b = const_eval(e->b, m);
    if (!a.ok || !b.ok)
      break;
    if (!a.is_int || !b.is_int) {
      break; // no float/str const arithmetic in v0.0.1
    }
    r.ok = r.is_int = true;
    uint64_t x = a.i, y = b.i;
    switch (e->binop) {
    case P_PLUS: r.i = x + y; break;
    case P_MINUS: r.i = x - y; break;
    case P_STAR: r.i = x * y; break;
    case P_SLASH: r.i = y ? x / y : 0; break;
    case P_PERCENT: r.i = y ? x % y : 0; break;
    case P_AMP: r.i = x & y; break;
    case P_PIPE: r.i = x | y; break;
    case P_CARET: r.i = x ^ y; break;
    case P_SHL: r.i = x << (y & 63); break;
    case P_SHR: r.i = x >> (y & 63); break;
    case P_EQ: r.is_int = false; r.b = x == y; break;
    case P_NE: r.is_int = false; r.b = x != y; break;
    case P_LT: r.is_int = false; r.b = x < y; break;
    case P_GT: r.is_int = false; r.b = x > y; break;
    case P_LE: r.is_int = false; r.b = x <= y; break;
    case P_GE: r.is_int = false; r.b = x >= y; break;
    case P_ANDAND: r.is_int = false; r.b = x && y; break;
    case P_OROR: r.is_int = false; r.b = x || y; break;
    default: r.ok = false;
    }
    break;
  }
  case EX_CAST: {
    CV a = const_eval(e->a, m);
    if (!a.ok)
      break;
    r.ok = a.is_int;
    r.is_int = a.is_int;
    r.i = a.i;
    break;
  }
  default:
    break;
  }
  cur_module = saved;
  return r;
}

// ============================================================ checking ======

static int next_local_id;

static Type *adapt_literal(Type *got, Type *expected) {
  if (!expected || expected->kind == TY_ERR)
    return got;
  if (got->kind == TY_INT_LIT && ty_is_int(expected))
    return expected;
  if (got->kind == TY_FLOAT_LIT && ty_is_float(expected))
    return expected;
  if (got->kind == TY_NULL && (expected->kind == TY_PTR || expected->kind == TY_WEAK))
    return expected;
  return got;
}

static bool types_compatible(Type *got, Type *expected) {
  return ty_eq(got, expected);
}

static void require(Type *got, Type *expected, Expr *e, const char *what) {
  got = adapt_literal(got, expected);
  e->typed = got;
  if (!types_compatible(got, expected))
    ERR(e, "%s: expected `%s`, found `%s`", what, ty_name(expected), ty_name(got));
}

// does an lvalue chain root at a mutable place? indexing a slice or writing
// through a pointer mutates heap storage — allowed regardless of how the
// reference itself was bound. Indexing an inline array mutates the binding.
static bool lvalue_mutable(Expr *e) {
  if (e->kind == EX_NAME) {
    Sym *sym = e->sym;
    if (!sym)
      return false;
    if (sym->kind == SY_LOCAL || sym->kind == SY_PARAM)
      return sym->mutable;
    if (sym->kind == SY_STATIC)
      return sym->mutable;
    return false;
  }
  if (e->kind == EX_FIELD) {
    Type *bt = e->a->typed;
    if (bt && bt->kind == TY_PTR)
      return true; // heap object
    return lvalue_mutable(e->a);
  }
  if (e->kind == EX_INDEX) {
    Type *bt = e->a->typed;
    if (bt && bt->kind == TY_SLICE)
      return true; // heap buffer shared by reference
    return lvalue_mutable(e->a);
  }
  if (e->kind == EX_UN && e->unop == P_STAR)
    return true; // through a pointer: heap objects are mutable
  return false;
}

static Type *check_block_value(Vec *stmts, Type *expected);

static Type *check_expr(Expr *e, Type *expected) {
  switch (e->kind) {
  case EX_INT: {
    if (expected && ty_is_int(expected) && expected->kind != TY_INT_LIT)
      e->typed = expected;
    else
      e->typed = ty_prim(PRIM_I32);
    return e->typed;
  }
  case EX_FLOAT: {
    if (expected && expected->kind == TY_F32)
      e->typed = expected;
    else
      e->typed = ty_prim(PRIM_F64);
    return e->typed;
  }
  case EX_STR:
    e->typed = ty_prim(PRIM_STRING);
    return e->typed;
  case EX_BOOL:
    e->typed = ty_prim(PRIM_BOOL);
    return e->typed;
  case EX_NULL: {
    if (expected && (expected->kind == TY_PTR || expected->kind == TY_WEAK))
      e->typed = expected;
    else
      e->typed = ty_newk(TY_NULL);
    return e->typed;
  }
  case EX_NAME: {
    Sym *sym = lookup(e->sv);
    if (!sym) {
      ERR(e, "unknown name `%s`", str_to_c(e->sv));
      e->typed = ty_err_;
      return e->typed;
    }
    e->sym = sym;
    e->typed = sym->kind == SY_MODULE
                   ? map_get(&interned_types, str_from("<module>"))
                   : sym->type;
    return e->typed;
  }
  case EX_TYPE:
    ERR(e, "a type is not a value");
    e->typed = ty_err_;
    return e->typed;
  case EX_UN: {
    Type *a = check_expr(e->a, NULL);
    switch (e->unop) {
    case P_BANG:
      if (a->kind != TY_BOOL)
        ERR(e, "`!` needs a bool");
      e->typed = ty_prim(PRIM_BOOL);
      break;
    case P_TILDE:
      if (!ty_is_int(a))
        ERR(e, "`~` needs an integer");
      e->typed = a;
      break;
    case P_MINUS:
      if (!ty_is_int(a) && !ty_is_float(a))
        ERR(e, "`-` needs a number");
      e->typed = a->kind == TY_INT_LIT ? ty_prim(PRIM_I32) : a;
      break;
    case P_STAR: {
      if (a->kind != TY_PTR) {
        ERR(e, "`*` dereference needs a pointer, found `%s`", ty_name(a));
        e->typed = ty_err_;
      } else {
        e->typed = a->elem;
      }
      break;
    }
    default:
      e->typed = ty_err_;
    }
    return e->typed;
  }
  case EX_CAST: {
    check_expr(e->a, NULL);
    Type *target = resolve_type_in_module(cur_module, e->ty);
    Type *src = e->a->typed;
    bool ok = (ty_is_int(target) || ty_is_float(target)) &&
              (ty_is_int(src) || ty_is_float(src) || src->kind == TY_ENUM ||
               src->kind == TY_INT_LIT || src->kind == TY_FLOAT_LIT);
    if (src->kind == TY_BOOL || target->kind == TY_BOOL)
      ok = false;
    if (!ok)
      ERR(e, "cannot cast `%s` to `%s`", ty_name(src), ty_name(target));
    e->typed = target;
    return e->typed;
  }
  case EX_BIN: {
    Tok op = e->binop;
    Type *l = check_expr(e->a, NULL);
    Type *r = NULL;
    if (op == P_ANDAND || op == P_OROR) {
      if (l->kind != TY_BOOL)
        ERR(e->a, "left of `%s` must be bool", tok_spell(op));
      r = check_expr(e->b, ty_prim(PRIM_BOOL));
      if (r->kind != TY_BOOL)
        ERR(e->b, "right of `%s` must be bool", tok_spell(op));
      e->typed = ty_prim(PRIM_BOOL);
      return e->typed;
    }
    // literal adaptation: give untyped literals the other side's type
    if (l->kind == TY_INT_LIT || l->kind == TY_FLOAT_LIT || l->kind == TY_NULL) {
      Type *r2 = check_expr(e->b, NULL);
      l = adapt_literal(l, r2);
      e->a->typed = l;
      r = r2;
    } else {
      r = adapt_literal(check_expr(e->b, l), l);
      e->b->typed = r;
    }
    bool cmp = op == P_EQ || op == P_NE || op == P_LT || op == P_GT || op == P_LE || op == P_GE;
    if (cmp) {
      bool ok = false;
      if (op == P_EQ || op == P_NE) {
        ok = (ty_is_int(l) && ty_is_int(r)) || (ty_is_float(l) && ty_is_float(r)) ||
             (l->kind == TY_BOOL && r->kind == TY_BOOL) ||
             ((l->kind == TY_PTR || l->kind == TY_NULL) && (r->kind == TY_PTR || r->kind == TY_NULL)) ||
             ((l->kind == TY_WEAK) && (r->kind == TY_WEAK || r->kind == TY_NULL)) ||
             (l->kind == TY_STRING && r->kind == TY_STRING);
        if ((l->kind == TY_STRUCT || r->kind == TY_STRUCT || l->kind == TY_ARRAY) ||
            (l->kind == TY_ENUM && l == r)) {
          // enum tag compare is only sound for payload-free enums — allow it,
          // payloadful enums get elementwise == in 0.0.5
          if (l->kind == TY_ENUM && l == r) {
            Decl *d = l->rec->decl;
            bool payloadful = false;
            for (size_t i = 0; i < d->variants.n; i++)
              if (((VariantAst *)d->variants.items[i])->vkind != VAR_UNIT)
                payloadful = true;
            ok = !payloadful;
          }
        }
      } else {
        ok = (ty_is_int(l) && ty_is_int(r)) || (ty_is_float(l) && ty_is_float(r));
      }
      if (!ok)
        ERR(e, "cannot compare `%s` and `%s`", ty_name(l), ty_name(r));
      e->typed = ty_prim(PRIM_BOOL);
      return e->typed;
    }
    if (op == P_SHL || op == P_SHR) {
      if (!ty_is_int(l))
        ERR(e, "shift needs an integer left side");
      if (!ty_is_int(r))
        ERR(e, "shift needs an integer right side");
      e->typed = l->kind == TY_INT_LIT ? ty_prim(PRIM_I32) : l;
      return e->typed;
    }
    if (!ty_eq(l, r)) {
      ERR(e, "type mismatch in `%s`: `%s` vs `%s`", tok_spell(op), ty_name(l), ty_name(r));
      e->typed = ty_err_;
      return e->typed;
    }
    if (op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH || op == P_PERCENT) {
      if (!ty_is_int(l) && !ty_is_float(l))
        ERR(e, "arithmetic needs numbers, found `%s`", ty_name(l));
    } else if (op == P_AMP || op == P_PIPE || op == P_CARET) {
      if (!ty_is_int(l))
        ERR(e, "bitwise needs integers, found `%s`", ty_name(l));
    } else {
      ERR(e, "unknown operator %s", tok_spell(op));
    }
    e->typed = l->kind == TY_INT_LIT ? ty_prim(PRIM_I32) : l;
    return e->typed;
  }
  case EX_INDEX: {
    Type *base = check_expr(e->a, NULL);
    Type *idx = check_expr(e->b, NULL);
    if (!ty_is_int(idx))
      ERR(e->b, "index must be an integer, found `%s`", ty_name(idx));
    if (base->kind == TY_ARRAY || base->kind == TY_SLICE) {
      e->typed = base->elem;
    } else if (base->kind == TY_STRING) {
      e->typed = ty_prim(PRIM_U8);
    } else {
      ERR(e, "cannot index `%s`", ty_name(base));
      e->typed = ty_err_;
    }
    return e->typed;
  }
  case EX_SLICE: {
    Type *base = check_expr(e->a, NULL);
    Type *lo = check_expr(e->b, NULL), *hi = check_expr(e->c, NULL);
    if (!ty_is_int(lo) || !ty_is_int(hi))
      ERR(e, "slice bounds must be integers");
    if (base->kind == TY_ARRAY || base->kind == TY_SLICE)
      e->typed = ty_slice(base->elem);
    else if (base->kind == TY_STRING)
      e->typed = ty_prim(PRIM_STRING);
    else {
      ERR(e, "cannot slice `%s`", ty_name(base));
      e->typed = ty_err_;
    }
    return e->typed;
  }
  case EX_FIELD:
    return check_field_access(e, expected);
  case EX_CALL:
    return check_call(e, expected);
  case EX_NEW:
    return check_new(e);
  case EX_IF: {
    Type *cond = check_expr(e->a, NULL);
    if (cond->kind != TY_BOOL)
      ERR(e->a, "if condition must be bool, found `%s`", ty_name(cond));
    Vec *then = &((Stmt *)e->items.items[0])->stmts;
    Type *t = check_block_value(then, expected);
    Type *result = ty_void_;
    if (e->items.n > 1) {
      void *els = e->items.items[1];
      Expr *else_if = els;
      if (else_if->kind == EX_IF) {
        Type *et = check_expr(else_if, expected ? expected : t);
        result = unify2(t, et, e);
      } else {
        Type *et = check_block_value(&((Stmt *)els)->stmts, expected ? expected : t);
        result = unify2(t, et, e);
      }
    } else {
      if (t && t->kind != TY_VOID)
        ERR(e, "an `if` without `else` cannot produce a value");
      result = ty_void_;
    }
    e->typed = result;
    return result;
  }
  case EX_BLOCK: {
    scope_push();
    Type *result = ty_void_;
    for (size_t i = 0; i < e->items.n; i++) {
      Stmt *s = e->items.items[i];
      bool last = i + 1 == e->items.n;
      check_stmt(s);
      if (last && s->kind == ST_EXPR && s->tail)
        result = s->a->typed;
    }
    scope_pop();
    e->typed = result;
    return result;
  }
  case EX_MATCH:
    return check_match(e, expected);
  case EX_CLOSURE:
    ERR(e, "closures arrive in 0.0.5");
    e->typed = ty_err_;
    return e->typed;
  case EX_QMARK:
    ERR(e, "`?` arrives in 0.0.5");
    e->typed = ty_err_;
    return e->typed;
  case EX_MAKE: // never produced by the parser; `make` is checked in EX_CALL
  case EX_ENUM_CTOR:
  case EX_METHOD:
    ERR(e, "internal: unexpected expression form");
    e->typed = ty_err_;
    return e->typed;
  }
  return ty_err_;
}

static Type *unify2(Type *a, Type *b, Expr *at) {
  if (!a || a->kind == TY_ERR)
    return b;
  if (!b || b->kind == TY_ERR)
    return a;
  Type *u = adapt_literal(a, b);
  Type *v = adapt_literal(b, a);
  if (u->kind == TY_VOID && v->kind == TY_VOID)
    return u;
  if (ty_eq(u, v))
    return u;
  ERR(at, "branches have different types: `%s` vs `%s`", ty_name(a), ty_name(b));
  return ty_err_;
}

static Type *check_field_access(Expr *e, Type *expected) {
  (void)expected;
  Type *base = check_expr(e->a, NULL);
  // module access: statics/consts (functions go through call resolution)
  if (base && base->kind == TY_MODULE) {
    Sym *mod_sym = e->a->sym;
    Module *target = mod_sym->module;
    Sym *item = map_get(&target->syms, e->sv);
    if (!item) {
      ERR(e, "module `%s` has no item `%s`", str_to_c(e->a->sv), str_to_c(e->sv));
      e->typed = ty_err_;
      return e->typed;
    }
    if (item->kind == SY_STATIC || item->kind == SY_CONST) {
      if (!item->decl->pub_ && !((Module *)item->owner)->is_prelude) {
        ERR(e, "`%s.%s` is not public", str_to_c(e->a->sv), str_to_c(e->sv));
      }
      e->sym = item;
      e->typed = item->type;
      return e->typed;
    }
    if (item->kind == SY_FN || item->kind == SY_EXTERN) {
      ERR(e, "function references arrive in 0.0.5");
      e->typed = ty_err_;
      return e->typed;
    }
    ERR(e, "`%s.%s` is a type; types are not values", str_to_c(e->a->sv), str_to_c(e->sv));
    e->typed = ty_err_;
    return e->typed;
  }
  if (!base) {
    e->typed = ty_err_;
    return e->typed;
  }
  // slice/string built-in fields
  if (base->kind == TY_SLICE || base->kind == TY_STRING) {
    if (str_eq_c(e->sv, "len")) {
      e->typed = ty_prim(PRIM_USIZE);
      return e->typed;
    }
    if (str_eq_c(e->sv, "ptr")) {
      e->typed = base->kind == TY_STRING ? ty_ptr(ty_prim(PRIM_U8)) : ty_ptr(base->elem);
      return e->typed;
    }
    ERR(e, "slices have `len` and `ptr`, not `%s`", str_to_c(e->sv));
    e->typed = ty_err_;
    return e->typed;
  }
  if (base->kind == TY_PTR) {
    Type *deref = base->elem;
    if (deref->kind == TY_STRUCT) {
      // auto-deref field access
      RecType *rec = deref->rec;
      Decl *d = rec->decl;
      for (size_t i = 0; i < d->fields.n; i++) {
        FieldAst *fa = d->fields.items[i];
        if (str_eq(fa->name, e->sv)) {
          e->typed = struct_field_type(rec, i);
          return e->typed;
        }
      }
      ERR(e, "no field `%s` on `%s`", str_to_c(e->sv), ty_name(deref));
      e->typed = ty_err_;
      return e->typed;
    }
    ERR(e, "`%s` has no fields (auto-deref only works on struct pointers)", ty_name(deref));
    e->typed = ty_err_;
    return e->typed;
  }
  if (base->kind == TY_STRUCT) {
    RecType *rec = base->rec;
    Decl *d = rec->decl;
    for (size_t i = 0; i < d->fields.n; i++) {
      FieldAst *fa = d->fields.items[i];
      if (str_eq(fa->name, e->sv)) {
        e->typed = struct_field_type(rec, i);
        return e->typed;
      }
    }
    ERR(e, "no field `%s` on `%s`", str_to_c(e->sv), ty_name(base));
    e->typed = ty_err_;
    return e->typed;
  }
  // enum variant: `Color.Red`
  if (e->a->kind == EX_NAME) {
    Sym *sym = e->a->sym;
    if (sym && sym->kind == SY_ENUM) {
      Decl *d = sym->decl;
      for (size_t i = 0; i < d->variants.n; i++) {
        VariantAst *v = d->variants.items[i];
        if (str_eq(v->name, e->sv)) {
          if (v->vkind != VAR_UNIT) {
            ERR(e, "variant `%s` carries a payload; call it to construct", str_to_c(e->sv));
            e->typed = ty_err_;
            return e->typed;
          }
          Sym *vs = arena_alloc_zeroed(sizeof(Sym));
          vs->kind = SY_VARIANT;
          vs->name = e->sv;
          vs->type = sym->type;
          vs->decl = d;
          vs->variant_index = (int)i;
          e->sym = vs;
          e->typed = sym->type;
          return e->typed;
        }
      }
      ERR(e, "enum `%s` has no variant `%s`", ty_name(sym->type), str_to_c(e->sv));
      e->typed = ty_err_;
      return e->typed;
    }
  }
  ERR(e, "cannot access field `%s` on `%s`", str_to_c(e->sv), ty_name(base));
  e->typed = ty_err_;
  return e->typed;
}

// A resolved call target.
typedef struct CallTarget {
  enum { CT_FN, CT_EXTERN, CT_VARIANT, CT_INDIRECT, CT_NONE } kind;
  Sym *sym;         // CT_FN/CT_EXTERN
  Type *fn_type;    // resolved signature (params include self for methods)
  int variant;      // CT_VARIANT: variant index
  Type *enum_type;  // CT_VARIANT
  bool is_method;   // callee supplies self as args[0]
  bool is_ctor;     // struct-variant with named args
} CallTarget;

static Type *deref_to_struct(Type *t) {
  while (t && t->kind == TY_PTR)
    t = t->elem;
  return t && (t->kind == TY_STRUCT) ? t : NULL;
}

static CallTarget resolve_callee(Expr *callee) {
  CallTarget ct = {0};
  ct.kind = CT_NONE;
  if (callee->kind == EX_NAME) {
    Sym *sym = lookup(callee->sv);
    if (!sym) {
      ERR(callee, "unknown function `%s`", str_to_c(callee->sv));
      return ct;
    }
    if (sym->kind == SY_FN || sym->kind == SY_EXTERN) {
      ct.kind = sym->kind == SY_FN ? CT_FN : CT_EXTERN;
      ct.sym = sym;
      ct.fn_type = sym->type;
      callee->sym = sym;
      return ct;
    }
    ERR(callee, "`%s` is not a function", str_to_c(callee->sv));
    return ct;
  }
  if (callee->kind == EX_FIELD) {
    // 1) module function: `mod.fn(...)`
    if (callee->a->kind == EX_NAME) {
      Sym *base = lookup(callee->a->sv);
      if (base && base->kind == SY_MODULE) {
        Sym *item = map_get(&((Module *)base->module)->syms, callee->sv);
        if (item && (item->kind == SY_FN || item->kind == SY_EXTERN)) {
          if (!item->decl->pub_ && !((Module *)item->owner)->is_prelude) {
            ERR(callee, "`%s.%s` is not public", str_to_c(callee->a->sv), str_to_c(callee->sv));
          }
          ct.kind = item->kind == SY_FN ? CT_FN : CT_EXTERN;
          ct.sym = item;
          ct.fn_type = item->type;
          callee->sym = item;
          return ct;
        }
        ERR(callee, "module `%s` has no function `%s`", str_to_c(callee->a->sv),
            str_to_c(callee->sv));
        return ct;
      }
      // 2) enum variant construction: `Color.RGB(...)`
      if (base && base->kind == SY_ENUM) {
        Decl *d = base->decl;
        for (size_t i = 0; i < d->variants.n; i++) {
          VariantAst *v = d->variants.items[i];
          if (str_eq(v->name, callee->sv)) {
            ct.kind = CT_VARIANT;
            ct.variant = (int)i;
            ct.enum_type = base->type;
            ct.is_ctor = v->vkind == VAR_STRUCT;
            callee->sym = base;
            return ct;
          }
        }
      }
      // 3) associated function: `Type.make(...)` — plain fn in owner module
      if (base && (base->kind == SY_STRUCT || base->kind == SY_ENUM)) {
        Sym *fn = map_get(&((Module *)base->owner)->syms, callee->sv);
        if (fn && fn->kind == SY_FN && !fn->decl->is_method) {
          ct.kind = CT_FN;
          ct.sym = fn;
          ct.fn_type = fn->type;
          callee->sym = fn;
          return ct;
        }
        ERR(callee, "type `%s` has no associated function `%s`", str_to_c(callee->a->sv),
            str_to_c(callee->sv));
        return ct;
      }
    }
    // 4) method call: `receiver.name(...)` — receiver typed by now?
    Type *bt = check_expr(callee->a, NULL);
    Type *rec_t = deref_to_struct(bt);
    if (rec_t) {
      RecType *rec = rec_t->rec;
      for (size_t i = 0; i < rec->methods.n; i++) {
        Sym *m = rec->methods.items[i];
        if (str_eq(m->name, callee->sv)) {
          ct.kind = CT_FN;
          ct.sym = m;
          ct.fn_type = m->type;
          ct.is_method = true;
          callee->sym = m;
          callee->a->typed = bt;
          return ct;
        }
      }
    }
    ERR(callee, "no method `%s` for `%s`", str_to_c(callee->sv), ty_name(bt));
    return ct;
  }
  // 5) function-value call: `f(...)` where f has a fn type
  Type *t = check_expr(callee, NULL);
  if (t->kind == TY_FN) {
    ct.kind = CT_INDIRECT;
    ct.fn_type = t;
    return ct;
  }
  ERR(callee, "cannot call `%s`", ty_name(t));
  return ct;
}

static Type *check_call(Expr *e, Type *expected) {
  (void)expected;
  // builtins: make / len
  if (e->a->kind == EX_NAME && str_eq_c(e->a->sv, "make")) {
    if (e->args.n != 2) {
      ERR(e, "make takes (type, length)");
      e->typed = ty_err_;
      return e->typed;
    }
    Expr *tye = e->args.items[0];
    if (tye->kind != EX_TYPE || tye->ty->kind != TA_ARRAY || tye->ty->size) {
      ERR(e, "make takes a slice type like `[]i32`");
      e->typed = ty_err_;
      return e->typed;
    }
    Type *elem = resolve_type_in_module(cur_module, tye->ty->elem);
    Type *len = check_expr(e->args.items[1], NULL);
    if (!ty_is_int(len))
      ERR((Expr *)e->args.items[1], "make length must be an integer");
    e->typed = ty_slice(elem);
    return e->typed;
  }
  if (e->a->kind == EX_NAME && str_eq_c(e->a->sv, "len")) {
    if (e->args.n != 1) {
      ERR(e, "len takes one argument");
      e->typed = ty_err_;
      return e->typed;
    }
    Type *t = check_expr(e->args.items[0], NULL);
    if (t->kind != TY_SLICE && t->kind != TY_STRING && t->kind != TY_ARRAY) {
      ERR(e, "len needs an array, slice or string, found `%s`", ty_name(t));
      e->typed = ty_err_;
      return e->typed;
    }
    e->typed = ty_prim(PRIM_USIZE);
    return e->typed;
  }

  CallTarget ct = resolve_callee(e->a);
  if (ct.kind == CT_NONE) {
    e->typed = ty_err_;
    return e->typed;
  }
  if (ct.kind == CT_VARIANT) {
    VariantAst *v = ((Decl *)ct.enum_type->rec->decl)->variants.items[ct.variant];
    size_t npayload = v->vkind == VAR_TUPLE ? v->types.n : v->vkind == VAR_STRUCT ? v->fields.n : 0;
    if (npayload == 0) {
      if (e->args.n != 0)
        ERR(e, "unit variant takes no arguments");
    } else if (v->vkind == VAR_TUPLE) {
      if (e->args.n != v->types.n) {
        ERR(e, "variant `%s` takes %zu arguments, got %zu", str_to_c(v->name), v->types.n,
            e->args.n);
      } else {
        for (size_t i = 0; i < e->args.n; i++) {
          Type *pt = resolve_type_in_module(cur_module, v->types.items[i]);
          require(check_expr(e->args.items[i], pt), pt, e->args.items[i], "variant payload");
        }
      }
    } else {
      // struct variant: named arguments
      for (size_t i = 0; i < e->args.n; i++) {
        char *nm = e->arg_names.n > i ? e->arg_names.items[i] : NULL;
        if (!nm) {
          ERR((Expr *)e->args.items[i], "struct variants take named arguments");
          continue;
        }
        bool found = false;
        for (size_t j = 0; j < v->fields.n; j++) {
          FieldAst *fa = v->fields.items[j];
          if (str_eq_c(fa->name, nm)) {
            Type *ft = resolve_type_in_module(cur_module, fa->ty);
            require(check_expr(e->args.items[i], ft), ft, (Expr *)e->args.items[i], "variant field");
            found = true;
            break;
          }
        }
        if (!found)
          ERR((Expr *)e->args.items[i], "variant `%s` has no field `%s`", str_to_c(v->name), nm);
      }
      if (e->args.n != v->fields.n)
        ERR(e, "variant `%s` needs %zu fields, got %zu", str_to_c(v->name), v->fields.n,
            e->args.n);
    }
    e->typed = ct.enum_type;
    return e->typed;
  }

  // regular call: check args against params
  Type *fn_t = ct.fn_type;
  size_t nparams = fn_t->params.n;
  (void)0;
  size_t first_arg = 0;
  if (ct.is_method) {
    if (e->args.n + 1 != nparams) {
      ERR(e, "method takes %zu arguments, got %zu", nparams - 1, e->args.n);
    }
    Type *self_param = fn_t->params.items[0];
    Type *recv = e->a->a->typed;
    if (!recv || recv->kind == TY_ERR) {
      recv = check_expr(e->a->a, NULL);
    }
    // accept receiver by value or pointer when self matches the other form
    bool ok = ty_eq(recv, self_param);
    if (!ok && recv && recv->kind == TY_PTR && self_param->kind != TY_PTR &&
        ty_eq(recv->elem, self_param))
      ok = true; // self: T, receiver *T: auto-deref (no auto-ref; there is no &)
    if (!ok)
      ERR(e->a, "method `%s` expects self as `%s`, receiver is `%s`",
          str_to_c(e->a->sv), ty_name(self_param), ty_name(recv));
    first_arg = 1;
  } else if (e->args.n != nparams) {
    ERR(e, "function takes %zu arguments, got %zu", nparams, e->args.n);
  }
  for (size_t i = first_arg; i < nparams; i++) {
    size_t ai = i - first_arg;
    if (ai >= e->args.n)
      break;
    Type *pt = fn_t->params.items[i];
    require(check_expr(e->args.items[ai], pt), pt, (Expr *)e->args.items[ai], "argument");
  }
  e->typed = fn_t->ret;
  return e->typed;
}

static Type *check_new(Expr *e) {
  Type *t = resolve_type_in_module(cur_module, e->ty);
  if (t->kind != TY_STRUCT) {
    ERR(e, "`new` takes a struct type");
    e->typed = ty_err_;
    return e->typed;
  }
  RecType *rec = t->rec;
  Decl *d = rec->decl;
  // force field types now
  struct_field_type(rec, d->fields.n ? d->fields.n - 1 : 0);
  if (e->items.n != d->fields.n)
    ERR(e, "`%s` has %zu fields, got %zu initializers", str_to_c(d->name), d->fields.n,
        e->items.n);
  for (size_t i = 0; i < e->items.n; i++) {
    FieldAst *fa = e->items.items[i];
    Expr *init = e->args.items[i];
    bool found = false;
    for (size_t j = 0; j < d->fields.n; j++) {
      FieldAst *df = d->fields.items[j];
      if (str_eq(df->name, fa->name)) {
        Type *ft = struct_field_type(rec, j);
        require(check_expr(init, ft), ft, init, "field initializer");
        found = true;
        break;
      }
    }
    if (!found)
      ERR(fa, "no field `%s` on `%s`", str_to_c(fa->name), str_to_c(d->name));
  }
  e->typed = ty_ptr(t);
  return e->typed;
}

static Type *check_match(Expr *e, Type *expected) {
  Type *scrut = check_expr(e->a, NULL);
  Decl *ed = NULL;
  bool int_match = ty_is_int(scrut) || scrut->kind == TY_INT_LIT;
  if (scrut->kind == TY_ENUM)
    ed = scrut->rec->decl;
  else if (!int_match) {
    ERR(e, "match scrutinee must be an enum or integer, found `%s`", ty_name(scrut));
    e->typed = ty_err_;
    return e->typed;
  }
  bool covered[256] = {0};
  bool wildcard = false;
  Type *result = NULL;
  for (size_t i = 0; i < e->arms.n; i++) {
    MatchArm *arm = e->arms.items[i];
    if (ed) {
      if (arm->pk == PAT_WILDCARD) {
        wildcard = true;
      } else if (arm->pk == PAT_UNIT) {
        Str vn = str_from(arm->pat_path.items[arm->pat_path.n - 1]);
        bool found = false;
        for (size_t j = 0; j < ed->variants.n; j++) {
          VariantAst *v = ed->variants.items[j];
          if (str_eq(v->name, vn)) {
            if (v->vkind != VAR_UNIT) {
              ERR(arm, "payload patterns arrive in 0.0.5");
              found = true; // keep going
            }
            covered[j] = true;
            found = true;
            break;
          }
        }
        if (!found)
          ERR(arm, "no variant `%s` on `%s`", str_to_c(vn), ty_name(scrut));
      } else if (arm->pk == PAT_TUPLE || arm->pk == PAT_STRUCT) {
        ERR(arm, "payload patterns arrive in 0.0.5");
      } else {
        ERR(arm, "this pattern does not match an enum");
      }
    } else {
      if (arm->pk == PAT_WILDCARD)
        wildcard = true;
      else if (arm->pk == PAT_INT)
        ; // literal match
      else
        ERR(arm, "integer match arms must be literals or `_`");
    }
    Type *bt = check_expr(arm->body, expected);
    if (!result)
      result = bt;
    else {
      Type *u = unify2(result, bt, arm->body);
      if (u->kind != TY_ERR)
        result = u;
    }
  }
  if (ed && !wildcard) {
    for (size_t j = 0; j < ed->variants.n; j++) {
      if (!covered[j]) {
        ERR(e, "match is not exhaustive: missing `%s`",
            str_to_c(((VariantAst *)ed->variants.items[j])->name));
        break;
      }
    }
  }
  if (!int_match || wildcard) {
    if (int_match && !wildcard && e->arms.n == 0)
      ERR(e, "match needs at least one arm");
  }
  e->typed = result ? result : ty_void_;
  return e->typed;
}

// ============================================================ statements ====

static bool block_returns(Vec *stmts);
static bool loop_has_break(Vec *stmts);

static Type *check_block_value(Vec *stmts, Type *expected) {
  scope_push();
  Type *result = ty_void_;
  for (size_t i = 0; i < stmts->n; i++) {
    Stmt *s = stmts->items[i];
    check_stmt(s);
    if (i + 1 == stmts->n && s->kind == ST_EXPR && s->tail)
      result = s->a->typed;
  }
  scope_pop();
  if (expected && result->kind != TY_VOID)
    result = adapt_literal(result, expected);
  return result;
}

void check_stmt(Stmt *s) {
  switch (s->kind) {
  case ST_LET: {
    Type *ann = s->ty ? resolve_type_in_module(cur_module, s->ty) : NULL;
    Type *t = check_expr(s->a, ann);
    if (!ann) {
      if (t->kind == TY_INT_LIT)
        t = ty_prim(PRIM_I32);
      else if (t->kind == TY_FLOAT_LIT)
        t = ty_prim(PRIM_F64);
      else if (t->kind == TY_NULL) {
        ERR(s, "`null` needs a type annotation");
        t = ty_err_;
      } else if (t->kind == TY_ERR) {
        t = ty_err_;
      } else if (t->kind == TY_VOID) {
        ERR(s, "cannot bind void");
        t = ty_err_;
      }
    }
    Sym *sym = arena_alloc_zeroed(sizeof(Sym));
    sym->kind = SY_LOCAL;
    sym->name = s->name;
    sym->type = t;
    sym->mutable = s->mut;
    sym->local_id = next_local_id++;
    // shadowing an outer scope name is allowed only for different scopes —
    // rho forbids shadowing entirely (spec §5)
    if (scope_lookup(s->name)) {
      err_at(s->file, s->line, s->col, "`%s` shadows an existing binding", str_to_c(s->name));
    }
    scope_decl(s->name, sym);
    s->a->typed = t;
    break;
  }
  case ST_ASSIGN: {
    Type *target = check_expr(s->a, NULL);
    bool compound = s->assign_op != P_ASSIGN;
    Type *value;
    if (compound) {
      value = check_expr(s->b, NULL);
      // target op value must be well-formed and equal target's type
      if (s->assign_op == P_SHLEQ || s->assign_op == P_SHREQ) {
        if (!ty_is_int(target) || !ty_is_int(value))
          ERR(s, "shift assignment needs integers");
      } else {
        if (!ty_eq(target, value) && !(ty_is_int(target) && ty_is_int(value)) &&
            !(ty_is_float(target) && ty_is_float(value)))
          ERR(s, "type mismatch in assignment: `%s` vs `%s`", ty_name(target), ty_name(value));
      }
    } else {
      value = adapt_literal(check_expr(s->b, target), target);
      s->b->typed = value;
      if (!ty_eq(target, value)) {
        ERR(s, "cannot assign `%s` to `%s`", ty_name(value), ty_name(target));
      }
    }
    if (!lvalue_mutable(s->a)) {
      err_at(s->file, s->line, s->col,
             "cannot assign to an immutable binding (declare it `let mut`)");
    }
    break;
  }
  case ST_EXPR:
    check_expr(s->a, s->tail ? NULL : NULL);
    break;
  case ST_RETURN: {
    if (in_defer) {
      err_at(s->file, s->line, s->col, "`return` is not allowed in defer");
      break;
    }
    if (!cur_ret || cur_ret->kind == TY_VOID) {
      if (s->a)
        ERR(s, "this function returns nothing");
    } else {
      if (!s->a) {
        ERR(s, "missing return value");
      } else {
        require(check_expr(s->a, cur_ret), cur_ret, s->a, "return value");
      }
    }
    break;
  }
  case ST_BREAK:
  case ST_CONTINUE:
    if (in_defer)
      err_at(s->file, s->line, s->col, "`%s` is not allowed in defer",
             s->kind == ST_BREAK ? "break" : "continue");
    else if (loop_depth == 0)
      err_at(s->file, s->line, s->col, "`%s` outside of a loop",
             s->kind == ST_BREAK ? "break" : "continue");
    break;
  case ST_DEFER: {
    bool saved = in_defer;
    in_defer = true;
    scope_push();
    for (size_t i = 0; i < s->stmts.n; i++)
      check_stmt(s->stmts.items[i]);
    scope_pop();
    in_defer = saved;
    break;
  }
  case ST_WHILE: {
    Type *cond = check_expr(s->cond, NULL);
    if (cond->kind != TY_BOOL)
      ERR(s->cond, "while condition must be bool");
    scope_push();
    loop_depth++;
    for (size_t i = 0; i < s->body.n; i++)
      check_stmt(s->body.items[i]);
    loop_depth--;
    scope_pop();
    break;
  }
  case ST_LOOP: {
    scope_push();
    loop_depth++;
    for (size_t i = 0; i < s->body.n; i++)
      check_stmt(s->body.items[i]);
    loop_depth--;
    scope_pop();
    break;
  }
  case ST_BLOCK: {
    scope_push();
    for (size_t i = 0; i < s->stmts.n; i++)
      check_stmt(s->stmts.items[i]);
    scope_pop();
    break;
  }
  }
}

static bool if_expr_returns(Expr *ifx) {
  Vec *then = &((Stmt *)ifx->items.items[0])->stmts;
  if (!block_returns(then))
    return false;
  if (ifx->items.n < 2)
    return false;
  void *els = ifx->items.items[1];
  Expr *else_if = els;
  if (else_if->kind == EX_IF)
    return if_expr_returns(else_if);
  return block_returns(&((Stmt *)els)->stmts);
}

static bool stmt_returns(Stmt *s) {
  if (s->kind == ST_RETURN)
    return true;
  if (s->kind == ST_EXPR && s->a->kind == EX_IF)
    return if_expr_returns(s->a);
  if (s->kind == ST_EXPR && s->a->kind == EX_MATCH) {
    if (s->a->arms.n == 0)
      return false;
    for (size_t i = 0; i < s->a->arms.n; i++) {
      MatchArm *arm = s->a->arms.items[i];
      if (arm->body->kind != EX_BLOCK)
        continue; // expression arms produce values, they do not return
      bool any = false;
      for (size_t j = 0; j < arm->body->items.n; j++)
        if (stmt_returns((Stmt *)arm->body->items.items[j]))
          any = true;
      if (!any)
        return false;
    }
    return true;
  }
  if (s->kind == ST_LOOP)
    return !loop_has_break(&s->body);
  if (s->kind == ST_BLOCK)
    return block_returns(&s->stmts);
  return false;
}

static bool block_returns(Vec *stmts) {
  if (stmts->n == 0)
    return false;
  return stmt_returns(stmts->items[stmts->n - 1]);
}

static bool if_arms_have_break(Expr *ifx) {
  Stmt *then = ifx->items.items[0];
  if (then->stmts.n && loop_has_break(&then->stmts))
    return true;
  if (ifx->items.n > 1) {
    Expr *els = ifx->items.items[1];
    if (els->kind == EX_IF)
      return if_arms_have_break(els);
    Stmt *blk = ifx->items.items[1];
    if (blk->stmts.n && loop_has_break(&blk->stmts))
      return true;
  }
  return false;
}

static bool loop_has_break(Vec *stmts) {
  for (size_t i = 0; i < stmts->n; i++) {
    Stmt *s = stmts->items[i];
    if (s->kind == ST_BREAK)
      return true;
    if (s->kind == ST_WHILE || s->kind == ST_LOOP)
      continue; // an inner loop's break belongs to the inner loop
    if (s->stmts.n && loop_has_break(&s->stmts))
      return true;
    if (s->kind == ST_EXPR) {
      if (s->a->kind == EX_IF && if_arms_have_break(s->a))
        return true;
      if (s->a->kind == EX_MATCH) {
        for (size_t j = 0; j < s->a->arms.n; j++) {
          MatchArm *arm = s->a->arms.items[j];
          if (arm->body->kind == EX_BLOCK && arm->body->items.n &&
              loop_has_break(&arm->body->items))
            return true;
        }
      }
    }
  }
  return false;
}

// ============================================================ fn bodies =====

static void check_fn_body(Decl *d, Sym *sym) {
  cur_fn = sym;
  cur_ret = sym->type->ret;
  next_local_id = 0;
  loop_depth = 0;
  in_defer = false;
  scope_push();
  // params live in the function's top scope
  for (size_t i = 0; i < d->params.n; i++) {
    Param *pa = d->params.items[i];
    Sym *ps = arena_alloc_zeroed(sizeof(Sym));
    ps->kind = SY_PARAM;
    ps->name = pa->name;
    ps->type = sym->type->params.n > i ? sym->type->params.items[i] : ty_err_;
    ps->mutable = true; // parameters are mutable local copies
    ps->local_id = next_local_id++;
    if (map_has(&cur_scope->syms, pa->name))
      err_at(pa->file, pa->line, pa->col, "duplicate parameter `%s`", str_to_c(pa->name));
    else
      map_put(&cur_scope->syms, pa->name, ps);
  }
  Module *saved = cur_module;
  cur_module = sym->owner;
  for (size_t i = 0; i < d->body.n; i++)
    check_stmt(d->body.items[i]);
  if (cur_ret && cur_ret->kind != TY_VOID && !block_returns(&d->body)) {
    err_at(d->file, d->line, d->col, "missing return: `%s` is `%s`", str_to_c(d->name),
           ty_name(cur_ret));
  }
  cur_module = saved;
  scope_pop();
  cur_fn = NULL;
}

// ============================================================ phase drivers =

static void resolve_sym_type(Sym *sym) {
  if (sym->type)
    return;
  Decl *d = sym->decl;
  Module *saved = cur_module;
  cur_module = sym->owner;
  switch (sym->kind) {
  case SY_FN:
  case SY_EXTERN: {
    Vec ps = {0};
    for (size_t i = 0; i < d->params.n; i++) {
      Param *pa = d->params.items[i];
      Type *pt = resolve_type_in_module(sym->owner, pa->ty);
      vec_push(&ps, pt);
      if (pa->is_self) {
        // attach the method to its struct/enum
        Type *rec_t = pt->kind == TY_PTR ? pt->elem : pt;
        if (rec_t->kind != TY_STRUCT && rec_t->kind != TY_ENUM) {
          err_at(d->file, d->line, d->col, "`self` must be a struct or enum");
        } else {
          vec_push(&rec_t->rec->methods, sym);
        }
      }
    }
    Type *ret = d->ret ? resolve_type_in_module(sym->owner, d->ret) : ty_void_;
    sym->type = ty_fn(ps, ret);
    break;
  }
  case SY_STRUCT:
  case SY_ENUM:
    sym->type = named_type(sym, sym->owner, NULL);
    break;
  case SY_STATIC:
  case SY_CONST: {
    Type *t = resolve_type_in_module(sym->owner, d->ret);
    CV v = const_eval(d->init, sym->owner);
    if (!v.ok) {
      err_at(d->file, d->line, d->col,
             "`%s` initializer must be a compile-time constant", str_to_c(d->name));
    }
    sym->type = t;
    break;
  }
  default:
    break;
  }
  cur_module = saved;
}

int check_module(Decl *module) {
  int before = diag_count();
  if (!ty_void_) {
    ty_void_ = ty_prim(PRIM_VOID);
    ty_err_ = ty_newk(TY_ERR);
    ty_err_->mangled = "<error>";
    Type *mod_t = ty_newk(TY_MODULE);
    mod_t->mangled = "<module>";
    map_put(&interned_types, str_from("<module>"), mod_t);
  }
  prelude_init();

  // register the root module if not already loaded (it never is)
  Str path = module->file;
  if (!map_get(&modules_by_path, path)) {
    Module *m = arena_alloc_zeroed(sizeof(Module));
    m->path = path;
    m->ns = str_from("");
    m->root = module;
    map_put(&modules_by_path, path, m);
    vec_push(&g_module_order, m);
    cur_module = m;
    // register its decls (same loop as load_module, minus file reading)
    for (size_t i = 0; i < module->decls.n; i++) {
      Decl *d = module->decls.items[i];
      if (d->kind == DK_USE) {
        SB p = {0};
        sb_append(&p, dir_of(m->path));
        for (size_t j = 0; j < d->path.n; j++) {
          if (j)
            sb_append_c(&p, "/");
          sb_append_c(&p, d->path.items[j]);
        }
        sb_append_c(&p, ".rho");
        Str ns_name = str_from(d->path.items[d->path.n - 1]);
        Module *target = load_module(sb_finish(&p), ns_name, false);
        Sym *sym = arena_alloc_zeroed(sizeof(Sym));
        sym->kind = SY_MODULE;
        sym->name = ns_name;
        sym->module = target;
        sym->owner = m;
        if (map_has(&m->syms, ns_name))
          err_at(d->file, d->line, d->col, "duplicate name `%s`", str_to_c(ns_name));
        else
          map_put(&m->syms, ns_name, sym);
        continue;
      }
      Sym *sym = arena_alloc_zeroed(sizeof(Sym));
      switch (d->kind) {
      case DK_FN: sym->kind = SY_FN; break;
      case DK_EXTERN: sym->kind = SY_EXTERN; break;
      case DK_STRUCT: sym->kind = SY_STRUCT; break;
      case DK_ENUM: sym->kind = SY_ENUM; break;
      case DK_STATIC: sym->kind = SY_STATIC; break;
      case DK_CONST: sym->kind = SY_CONST; break;
      default: continue;
      }
      sym->name = d->name;
      sym->decl = d;
      sym->owner = m;
      sym->mutable = d->kind == DK_STATIC && d->is_mut;
      if (d->kind == DK_FN && d->is_method) {
        Str key = str_from(arena_printf("%.*s.%.*s", (int)d->recv.n, d->recv.p, (int)d->name.n,
                                        d->name.p));
        if (map_has(&m->syms, key))
          err_at(d->file, d->line, d->col, "duplicate method `%s`", str_to_c(key));
        else
          map_put(&m->syms, key, sym);
        continue;
      }
      if (map_has(&m->syms, d->name))
        err_at(d->file, d->line, d->col, "duplicate `%s` in module", str_to_c(d->name));
      else
        map_put(&m->syms, d->name, sym);
    }
  }

  // phase 2: resolve all top-level types (modules now all known)
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    cur_module = m;
    for (size_t k = 0; k < m->syms.keys.n; k++) {
      Str *key = m->syms.keys.items[k];
      resolve_sym_type(map_get(&m->syms, *key));
    }
  }

  // phase 3: fn bodies
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    if (m->checked)
      continue;
    m->checked = true;
    for (size_t k = 0; k < m->syms.keys.n; k++) {
      Str *key = m->syms.keys.items[k];
      Sym *sym = map_get(&m->syms, *key);
      if ((sym->kind == SY_FN) && sym->decl && sym->decl->body.n)
        check_fn_body(sym->decl, sym);
    }
  }
  cur_module = NULL;
  return diag_count() - before;
}

// ============================================================ shared =======

Module *g_prelude_module(void) { return prelude_module; }

static const char *sanitize(const char *s) {
  SB sb = {0};
  for (const char *p = s; *p; p++) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_';
    sb_push(&sb, ok ? c : '_');
  }
  sb_push(&sb, 0);
  return sb.buf;
}

const char *sym_symbol(Sym *s) {
  if (s->symbol)
    return s->symbol;
  if (s->kind == SY_EXTERN) {
    s->symbol = str_to_c(s->name); // C ABI name; emitter adds prefix per target
    return s->symbol;
  }
  Module *m = s->owner;
  s->symbol = arena_printf("rho_%s__%s", sanitize(str_to_c(m->path)), str_to_c(s->name));
  return s->symbol;
}

static void layout_rec(RecType *rec);

int64_t type_align(Type *t) {
  switch (t->kind) {
  case TY_BOOL: case TY_I8: case TY_U8: return 1;
  case TY_I16: case TY_U16: return 2;
  case TY_I32: case TY_U32: case TY_F32: return 4;
  case TY_I64: case TY_U64: case TY_F64: case TY_ISIZE: case TY_USIZE:
    return 8;
  case TY_PTR: case TY_WEAK: case TY_SLICE: case TY_STRING: return 8;
  case TY_ARRAY: return type_align(t->elem);
  case TY_STRUCT: case TY_ENUM:
    layout_rec(t->rec);
    return t->rec->align;
  default:
    return 8;
  }
}

static int64_t round_up_i64(int64_t v, int64_t a) { return (v + a - 1) / a * a; }

int64_t type_size(Type *t) {
  switch (t->kind) {
  case TY_BOOL: case TY_I8: case TY_U8: return 1;
  case TY_I16: case TY_U16: return 2;
  case TY_I32: case TY_U32: case TY_F32: return 4;
  case TY_I64: case TY_U64: case TY_F64: case TY_ISIZE: case TY_USIZE:
    return 8;
  case TY_PTR: case TY_WEAK: return 8;
  case TY_SLICE: case TY_STRING: return 24; // {buf, ptr, len}
  case TY_ARRAY: return (int64_t)t->len * type_size(t->elem);
  case TY_STRUCT: case TY_ENUM:
    layout_rec(t->rec);
    return t->rec->size;
  default:
    return 8;
  }
}

static void layout_rec(RecType *rec) {
  if (rec->align != 0)
    return;
  rec->align = -1; // cycle marker
  Decl *d = rec->decl;
  Module *saved = cur_module;
  cur_module = rec->owner;
  int64_t size = 0, align = 1;
  if (d->kind == DK_STRUCT) {
    struct_field_type(rec, 0); // resolve field types first
    for (size_t i = 0; i < d->fields.n; i++) {
      Type *ft = rec->field_types.items[i];
      int64_t fa = type_align(ft);
      align = align > fa ? align : fa;
      int64_t off = round_up_i64(size, fa);
      vec_push(&rec->offsets, (void *)(long)off);
      size = off + type_size(ft);
    }
  } else {
    align = 4;
    size = 4; // tag
    for (size_t i = 0; i < d->variants.n; i++) {
      VariantAst *v = d->variants.items[i];
      if (v->vkind == VAR_TUPLE) {
        for (size_t j = 0; j < v->types.n; j++) {
          Type *vt = resolve_type_in_module(rec->owner, v->types.items[j]);
          int64_t va = type_align(vt);
          align = align > va ? align : va;
          int64_t vs = type_size(vt);
          size = size > vs ? size : vs;
        }
      } else if (v->vkind == VAR_STRUCT) {
        for (size_t j = 0; j < v->fields.n; j++) {
          Type *vt = resolve_type_in_module(
              rec->owner, ((FieldAst *)v->fields.items[j])->ty);
          int64_t va = type_align(vt);
          align = align > va ? align : va;
          int64_t vs = type_size(vt);
          size = size > vs ? size : vs;
        }
      }
    }
  }
  cur_module = saved;
  rec->align = align;
  rec->size = round_up_i64(size, align);
}

int64_t struct_field_offset(RecType *rec, size_t i) {
  layout_rec(rec);
  if (rec->offsets.n > i)
    return (long)rec->offsets.items[i];
  return 0;
}

int64_t variant_payload_offset(Type *enum_t) {
  return 4; // tag is 4 bytes; payload starts after it (no packing)
}
