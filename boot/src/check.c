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

// active closure-capture collection: locals with ids below the boundary
// belong to enclosing scopes and are copy-captured when referenced
static Vec *g_cap_list;
static int g_cap_boundary;

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
  if (!t)
    return false;
  switch (t->kind) {
  // raw byte pointers (malloc results, extern buffers) carry no rc header;
  // a pointer is reference-counted exactly when it points at a `new` object
  case TY_PTR:
    return t->elem && (t->elem->kind == TY_STRUCT || t->elem->kind == TY_ENUM);
  case TY_WEAK: case TY_SLICE: case TY_STRING: case TY_FN:
    return true;
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

// ============================================================ generics =====
//
// Monomorphization: generic decls are templates. Signatures resolve once
// against TY_PARAM placeholders; call sites infer the mapping, and each
// distinct instantiation clones the declaration, re-resolves and re-checks
// its body under the concrete environment, and is lowered as a plain
// function. Template decls themselves are never checked or lowered.

static Map *g_tenv;  // active name -> Type* while resolving/checking a clone

static Type *ty_param_(char *name) {
  Type *t = ty_newk(TY_PARAM);
  t->mangled = name;
  return t;
}

static Map *template_env_of(Decl *d) {
  Map *env = arena_alloc_zeroed(sizeof(Map));
  for (size_t i = 0; i < d->tparams.n; i++)
    map_put(env, str_from(d->tparams.items[i]), ty_param_(d->tparams.items[i]));
  return env;
}

static Type *subst_type(Type *t, Map *env) {
  if (!t)
    return t;
  switch (t->kind) {
  case TY_PARAM: {
    Type *r = map_get(env, str_from(t->mangled));
    return r ? r : t;
  }
  case TY_PTR:
    return ty_ptr(subst_type(t->elem, env));
  case TY_WEAK:
    return ty_weak(subst_type(t->elem, env));
  case TY_SLICE:
    return ty_slice(subst_type(t->elem, env));
  case TY_ARRAY:
    return ty_array(subst_type(t->elem, env), t->len);
  case TY_FN: {
    Vec ps = {0};
    for (size_t i = 0; i < t->params.n; i++)
      vec_push(&ps, subst_type(t->params.items[i], env));
    return ty_fn(ps, subst_type(t->ret, env));
  }
  default:
    return t;
  }
}

// bind type parameters by matching a template parameter type against the
// argument's concrete type, structurally
static void infer_targs(Type *tmpl, Type *arg, Map *env) {
  if (!tmpl || !arg || tmpl->kind != TY_PARAM) {
    if (!tmpl || !arg)
      return;
    switch (tmpl->kind) {
    case TY_PTR:
      if (arg->kind == TY_PTR)
        infer_targs(tmpl->elem, arg->elem, env);
      return;
    case TY_WEAK:
      if (arg->kind == TY_WEAK)
        infer_targs(tmpl->elem, arg->elem, env);
      return;
    case TY_SLICE:
      if (arg->kind == TY_SLICE)
        infer_targs(tmpl->elem, arg->elem, env);
      return;
    case TY_ARRAY:
      if (arg->kind == TY_ARRAY && arg->len == tmpl->len)
        infer_targs(tmpl->elem, arg->elem, env);
      return;
    case TY_FN:
      if (arg->kind == TY_FN && arg->params.n == tmpl->params.n) {
        for (size_t i = 0; i < tmpl->params.n; i++)
          infer_targs(tmpl->params.items[i], arg->params.items[i], env);
        infer_targs(tmpl->ret, arg->ret, env);
      }
      return;
    case TY_STRUCT:
    case TY_ENUM:
      if (arg->kind == tmpl->kind && arg->rec->decl == tmpl->rec->decl) {
        size_t n = tmpl->rec->targs.n < arg->rec->targs.n ? tmpl->rec->targs.n
                                                          : arg->rec->targs.n;
        for (size_t i = 0; i < n; i++)
          infer_targs(tmpl->rec->targs.items[i], arg->rec->targs.items[i], env);
      }
      return;
    default:
      return;
    }
  }
  Type *bound = map_get(env, str_from(tmpl->mangled));
  if (!bound)
    map_put(env, str_from(tmpl->mangled), arg);
  else if (bound != arg && bound->kind != TY_ERR && arg->kind != TY_ERR) {
    // conflicting bindings: the substituted signature reports it via require()
    if (bound->kind == TY_INT_LIT || arg->kind == TY_INT_LIT ||
        bound->kind == TY_FLOAT_LIT || arg->kind == TY_FLOAT_LIT)
      map_put(env, str_from(tmpl->mangled), bound->kind == TY_INT_LIT ? arg : bound);
  }
}

static bool type_has_param(Type *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case TY_PARAM:
    return true;
  case TY_PTR: case TY_WEAK: case TY_SLICE:
    return type_has_param(t->elem);
  case TY_ARRAY:
    return type_has_param(t->elem);
  case TY_FN:
    if (type_has_param(t->ret))
      return true;
    for (size_t i = 0; i < t->params.n; i++)
      if (type_has_param(t->params.items[i]))
        return true;
    return false;
  default:
    return false;
  }
}

// ---- declaration cloning (instantiation bodies) ----------------------------

static TypeAst *clone_typeast(TypeAst *t);
static Expr *clone_expr(Expr *e);

static Vec clone_ptr_vec(Vec *v, void *(*fn)(void *)) {
  Vec out = {0};
  for (size_t i = 0; i < v->n; i++)
    vec_push(&out, fn(v->items[i]));
  return out;
}

static TypeAst *clone_typeast(TypeAst *t) {
  if (!t)
    return NULL;
  TypeAst *c = arena_alloc(sizeof(TypeAst));
  *c = *t;
  c->targs = clone_ptr_vec(&t->targs, (void *(*)(void *))clone_typeast);
  c->params = clone_ptr_vec(&t->params, (void *(*)(void *))clone_typeast);
  c->elem = clone_typeast(t->elem);
  c->ret = clone_typeast(t->ret);
  c->size = clone_expr(t->size);
  return c;
}

static FieldAst *clone_fieldast(FieldAst *f) {
  if (!f)
    return NULL;
  FieldAst *c = arena_alloc(sizeof(FieldAst));
  *c = *f;
  c->ty = clone_typeast(f->ty);
  return c;
}

static Param *clone_param(Param *p) {
  if (!p)
    return NULL;
  Param *c = arena_alloc(sizeof(Param));
  *c = *p;
  c->ty = clone_typeast(p->ty);
  return c;
}

static MatchArm *clone_arm(MatchArm *a) {
  if (!a)
    return NULL;
  MatchArm *c = arena_alloc(sizeof(MatchArm));
  c->pk = a->pk;
  c->pat_path = a->pat_path; // char* segments are immutable
  c->pat_int = a->pat_int;
  c->pat_str = a->pat_str;
  c->pat_names = a->pat_names;
  c->pat_fields = clone_ptr_vec(&a->pat_fields, (void *(*)(void *))clone_fieldast);
  c->body = clone_expr(a->body);
  c->file = a->file;
  c->line = a->line;
  c->col = a->col;
  return c;
}

static Stmt *clone_stmt(Stmt *s) {
  if (!s)
    return NULL;
  Stmt *c = arena_alloc_zeroed(sizeof(Stmt));
  c->kind = s->kind;
  c->file = s->file;
  c->line = s->line;
  c->col = s->col;
  c->mut = s->mut;
  c->tail = s->tail;
  c->name = s->name;
  c->ty = clone_typeast(s->ty);
  c->a = clone_expr(s->a);
  c->b = clone_expr(s->b);
  c->assign_op = s->assign_op;
  c->stmts = clone_ptr_vec(&s->stmts, (void *(*)(void *))clone_stmt);
  c->cond = clone_expr(s->cond);
  c->body = clone_ptr_vec(&s->body, (void *(*)(void *))clone_stmt);
  return c;
}

static Expr *clone_expr(Expr *e) {
  if (!e)
    return NULL;
  Expr *c = arena_alloc_zeroed(sizeof(Expr));
  c->kind = e->kind;
  c->file = e->file;
  c->line = e->line;
  c->col = e->col;
  c->iv = e->iv;
  c->fv = e->fv;
  c->sv = e->sv;
  c->bv = e->bv;
  c->a = clone_expr(e->a);
  c->b = clone_expr(e->b);
  c->c = clone_expr(e->c);
  c->binop = e->binop;
  c->unop = e->unop;
  c->args = clone_ptr_vec(&e->args, (void *(*)(void *))clone_expr);
  c->arg_names = e->arg_names; // char* entries, immutable
  c->ty = clone_typeast(e->ty);
  c->arms = clone_ptr_vec(&e->arms, (void *(*)(void *))clone_arm);
  c->params = clone_ptr_vec(&e->params, (void *(*)(void *))clone_param);
  c->ret = clone_typeast(e->ret);
  // NEW field lists: FieldAst* in items alongside args
  if (e->kind == EX_NEW) {
    Vec items = {0};
    for (size_t i = 0; i < e->items.n; i++)
      vec_push(&items, clone_fieldast(e->items.items[i]));
    c->items = items;
  } else {
    c->items = clone_ptr_vec(&e->items, (void *(*)(void *))clone_stmt);
  }
  return c;
}

static Decl *clone_fn_decl(Decl *d) {
  Decl *c = arena_alloc_zeroed(sizeof(Decl));
  c->kind = DK_FN;
  c->file = d->file;
  c->line = d->line;
  c->col = d->col;
  c->pub_ = d->pub_;
  c->name = d->name;
  c->params = clone_ptr_vec(&d->params, (void *(*)(void *))clone_param);
  c->ret = clone_typeast(d->ret);
  c->body = clone_ptr_vec(&d->body, (void *(*)(void *))clone_stmt);
  c->is_method = d->is_method;
  c->recv = d->recv;
  return c;
}

// ---- instantiation ---------------------------------------------------------

static Vec g_instantiations; // Sym* pending body checks, in discovery order
static Map g_instantiated;   // mangled key -> Sym* (dedup across modules)
static int g_instantiation_depth;

static void resolve_sym_type(Sym *sym);

static Sym *instantiate_fn(Sym *gsym, Vec *names, Map *env, Expr *at) {
  Decl *d = gsym->decl;
  // mangle in the canonical parameter order so the name is deterministic
  SB sb = {0};
  sb_printf(&sb, "%.*s$", (int)d->name.n, d->name.p);
  for (size_t i = 0; i < names->n; i++) {
    char *name = names->items[i];
    Type *targ = map_get(env, str_from(name));
    if (i)
      sb_push(&sb, ',');
    if (!targ || targ->kind == TY_PARAM) {
      err_at(at->file, at->line, at->col, "cannot infer `%s` for `%s`", name,
             str_to_c(d->name));
      return gsym;
    }
    if (targ->kind == TY_INT_LIT)
      targ = ty_prim(PRIM_I32);
    if (targ->kind == TY_FLOAT_LIT)
      targ = ty_prim(PRIM_F64);
    map_put(env, str_from(name), targ);
    // sanitize into the symbol name
    for (const char *p = targ->mangled; *p; p++)
      sb_push(&sb, ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                            (*p >= '0' && *p <= '9') || *p == '_')
                       ? *p
                       : '_');
  }
  Str mangled = sb_finish(&sb);
  Sym *existing = map_get(&g_instantiated, mangled);
  if (existing)
    return existing;
  if (++g_instantiation_depth > 256) {
    err_at(at->file, at->line, at->col,
           "generic instantiation nesting too deep (recursive generics?)");
    g_instantiation_depth--;
    return gsym;
  }
  Decl *clone = clone_fn_decl(d);
  clone->name = mangled;
  clone->tenv = env;
  Sym *sym = arena_alloc_zeroed(sizeof(Sym));
  sym->kind = SY_FN;
  sym->name = mangled;
  sym->decl = clone;
  sym->owner = gsym->owner;
  map_put(&g_instantiated, mangled, sym);
  Module *saved = cur_module;
  Map *saved_env = g_tenv;
  cur_module = gsym->owner;
  g_tenv = env;
  resolve_sym_type(sym);
  g_tenv = saved_env;
  cur_module = saved;
  map_put(&((Module *)gsym->owner)->syms, mangled, sym);
  vec_push(&g_instantiations, sym);
  g_instantiation_depth--;
  return sym;
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
Type *struct_field_type(RecType *rec, size_t i);
Type *variant_field_type(Type *enum_t, int variant, int field);
static void check_fn_body(Decl *d, Sym *sym);
static bool block_returns(Vec *stmts);

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
  g_tenv = NULL;
  g_instantiations = (Vec){0};
  g_instantiated = (Map){0};
  g_instantiation_depth = 0;
}

bool g_prelude_wasm = false; // wasm32-wasi targets use the fd_write prelude

void prelude_init(void) {
  if (prelude_loaded)
    return;
  prelude_loaded = true;
  extern const char PRELUDE_SOURCE[];
  extern const char PRELUDE_WASI_SOURCE[];
  Str path = str_from("<prelude>");
  Str src = str_from(g_prelude_wasm ? PRELUDE_WASI_SOURCE : PRELUDE_SOURCE);
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

// the RecType of a generic definition (T's bound to TY_PARAM placeholders)
static RecType *ensure_template(Decl *d, Module *owner) {
  if (!d->templ) {
    char *base = arena_printf("%.*s.%.*s", (int)owner->path.n, owner->path.p, (int)d->name.n,
                              d->name.p);
    RecType *rec = rec_intern(d, owner, str_from(arena_printf("%s[]", base)));
    if (!rec->env) {
      rec->is_template = true;
      rec->env = template_env_of(d);
      rec->targs = (Vec){0};
      for (size_t i = 0; i < d->tparams.n; i++)
        vec_push(&rec->targs, map_get(rec->env, str_from(d->tparams.items[i])));
      d->templ = rec;
    }
  }
  return (RecType *)d->templ;
}

static Type *inst_from_targs(Sym *sym, Module *owner, Vec targs) {
  Decl *d = sym->decl;
  char *base = arena_printf("%.*s.%.*s", (int)owner->path.n, owner->path.p, (int)d->name.n,
                            d->name.p);
  SB sb = {0};
  sb_printf(&sb, "%s[", base);
  for (size_t i = 0; i < targs.n; i++) {
    if (i)
      sb_push(&sb, ',');
    sb_append_c(&sb, ((Type *)targs.items[i])->mangled);
  }
  sb_push(&sb, ']');
  Str mangled = sb_finish(&sb);
  RecType *rec = rec_intern(d, owner, mangled);
  if (!rec->env) {
    rec->env = arena_alloc_zeroed(sizeof(Map));
    for (size_t i = 0; i < d->tparams.n; i++)
      map_put(rec->env, str_from(d->tparams.items[i]), targs.items[i]);
    rec->targs = targs;
  }
  Type *t = ty_newk(sym->kind == SY_STRUCT ? TY_STRUCT : TY_ENUM);
  t->rec = rec;
  return ty_intern2(mangled, t);
}

static Type *named_type(Sym *sym, Module *owner, TypeAst *ta) {
  Decl *d = sym->decl;
  size_t nparams = d->tparams.n;
  size_t ntargs = ta ? ta->targs.n : 0;
  if (nparams == 0 && ntargs == 0) {
    char *base = arena_printf("%.*s.%.*s", (int)owner->path.n, owner->path.p, (int)d->name.n,
                              d->name.p);
    RecType *rec = rec_intern(d, owner, str_from(base));
    Type *t = ty_newk(sym->kind == SY_STRUCT ? TY_STRUCT : TY_ENUM);
    t->rec = rec;
    return ty_intern2(str_from(base), t);
  }
  if (ntargs == 0) {
    // bare generic name: inside an instantiation clone (env all concrete) it
    // names the concrete instance; inside the definition itself it names the
    // template; anywhere else it is an error
    bool all_bound = true, any_param = false;
    Vec targs = {0};
    for (size_t i = 0; i < d->tparams.n; i++) {
      Type *tv = g_tenv ? map_get(g_tenv, str_from(d->tparams.items[i])) : NULL;
      if (!tv || tv->kind == TY_ERR) {
        all_bound = false;
        break;
      }
      if (tv->kind == TY_PARAM)
        any_param = true;
      vec_push(&targs, tv);
    }
    if (all_bound && !any_param)
      return inst_from_targs(sym, owner, targs);
    if (all_bound && any_param) {
      // intern by the rec's own mangled name: an un-mangled type would make
      // every ty_ptr<T-template> intern under the same key and alias
      RecType *rt = ensure_template(d, owner);
      Type *t = ty_newk(sym->kind == SY_STRUCT ? TY_STRUCT : TY_ENUM);
      t->rec = rt;
      return ty_intern2(rt->mangled, t); // template reference (self position)
    }
    if (ta)
      err_at(ta->file, ta->line, ta->col, "generic type `%s` needs type arguments",
             str_to_c(d->name));
    return ty_err_;
  }
  if (nparams != ntargs) {
    err_at(ta->file, ta->line, ta->col, "`%s` takes %zu type arguments, got %zu",
           str_to_c(d->name), nparams, ntargs);
    return ty_err_;
  }
  Vec targs = {0};
  for (size_t i = 0; i < ntargs; i++) {
    Type *targ = resolve_type_in_module(owner, ta->targs.items[i]);
    if (targ->kind == TY_INT_LIT)
      targ = ty_prim(PRIM_I32);
    if (targ->kind == TY_FLOAT_LIT)
      targ = ty_prim(PRIM_F64);
    if (targ->kind == TY_ERR)
      return ty_err_;
    vec_push(&targs, targ);
  }
  return inst_from_targs(sym, owner, targs);
}

Type *struct_field_type(RecType *rec, size_t i) {
  if (!rec->fields_done) {
    if (rec->resolving)
      return ty_err_; // self-containing struct; layout phase reports it
    rec->resolving = true;
    Decl *d = rec->decl;
    Module *saved = cur_module;
    Map *saved_env = g_tenv;
    cur_module = rec->owner;
    g_tenv = (Map *)rec->env;
    for (size_t j = 0; j < d->fields.n; j++) {
      FieldAst *fa = d->fields.items[j];
      vec_push(&rec->field_types, resolve_type_in_module(rec->owner, fa->ty));
    }
    g_tenv = saved_env;
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
      if (g_tenv && ta->targs.n == 0) {
        Type *tv = map_get(g_tenv, last);
        if (tv) {
          result = tv;
          break;
        }
      }
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

// an error type, possibly wrapped in a pointer/slice/array that failed to
// resolve — such types already carry their own diagnostic. Failed generic
// instantiations keep their kind but carry the <error> mangled name.
static bool ty_is_or_has_err(Type *t) {
  if (!t)
    return false;
  if (t->kind == TY_ERR || (t->mangled && !strcmp(t->mangled, "<error>")))
    return true;
  if (t->kind == TY_PTR || t->kind == TY_WEAK || t->kind == TY_SLICE ||
      t->kind == TY_ARRAY)
    return ty_is_or_has_err(t->elem);
  return false;
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
    // stay untyped until a consumer fixes the width
    if (expected && ty_is_int(expected) && expected->kind != TY_INT_LIT)
      e->typed = expected;
    else
      e->typed = ty_newk(TY_INT_LIT);
    return e->typed;
  }
  case EX_FLOAT: {
    if (expected && expected->kind == TY_F32)
      e->typed = expected;
    else if (expected && expected->kind == TY_F64)
      e->typed = expected;
    else
      e->typed = ty_newk(TY_FLOAT_LIT);
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
    if (sym->kind == SY_FN || sym->kind == SY_EXTERN)
      e->fnval = true; // a fn name in value position makes a closure value
    if (g_cap_list && (sym->kind == SY_LOCAL || sym->kind == SY_PARAM) &&
        sym->local_id < g_cap_boundary) {
      bool dup = false;
      for (size_t i = 0; i < g_cap_list->n; i++)
        if (str_eq(((Sym *)g_cap_list->items[i])->name, sym->name))
          dup = true;
      if (!dup)
        vec_push(g_cap_list, sym);
    }
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
      e->typed = a; // INT_LIT stays untyped; adapted in context
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
    // pointer/usize escapes: the one sanctioned hole for std/runtime plumbing
    if ((src->kind == TY_PTR || target->kind == TY_PTR) &&
        (target->kind == TY_USIZE || target->kind == TY_I64 ||
         src->kind == TY_USIZE || src->kind == TY_I64))
      ok = true;
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
        if (l->kind == TY_ENUM && l == r)
          ok = true; // tags, or the synthesized field-by-field eq glue
        else if ((l->kind == TY_STRUCT || r->kind == TY_STRUCT || l->kind == TY_ARRAY))
          ok = false;
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
      e->typed = l; // may remain INT_LIT; consumers pin it
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
    // both operands untyped: pin the default width now
    if (l->kind == TY_INT_LIT)
      l = ty_prim(PRIM_I32);
    if (l->kind == TY_FLOAT_LIT) {
      // float literals default to f64 (spec §1), or follow the context;
      // retype the operand nodes too so the lowerer sees settled types
      l = expected && expected->kind == TY_F32 ? ty_prim(PRIM_F32) : ty_prim(PRIM_F64);
      Type *at = (Type *)e->a->typed;
      Type *bt2 = (Type *)e->b->typed;
      if (at && at->kind == TY_FLOAT_LIT)
        e->a->typed = l;
      if (bt2 && bt2->kind == TY_FLOAT_LIT)
        e->b->typed = l;
    }
    e->typed = l;
    return e->typed;
  }
  case EX_INDEX: {
    Type *base = check_expr(e->a, NULL);
    Type *idx = check_expr(e->b, NULL);
    if (idx->kind == TY_INT_LIT) {
      idx = ty_prim(PRIM_USIZE);
      e->b->typed = idx;
    }
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
    if (lo->kind == TY_INT_LIT) {
      lo = ty_prim(PRIM_USIZE);
      e->b->typed = lo;
    }
    if (hi->kind == TY_INT_LIT) {
      hi = ty_prim(PRIM_USIZE);
      e->c->typed = hi;
    }
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
  case EX_CLOSURE: {
    // copy-capture closures: the body is checked here against its own
    // signature; outer locals it names are snapshotted into the environment
    Vec *saved_caps = g_cap_list;
    int saved_boundary = g_cap_boundary;
    Type *saved_ret = cur_ret;
    bool saved_defer = in_defer;
    int saved_loop = loop_depth;
    Vec *caps = arena_alloc_zeroed(sizeof(Vec));
    g_cap_list = caps;
    g_cap_boundary = next_local_id;

    Vec ps = {0};
    scope_push();
    for (size_t i = 0; i < e->params.n; i++) {
      Param *pa = e->params.items[i];
      Type *pt = resolve_type_in_module(cur_module, pa->ty);
      vec_push(&ps, pt);
      Sym *psym = arena_alloc_zeroed(sizeof(Sym));
      psym->kind = SY_PARAM;
      psym->name = pa->name;
      psym->type = pt;
      psym->mutable = true; // parameters are mutable local copies
      psym->local_id = next_local_id++;
      if (map_has(&cur_scope->syms, pa->name))
        err_at(pa->file, pa->line, pa->col, "duplicate parameter `%s`",
               str_to_c(pa->name));
      else
        map_put(&cur_scope->syms, pa->name, psym);
    }
    Type *ret = e->ret ? resolve_type_in_module(cur_module, e->ret) : ty_void_;
    Type *sig = ty_fn(ps, ret);

    cur_ret = ret;
    in_defer = false;
    loop_depth = 0;
    for (size_t i = 0; i < e->items.n; i++)
      check_stmt(e->items.items[i]);
    if (ret->kind != TY_VOID && !block_returns(&e->items)) {
      ERR(e, "missing return: closure body must produce `%s`", ty_name(ret));
    }

    scope_pop();
    g_cap_list = saved_caps;
    g_cap_boundary = saved_boundary;
    cur_ret = saved_ret;
    in_defer = saved_defer;
    loop_depth = saved_loop;

    for (size_t i = 0; i < caps->n; i++) {
      Sym *cap = caps->items[i];
      if (cap->kind == SY_LOCAL && cap->mutable)
        ERR(e, "cannot capture mutable `%s` (boot-era closures copy; box it "
               "in a struct instead)",
            str_to_c(cap->name));
    }
    e->caps = *caps;
    e->typed = sig;
    return e->typed;
  }
  case EX_QMARK: {
    Type *operand = check_expr(e->a, NULL);
    if (operand->kind != TY_ENUM) {
      const char *what = operand->mangled ? operand->mangled : "an untyped literal";
      ERR(e, "`?` needs a Result or Option, found `%s`", what);
      e->typed = ty_err_;
      return e->typed;
    }
    Decl *ed = operand->rec->decl;
    int ok = -1, err = -1, none = -1, some = -1;
    for (size_t i = 0; i < ed->variants.n; i++) {
      Str vn = ((VariantAst *)ed->variants.items[i])->name;
      if (str_eq_c(vn, "Ok"))
        ok = (int)i;
      if (str_eq_c(vn, "Err"))
        err = (int)i;
      if (str_eq_c(vn, "Some"))
        some = (int)i;
      if (str_eq_c(vn, "None"))
        none = (int)i;
    }
    bool result_like = ok >= 0 && err >= 0;
    bool option_like = some >= 0 && none >= 0;
    if (!result_like && !option_like) {
      ERR(e, "`?` needs a Result or Option, found `%s`", ty_name(operand));
      e->typed = ty_err_;
      return e->typed;
    }
    e->q_ok = result_like ? ok : some;
    e->q_err = result_like ? err : none;
    // the enclosing function must return the same family, with a
    // compatible error payload for Results
    if (!cur_ret || cur_ret->kind != TY_ENUM || cur_ret->rec->decl != ed) {
      ERR(e, "`?` propagates into `%s`, but this function returns `%s`",
          result_like ? "Result" : "Option", ty_name(cur_ret ? cur_ret : ty_void_));
      e->typed = ty_err_;
      return e->typed;
    }
    if (result_like) {
      Type *e_op = variant_field_type(operand, err, 0);
      Type *e_ret = variant_field_type(cur_ret, err, 0);
      if (!ty_eq(e_op, e_ret)) {
        ERR(e, "error payload mismatch: function propagates `%s`, operand carries `%s`",
            ty_name(e_ret), ty_name(e_op));
        e->typed = ty_err_;
        return e->typed;
      }
    }
    e->typed = variant_field_type(operand, e->q_ok, 0);
    return e->typed;
  }
  case EX_MAKE: // never produced by the parser; `make` is checked in EX_CALL
  case EX_ENUM_CTOR:
  case EX_METHOD:
    ERR(e, "internal: unexpected expression form");
    e->typed = ty_err_;
    return e->typed;
  }
  return ty_err_;
}

static bool is_panic_call(Expr *e) {
  if (!e || e->kind != EX_CALL || !e->a || !e->a->sym)
    return false;
  Sym *s = (Sym *)e->a->sym;
  // the prelude's panic — imported syms can carry the importer as owner
  if (!str_eq_c(s->name, "panic") || !s->owner)
    return false;
  Module *om = (Module *)s->owner;
  return om == prelude_module || om->is_prelude;
}

static Type *unify2(Type *a, Type *b, Expr *at) {
  if (!a || a->kind == TY_ERR)
    return b;
  if (!b || b->kind == TY_ERR)
    return a;
  // panic never returns: its arm coerces to the other branch's type
  if (getenv("RHO_DBG_U") && at && at->kind == EX_CALL) {
    Sym *s2 = at->a && at->a->sym ? (Sym *)at->a->sym : NULL;
    fprintf(stderr, "[unify] a=%s b=%s name=%.*s owner_is_prelude=%s prelude=%p owner=%p\n",
            ty_name(a), ty_name(b),
            s2 ? (int)s2->name.n : 0, s2 && s2->name.p ? s2->name.p : "?",
            s2 && s2->owner == prelude_module ? "yes" : "NO",
            (void *)prelude_module, s2 ? s2->owner : NULL);
  }
  if (a->kind == TY_VOID && b->kind != TY_VOID && is_panic_call(at))
    return b;
  if (b->kind == TY_VOID && a->kind != TY_VOID && is_panic_call(at))
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
      e->sym = item;
      e->typed = item->type;
      e->fnval = true;
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
          // unit variant of a generic enum: take the expected instantiation
          // when the context pins one (`let x: Option[i32] = Option.None`)
          if (sym->type->rec && sym->type->rec->is_template && expected &&
              expected->kind == TY_ENUM && expected->rec->decl == d)
            vs->type = expected;
          vs->decl = d;
          vs->variant_index = (int)i;
          e->sym = vs;
          e->typed = vs->type;
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
  Sym *sym;          // CT_FN/CT_EXTERN
  Type *fn_type;     // resolved signature (params include self for methods)
  int variant;       // CT_VARIANT: variant index
  Type *enum_type;   // CT_VARIANT
  bool is_method;    // callee supplies self as args[0]
  bool is_ctor;      // struct-variant with named args
  RecType *inst_rec; // method found on a generic template: the receiver's
                     // instantiation the call must be specialized for
} CallTarget;

static Type *deref_to_struct(Type *t) {
  while (t && t->kind == TY_PTR)
    t = t->elem;
  return t && (t->kind == TY_STRUCT || t->kind == TY_ENUM) ? t : NULL;
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
    if (sym->type && sym->type->kind == TY_FN) {
      // fn-typed local/param: indirect call through the closure value
      ct.kind = CT_INDIRECT;
      ct.fn_type = sym->type;
      callee->sym = sym;
      callee->typed = sym->type;
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
            Sym *vs = arena_alloc_zeroed(sizeof(Sym));
            vs->kind = SY_VARIANT;
            vs->name = callee->sv;
            vs->type = base->type;
            vs->decl = d;
            vs->variant_index = (int)i;
            callee->sym = vs;
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
      // generic receiver: the methods live on the template — the call is
      // specialized for this instantiation in check_call
      if (rec->is_template == false && rec->decl->templ) {
        RecType *tmpl = rec->decl->templ;
        for (size_t i = 0; i < tmpl->methods.n; i++) {
          Sym *m = tmpl->methods.items[i];
          if (str_eq(m->name, callee->sv)) {
            ct.kind = CT_FN;
            ct.sym = m;
            ct.fn_type = m->type;
            ct.is_method = true;
            ct.inst_rec = rec;
            callee->sym = m;
            callee->a->typed = bt;
            return ct;
          }
        }
      }
    }
    if (getenv("RHO_DBG_M") && rec_t) {
      RecType *rr = rec_t->rec;
      RecType *tt = rr->decl->templ;
      fprintf(stderr, "[nomethod] rec=%s tmpl=%s nmethods=%u tmplmethods=%u\n",
              rr->mangled.p ? rr->mangled.p : "?",
              tt ? (tt->mangled.p ? tt->mangled.p : "?") : "NULL",
              (unsigned)rr->methods.n, tt ? (unsigned)tt->methods.n : 0u);
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
    if (len->kind == TY_INT_LIT) {
      len = ty_prim(PRIM_USIZE);
      ((Expr *)e->args.items[1])->typed = len;
    }
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
  // weak.from(p): borrow a `new` object as a weak reference
  if (e->a->kind == EX_FIELD && e->a->a->kind == EX_NAME &&
      str_eq_c(e->a->a->sv, "weak") && str_eq_c(e->a->sv, "from")) {
    if (e->args.n != 1) {
      ERR(e, "weak.from takes one argument");
      e->typed = ty_err_;
      return e->typed;
    }
    Type *t = check_expr(e->args.items[0], NULL);
    if (t->kind != TY_PTR ||
        (t->elem->kind != TY_STRUCT && t->elem->kind != TY_ENUM)) {
      ERR(e, "weak.from needs a pointer to a `new` object, found `%s`", ty_name(t));
      e->typed = ty_err_;
      return e->typed;
    }
    e->typed = ty_weak(t->elem);
    return e->typed;
  }
  // w.get(): follow a weak reference; null once the object is gone
  if (e->a->kind == EX_FIELD && str_eq_c(e->a->sv, "get")) {
    Type *bt = e->a->a->typed ? e->a->a->typed : check_expr(e->a->a, NULL);
    if (bt && bt->kind == TY_WEAK) {
      if (e->args.n != 0)
        ERR(e, "get takes no arguments");
      e->typed = ty_ptr(bt->elem);
      return e->typed;
    }
  }
  // intrinsics.* — the raw-memory kernel of spec §10 (std builds on it;
  // the self-hosted compiler needs it for argv and buffer plumbing)
  if (e->a->kind == EX_FIELD && e->a->a->kind == EX_NAME &&
      str_eq_c(e->a->a->sv, "intrinsics")) {
    const char *name = str_to_c(e->a->sv);
    Type *ptr_u8 = ty_ptr(ty_prim(PRIM_U8));
    bool is_load = !strcmp(name, "load_u8") || !strcmp(name, "load_u32") ||
                   !strcmp(name, "load_u64") || !strcmp(name, "load_i64");
    bool is_store = !strcmp(name, "store_u8") || !strcmp(name, "store_u32") ||
                    !strcmp(name, "store_u64") || !strcmp(name, "store_i64");
    bool is_memcpy = !strcmp(name, "memcpy");
    bool is_slice_str = !strcmp(name, "slice_string");
    if (!is_load && !is_store && !is_memcpy && !is_slice_str) {
      ERR(e, "unknown intrinsic `%s`", name);
      e->typed = ty_err_;
      return e->typed;
    }
    if (is_load) {
      if (e->args.n != 1) {
        ERR(e, "intrinsics.%s takes one argument", name);
        e->typed = ty_err_;
        return e->typed;
      }
      Type *t = check_expr(e->args.items[0], NULL);
      if (t->kind != TY_PTR) {
        ERR(e, "intrinsics.%s needs a pointer, found `%s`", name, ty_name(t));
        e->typed = ty_err_;
        return e->typed;
      }
      e->typed = ty_prim(!strcmp(name, "load_u8") ? PRIM_U8
                      : !strcmp(name, "load_u32") ? PRIM_U32
                      : !strcmp(name, "load_u64") ? PRIM_U64
                                                  : PRIM_I64);
      return e->typed;
    }
    if (is_store) {
      if (e->args.n != 2) {
        ERR(e, "intrinsics.%s takes two arguments", name);
        e->typed = ty_err_;
        return e->typed;
      }
      Type *t = check_expr(e->args.items[0], NULL);
      if (t->kind != TY_PTR) {
        ERR(e, "intrinsics.%s needs a pointer, found `%s`", name, ty_name(t));
        e->typed = ty_err_;
        return e->typed;
      }
      Type *v = adapt_literal(check_expr(e->args.items[1], NULL), NULL);
      Type *want = ty_prim(!strcmp(name, "store_u8") ? PRIM_U8
                          : !strcmp(name, "store_u32") ? PRIM_U32
                          : !strcmp(name, "store_u64") ? PRIM_U64
                                                       : PRIM_I64);
      if (!ty_eq(adapt_literal(v, want), want)) {
        ERR(e, "intrinsics.%s value: expected `%s`, found `%s`", name,
            ty_name(want), ty_name(v));
        e->typed = ty_err_;
        return e->typed;
      }
      e->typed = ty_void_;
      return e->typed;
    }
    if (is_memcpy) {
      if (e->args.n != 3) {
        ERR(e, "intrinsics.memcpy takes three arguments");
        e->typed = ty_err_;
        return e->typed;
      }
      for (int k = 0; k < 2; k++) {
        Type *t = check_expr(e->args.items[k], NULL);
        if (t->kind != TY_PTR) {
          ERR(e, "intrinsics.memcpy needs pointers, found `%s`", ty_name(t));
          e->typed = ty_err_;
          return e->typed;
        }
      }
      Type *n = check_expr(e->args.items[2], NULL);
      if (!ty_is_int(n)) {
        ERR(e, "intrinsics.memcpy length must be an integer, found `%s`", ty_name(n));
        e->typed = ty_err_;
        return e->typed;
      }
      e->typed = ty_void_;
      return e->typed;
    }
    // slice_string: copy a []u8 into a fresh managed string
    if (e->args.n != 1) {
      ERR(e, "intrinsics.slice_string takes one argument");
      e->typed = ty_err_;
      return e->typed;
    }
    Type *t = check_expr(e->args.items[0], NULL);
    if (t->kind != TY_SLICE || t->elem->kind != TY_U8) {
      ERR(e, "intrinsics.slice_string needs a []u8, found `%s`", ty_name(t));
      e->typed = ty_err_;
      return e->typed;
    }
    e->typed = ty_prim(PRIM_STRING);
    return e->typed;
  }

  CallTarget ct = resolve_callee(e->a);
  if (ct.kind == CT_NONE) {
    e->typed = ty_err_;
    return e->typed;
  }
  // generic call: infer the type arguments and specialize
  if (ct.kind == CT_FN &&
      (ct.inst_rec || (ct.sym->decl->templated && ct.sym->decl->tparams.n))) {
    Map *env = arena_alloc_zeroed(sizeof(Map));
    Vec names = {0};
    if (ct.inst_rec) {
      Decl *rd = ct.inst_rec->decl;
      for (size_t i = 0; i < rd->tparams.n && i < ct.inst_rec->targs.n; i++) {
        vec_push(&names, rd->tparams.items[i]);
        map_put(env, str_from(rd->tparams.items[i]), ct.inst_rec->targs.items[i]);
      }
    }
    for (size_t i = 0; i < ct.sym->decl->tparams.n; i++)
      vec_push(&names, ct.sym->decl->tparams.items[i]);
    Type *fn_t = ct.fn_type; // template signature
    size_t nparams = fn_t->params.n;
    size_t first_arg = ct.is_method ? 1 : 0;
    if (ct.is_method) {
      Type *recv = e->a->a->typed;
      if (!recv)
        recv = check_expr(e->a->a, NULL);
      infer_targs(fn_t->params.items[0], recv, env);
    }
    if (e->args.n + first_arg != nparams)
      ERR(e, "function takes %zu arguments, got %zu", nparams - first_arg, e->args.n);
    for (size_t i = 0; i < e->args.n; i++) {
      Type *at = check_expr(e->args.items[i], NULL);
      infer_targs(fn_t->params.items[first_arg + i], at, env);
    }
    // untyped literals adapt through a direct `-> T` return
    if (expected && fn_t->ret && fn_t->ret->kind == TY_PARAM) {
      Type *targ = map_get(env, str_from(fn_t->ret->mangled));
      if (targ && targ->kind == TY_INT_LIT && ty_is_int(expected))
        map_put(env, str_from(fn_t->ret->mangled), expected);
      else if (targ && targ->kind == TY_FLOAT_LIT && ty_is_float(expected))
        map_put(env, str_from(fn_t->ret->mangled), expected);
    }
    Map *saved_env = g_tenv;
    g_tenv = NULL;
    Sym *inst = instantiate_fn(ct.sym, &names, env, e);
    g_tenv = saved_env;
    ct.sym = inst;
    ct.fn_type = inst->type;
    ct.inst_rec = NULL;
    e->a->sym = inst; // lower emits the call against the instantiation
  }
  if (ct.kind == CT_VARIANT) {
    Type *et = ct.enum_type;
    VariantAst *v = ((Decl *)et->rec->decl)->variants.items[ct.variant];
    // generic enum: infer the type arguments from the payload values
    if (et->rec->is_template) {
      Map *env = arena_alloc_zeroed(sizeof(Map));
      Decl *ed = et->rec->decl;
      size_t np = v->vkind == VAR_TUPLE ? v->types.n : v->vkind == VAR_STRUCT ? v->fields.n : 0;
      for (size_t i = 0; i < e->args.n && i < np; i++) {
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
        Type *at = check_expr(e->args.items[i], NULL);
        infer_targs(variant_field_type(et, ct.variant, fidx), at, env);
      }
      // tparams the payload cannot pin (Result.Err leaves T open) come from
      // the expected instantiation when the context provides one
      if (expected && expected->kind == TY_ENUM && expected->rec->decl == ed &&
          expected->rec->targs.n == ed->tparams.n && !expected->rec->is_template) {
        for (size_t i = 0; i < ed->tparams.n; i++) {
          char *tn = ed->tparams.items[i];
          Type *bound = map_get(env, str_from(tn));
          if (!bound || bound->kind == TY_PARAM)
            map_put(env, str_from(tn), expected->rec->targs.items[i]);
        }
      }
      for (size_t i = 0; i < ed->tparams.n; i++) {
        Type *targ = map_get(env, str_from(ed->tparams.items[i]));
        if (!targ || targ->kind == TY_PARAM) {
          ERR(e, "cannot infer `%s` for `%s`", (char *)ed->tparams.items[i],
              str_to_c(ed->name));
          e->typed = ty_err_;
          return e->typed;
        }
      }
      // build the concrete instance
      SB msb = {0};
      sb_printf(&msb, "%.*s.%.*s[", (int)((Module *)et->rec->owner)->path.n,
                ((Module *)et->rec->owner)->path.p, (int)ed->name.n, ed->name.p);
      Vec targs = {0};
      for (size_t i = 0; i < ed->tparams.n; i++) {
        Type *targ = map_get(env, str_from(ed->tparams.items[i]));
        if (targ->kind == TY_INT_LIT)
          targ = ty_prim(PRIM_I32);
        if (targ->kind == TY_FLOAT_LIT)
          targ = ty_prim(PRIM_F64);
        if (i)
          sb_push(&msb, ',');
        sb_append_c(&msb, targ->mangled);
        vec_push(&targs, targ);
      }
      sb_push(&msb, ']');
      Str mangled = sb_finish(&msb);
      RecType *rec = rec_intern(ed, et->rec->owner, mangled);
      if (!rec->env) {
        rec->env = arena_alloc_zeroed(sizeof(Map));
        for (size_t i = 0; i < ed->tparams.n; i++)
          map_put(rec->env, str_from(ed->tparams.items[i]), targs.items[i]);
        rec->targs = targs;
      }
      Type *inst = ty_newk(TY_ENUM);
      inst->rec = rec;
      et = ty_intern2(mangled, inst);
    }
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
          Type *pt = variant_field_type(et, ct.variant, (int)i);
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
            Type *ft = variant_field_type(et, ct.variant, (int)j);
            require(check_expr(e->args.items[i], ft), ft, (Expr *)e->args.items[i],
                    "variant field");
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
    e->typed = et;
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
  // never coercion: panic never returns, so a call to it is compatible with
  // any expected type (match arms like `Result.Err(_) => panic("...")`)
  if (expected && expected->kind != TY_ERR && expected->kind != TY_VOID &&
      fn_t->ret && fn_t->ret->kind == TY_VOID && ct.sym && ct.sym->owner &&
      str_eq_c(ct.sym->name, "panic") &&
      (((Module *)ct.sym->owner) == prelude_module ||
       ((Module *)ct.sym->owner)->is_prelude))
    e->typed = expected;
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
      } else if (arm->pk == PAT_UNIT || arm->pk == PAT_TUPLE || arm->pk == PAT_STRUCT) {
        Str vn = str_from(arm->pat_path.items[arm->pat_path.n - 1]);
        bool found = false;
        for (size_t j = 0; j < ed->variants.n; j++) {
          VariantAst *v = ed->variants.items[j];
          if (!str_eq(v->name, vn))
            continue;
          if (arm->pk == PAT_UNIT && v->vkind != VAR_UNIT) {
            ERR(arm, "variant `%s` carries a payload; bind it: `%s(..)`", str_to_c(vn),
                str_to_c(vn));
          } else if (arm->pk != PAT_UNIT && v->vkind == VAR_UNIT) {
            ERR(arm, "variant `%s` has no payload to bind", str_to_c(vn));
          }
          covered[j] = true;
          arm->variant_index = (int)j;
          arm->disc = v->disc;
          found = true;
          break;
        }
        if (!found) {
          ERR(arm, "no variant `%s` on `%s`", str_to_c(vn), ty_name(scrut));
        } else {
          VariantAst *v = ed->variants.items[arm->variant_index];
          size_t nfields =
              v->vkind == VAR_TUPLE ? v->types.n : v->vkind == VAR_STRUCT ? v->fields.n : 0;
          size_t nbinds = arm->pk == PAT_TUPLE ? arm->pat_names.n
                          : arm->pk == PAT_STRUCT ? arm->pat_fields.n : 0;
          if (arm->pk != PAT_UNIT && nbinds != nfields) {
            ERR(arm, "pattern `%s` binds %zu values, variant has %zu", str_to_c(vn), nbinds,
                nfields);
          } else if (arm->pk == PAT_TUPLE && v->vkind == VAR_TUPLE) {
            for (size_t b = 0; b < nbinds && b < nfields; b++) {
              char *nm = arm->pat_names.items[b];
              Sym *bs = NULL;
              if (!str_eq_c(str_from(nm), "_")) {
                bs = arena_alloc_zeroed(sizeof(Sym));
                bs->kind = SY_LOCAL;
                bs->name = str_from(nm);
                bs->type = variant_field_type(scrut, arm->variant_index, (int)b);
                bs->local_id = next_local_id++;
              }
              vec_push(&arm->bind_syms, bs);
              vec_push(&arm->bind_fidx, (void *)(long)b);
            }
          } else if (arm->pk == PAT_STRUCT && v->vkind == VAR_STRUCT) {
            // every pattern field must exist on the variant; bindings in
            // pattern order, types from the matched variant field
            for (size_t b = 0; b < nbinds; b++) {
              FieldAst *fa = arm->pat_fields.items[b];
              bool hit = false;
              for (size_t j2 = 0; j2 < v->fields.n; j2++) {
                FieldAst *vf = v->fields.items[j2];
                if (str_eq(vf->name, fa->name)) {
                  hit = true;
                  char *nm = arm->pat_names.items[b];
                  Sym *bs = NULL;
                  if (!str_eq_c(str_from(nm), "_")) {
                    bs = arena_alloc_zeroed(sizeof(Sym));
                    bs->kind = SY_LOCAL;
                    bs->name = str_from(nm);
                    bs->type = variant_field_type(scrut, arm->variant_index, (int)j2);
                    bs->local_id = next_local_id++;
                  }
                  vec_push(&arm->bind_syms, bs);
                  vec_push(&arm->bind_fidx, (void *)(long)j2);
                  break;
                }
              }
              if (!hit)
                ERR(fa, "variant `%s` has no field `%s`", str_to_c(vn), str_to_c(fa->name));
            }
            if (nbinds != 0 && arm->bind_syms.n != v->fields.n)
              ERR(arm, "pattern `%s { .. }` must bind every field", str_to_c(vn));
          }
        }
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
    scope_push();
    for (size_t b = 0; b < arm->bind_syms.n; b++) {
      Sym *bs = arm->bind_syms.items[b];
      if (!bs)
        continue;
      if (scope_lookup(bs->name)) {
        err_at(arm->file, arm->line, arm->col, "`%s` shadows an existing binding",
               str_to_c(bs->name));
      }
      scope_decl(bs->name, bs);
    }
    Type *bt = check_expr(arm->body, expected);
    scope_pop();
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
    if (ann) {
      // the annotation pins the binding's type; literals adapt, anything
      // else must genuinely match. An error type already has its own
      // diagnostic — don't pile a second one onto it
      if (!ty_is_or_has_err(ann) && !ty_is_or_has_err(t))
        require(t, ann, s->a, "initializer");
      t = ann;
    } else {
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
      value = adapt_literal(check_expr(s->b, target), target);
      s->b->typed = value;
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
  Map *saved_env = g_tenv;
  cur_module = sym->owner;
  switch (sym->kind) {
  case SY_FN:
  case SY_EXTERN: {
    Vec ps = {0};
    if (getenv("RHO_DBG_M"))
      fprintf(stderr, "[res] %s recv=%.*s tparams=%u is_self=%u\n", str_to_c(sym->name),
              (int)d->recv.n, d->recv.p ? d->recv.p : "", (unsigned)d->tparams.n,
              (unsigned)(d->params.n ? ((Param *)d->params.items[0])->is_self : 0));
    if (d->tparams.n) {
      g_tenv = template_env_of(d);
      d->templated = true;
    }
    // a method/associated fn on a generic type resolves under that type's
    // template environment, so `self: Pair` and `-> A` both see the tparams;
    // instantiations already carry their concrete env — don't overwrite it
    if (d->recv.n && !d->tenv && sym->owner) {
      Module *om = (Module *)sym->owner;
      Sym *rs = map_get(&om->syms, d->recv);
      Module *towner = om;
      if (!rs && prelude_module && prelude_module != om) {
        rs = map_get(&prelude_module->syms, d->recv);
        towner = prelude_module;
      }
      if (rs && (rs->kind == SY_STRUCT || rs->kind == SY_ENUM) && rs->decl->tparams.n) {
        RecType *tmpl = ensure_template(rs->decl, towner);
        g_tenv = (Map *)tmpl->env;
        d->templated = true;
      }
    }
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
          if (getenv("RHO_DBG_M"))
            fprintf(stderr, "[reg] %s on %s\n", str_to_c(sym->name),
                    rec_t->rec->mangled.p ? rec_t->rec->mangled.p : "?");
        }
      }
    }
    Type *ret = d->ret ? resolve_type_in_module(sym->owner, d->ret) : ty_void_;
    sym->type = ty_fn(ps, ret);
    break;
  }
  case SY_STRUCT:
  case SY_ENUM:
    if (d->tparams.n) {
      Type *t = ty_newk(sym->kind == SY_STRUCT ? TY_STRUCT : TY_ENUM);
      t->rec = ensure_template(d, sym->owner);
      sym->type = t;
    } else {
      sym->type = named_type(sym, sym->owner, NULL);
    }
    break;
  case SY_STATIC:
  case SY_CONST: {
    Type *t = resolve_type_in_module(sym->owner, d->ret);
    CV v = const_eval(d->init, sym->owner);
    if (!v.ok) {
      err_at(d->file, d->line, d->col,
             "`%s` initializer must be a compile-time constant", str_to_c(d->name));
    } else {
      CV *memo = arena_alloc(sizeof(CV));
      *memo = v;
      d->ceval_cache = memo;
      d->ceval_cache_ok = true;
    }
    sym->type = t;
    if (d->init)
      d->init->typed = t; // lower_static tests this to emit the data segment
    break;
  }
  default:
    break;
  }
  cur_module = saved;
  g_tenv = saved_env;
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

  // phase 3: fn bodies (templates are checked per instantiation, not here)
  for (size_t i = 0; i < g_module_order.n; i++) {
    Module *m = g_module_order.items[i];
    if (m->checked)
      continue;
    m->checked = true;
    for (size_t k = 0; k < m->syms.keys.n; k++) {
      Str *key = m->syms.keys.items[k];
      Sym *sym = map_get(&m->syms, *key);
      if ((sym->kind == SY_FN) && sym->decl && sym->decl->body.n && !sym->decl->templated &&
          !sym->decl->tenv)
        check_fn_body(sym->decl, sym);
    }
  }
  // instantiation worklist: each clone's body re-checked under its concrete
  // environment; checking may discover further instantiations, so the loop
  // runs until the queue settles
  for (size_t wl = 0; wl < g_instantiations.n; wl++) {
    Sym *sym = g_instantiations.items[wl];
    Map *saved_env = g_tenv;
    g_tenv = (Map *)sym->decl->tenv;
    check_fn_body(sym->decl, sym);
    g_tenv = saved_env;
  }
  cur_module = NULL;
  return diag_count() - before;
}

// ============================================================ shared =======

Module *g_prelude_module(void) { return prelude_module; }

const char *rho_sanitize(const char *s) {
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
  // instantiation names carry type arguments (`swap$i32,i64`) — sanitize so
  // the assembler sees one legal token
  s->symbol = arena_printf("rho_%s__%s", rho_sanitize(str_to_c(m->path)),
                           rho_sanitize(str_to_c(s->name)));
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
  case TY_PTR: case TY_WEAK: case TY_SLICE: case TY_STRING: case TY_FN:
    return 8;
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
  case TY_FN: return 16;                    // {code, env}
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
    Map *saved_env = g_tenv;
    g_tenv = (Map *)rec->env;
    for (size_t i = 0; i < d->variants.n; i++) {
      VariantAst *v = d->variants.items[i];
      Vec *foff = arena_alloc(sizeof(Vec));
      *foff = (Vec){0};
      Vec *ftys = arena_alloc(sizeof(Vec));
      *ftys = (Vec){0};
      vec_push(&rec->var_offsets, foff);
      vec_push(&rec->var_types, ftys);
      if (v->vkind == VAR_UNIT) {
        vec_push(&rec->var_poff, (void *)(long)4);
        continue;
      }
      size_t nfields = v->vkind == VAR_TUPLE ? v->types.n : v->fields.n;
      int64_t off = 4; // payload starts after the tag, per-field aligned
      for (size_t j = 0; j < nfields; j++) {
        TypeAst *fta = v->vkind == VAR_TUPLE ? v->types.items[j]
                                            : ((FieldAst *)v->fields.items[j])->ty;
        Type *vt = resolve_type_in_module(rec->owner, fta);
        vec_push(ftys, vt);
        int64_t va = type_align(vt);
        align = align > va ? align : va;
        off = round_up_i64(off, va);
        vec_push(foff, (void *)(long)off);
        off += type_size(vt);
      }
      vec_push(&rec->var_poff, (void *)(long)(foff->n ? (long)foff->items[0] : 4));
      size = size > off ? size : off;
    }
    g_tenv = saved_env;
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

int64_t variant_payload_offset(Type *enum_t, int variant) {
  layout_rec(enum_t->rec);
  if ((size_t)variant < enum_t->rec->var_poff.n)
    return (long)enum_t->rec->var_poff.items[variant];
  return 4;
}

int64_t variant_field_offset(Type *enum_t, int variant, int field) {
  layout_rec(enum_t->rec);
  if ((size_t)variant < enum_t->rec->var_offsets.n) {
    Vec *foff = enum_t->rec->var_offsets.items[variant];
    if ((size_t)field < foff->n)
      return (long)foff->items[field];
  }
  return 4;
}

Type *variant_field_type(Type *enum_t, int variant, int field) {
  layout_rec(enum_t->rec);
  if ((size_t)variant < enum_t->rec->var_types.n) {
    Vec *ftys = enum_t->rec->var_types.items[variant];
    if ((size_t)field < ftys->n)
      return ftys->items[field];
  }
  return ty_err_;
}

const char *prelude_symbol(const char *name) {
  Sym *fn = prelude_module ? map_get(&prelude_module->syms, str_from(name)) : NULL;
  return fn ? sym_symbol(fn) : name;
}

bool ty_is_aggregate_t(Type *t) {
  switch (t->kind) {
  case TY_STRUCT: case TY_ENUM: case TY_ARRAY: case TY_SLICE: case TY_STRING:
  case TY_FN: // {code, env} closure pair
    return true;
  default:
    return false;
  }
}
