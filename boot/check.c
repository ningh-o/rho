// check.c — the checker. T1.4 names (module graph, symbols, scoping),
// T1.5 types (the eleven rules), T1.6 errors and folding.
#include "sem.h"

#include <dirent.h>
#include <sys/stat.h>

static Program *g_program; // the program under check
Program *g_program_ctx;    // alias visible to check2
Module *g_prelude_mod;     // completed by check_program
Module *g_entry_mod;

// ================================================================ types

Type *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16, *ty_u32,
    *ty_u64, *ty_usize, *ty_f32, *ty_f64, *ty_bool, *ty_string, *ty_unit;

void init_builtin_types(void);

static Type *new_type(TyKind k) {
  Type *t = arena_alloc(g_arena, sizeof(Type), 8);
  t->kind = k;
  return t;
}

void init_builtin_types(void) {
  ty_i8 = new_type(TY_I8);
  ty_i16 = new_type(TY_I16);
  ty_i32 = new_type(TY_I32);
  ty_i64 = new_type(TY_I64);
  ty_u8 = new_type(TY_U8);
  ty_u16 = new_type(TY_U16);
  ty_u32 = new_type(TY_U32);
  ty_u64 = new_type(TY_U64);
  ty_usize = new_type(TY_USIZE);
  ty_f32 = new_type(TY_F32);
  ty_f64 = new_type(TY_F64);
  ty_bool = new_type(TY_BOOL);
  ty_string = new_type(TY_STRING);
  ty_unit = new_type(TY_UNIT);
}

Type *make_type_public(TyKind k) { return new_type(k); }

Type *type_ptr(Type *elem) {
  Type *t = new_type(TY_PTR);
  t->base = elem;
  return t;
}
Type *type_slice(Type *elem) {
  Type *t = new_type(TY_SLICE);
  t->base = elem;
  return t;
}
Type *type_dyn(TraitDef *td) {
  Type *t = new_type(TY_DYN);
  t->tdef = td;
  return t;
}
Type *type_fn(FnSig *sig) {
  Type *t = new_type(TY_FN);
  t->sig = sig;
  return t;
}
Type *type_struct(StructDef *sd, Type **args, size_t nargs) {
  Type *t = new_type(TY_STRUCT);
  t->sdef = sd;
  t->args = args;
  t->nargs = nargs;
  return t;
}
Type *type_enum(EnumDef *ed, Type **args, size_t nargs) {
  Type *t = new_type(TY_ENUM);
  t->edef = ed;
  t->args = args;
  t->nargs = nargs;
  return t;
}
Type *type_param(const char *name) {
  Type *t = new_type(TY_PARAM);
  t->pname = name;
  return t;
}

bool type_eq(Type *a, Type *b) {
  if (a == b)
    return true;
  if (!a || !b || a->kind != b->kind)
    return false;
  switch (a->kind) {
  case TY_PTR:
  case TY_SLICE:
  case TY_WEAK:
    return type_eq(a->base, b->base);
  case TY_DYN:
    return a->tdef == b->tdef;
  case TY_FN: {
    if (a->sig->nparams != b->sig->nparams)
      return false;
    for (size_t i = 0; i < a->sig->nparams; i++)
      if (!type_eq(a->sig->params[i].ty, b->sig->params[i].ty))
        return false;
    return type_eq(a->sig->ret, b->sig->ret);
  }
  case TY_STRUCT:
    if (a->sdef != b->sdef || a->nargs != b->nargs)
      return false;
    if (!a->args || !b->args)
      return a->args == b->args; // template forms match only templates
    for (size_t i = 0; i < a->nargs; i++)
      if (!type_eq(a->args[i], b->args[i]))
        return false;
    return true;
  case TY_ENUM:
    if (a->edef != b->edef || a->nargs != b->nargs)
      return false;
    if (!a->args || !b->args)
      return a->args == b->args;
    for (size_t i = 0; i < a->nargs; i++)
      if (!type_eq(a->args[i], b->args[i]))
        return false;
    return true;
  case TY_PARAM:
    return a->pname == b->pname; // interned
  default:
    return true; // builtins are singletons
  }
}

const char *type_name(Type *t) {
  if (!t)
    return "<none>";
  switch (t->kind) {
  case TY_I8:
    return "i8";
  case TY_I16:
    return "i16";
  case TY_I32:
    return "i32";
  case TY_I64:
    return "i64";
  case TY_U8:
    return "u8";
  case TY_U16:
    return "u16";
  case TY_U32:
    return "u32";
  case TY_U64:
    return "u64";
  case TY_USIZE:
    return "usize";
  case TY_F32:
    return "f32";
  case TY_F64:
    return "f64";
  case TY_BOOL:
    return "bool";
  case TY_STRING:
    return "string";
  case TY_UNIT:
    return "unit";
  case TY_WEAK:
    return aprintf(g_arena, "weak[%s]", type_name(t->base));
  case TY_PTR:
    return aprintf(g_arena, "*%s", type_name(t->base));
  case TY_SLICE:
    return aprintf(g_arena, "[]%s", type_name(t->base));
  case TY_DYN:
    return aprintf(g_arena, "dyn %s", t->tdef->name);
  case TY_FN:
    return "fn";
  case TY_STRUCT:
    return t->sdef->name;
  case TY_ENUM:
    return t->edef->name;
  case TY_PARAM:
    return t->pname;
  }
  return "?";
}

bool type_is_int(Type *t) {
  return t->kind >= TY_I8 && t->kind <= TY_USIZE;
}
bool type_is_float(Type *t) { return t->kind == TY_F32 || t->kind == TY_F64; }
bool type_is_num(Type *t) { return type_is_int(t) || type_is_float(t); }
bool type_is_managed(Type *t) {
  switch (t->kind) {
  case TY_PTR:
  case TY_SLICE:
  case TY_STRING:
  case TY_DYN:
  case TY_WEAK:
    return true;
  default:
    return false;
  }
}

// ================================================================ symtab

static void symtab_init(SymTab *st) {
  st->cap = 64;
  st->slots = arena_alloc(g_arena, st->cap * sizeof(Sym *), 8);
  memset(st->slots, 0, st->cap * sizeof(Sym *));
  st->count = 0;
  st->order_head = st->order_tail = NULL;
}

static size_t sym_hash(const char *s) {
  size_t h = 5381;
  while (*s)
    h = h * 33 + (unsigned char)*s++;
  return h;
}

Sym *symtab_get(SymTab *st, const char *name) {
  size_t i = sym_hash(name) & (st->cap - 1);
  for (Sym *s = st->slots[i]; s; s = s->next)
    if (strcmp(s->name, name) == 0)
      return s;
  return NULL;
}

Sym *symtab_add(SymTab *st, const char *name) {
  if ((st->count + 1) * 4 >= st->cap * 3) {
    // grow: rehash into a bigger slot array (order chain untouched)
    size_t ncap = st->cap * 2;
    Sym **nslots = arena_alloc(g_arena, ncap * sizeof(Sym *), 8);
    memset(nslots, 0, ncap * sizeof(Sym *));
    for (size_t j = 0; j < st->cap; j++)
      for (Sym *s = st->slots[j]; s;) {
        Sym *nx = s->next;
        size_t i = sym_hash(s->name) & (ncap - 1);
        s->next = nslots[i];
        nslots[i] = s;
        s = nx;
      }
    st->slots = nslots;
    st->cap = ncap;
  }
  Sym *s = arena_alloc(g_arena, sizeof(Sym), 8);
  memset(s, 0, sizeof(Sym));
  s->name = name;
  size_t i = sym_hash(name) & (st->cap - 1);
  s->next = st->slots[i];
  st->slots[i] = s;
  st->count++;
  // creation-order chain (for deterministic iteration)
  if (st->order_tail)
    st->order_tail->order_next = s;
  else
    st->order_head = s;
  st->order_tail = s;
  return s;
}

size_t symtab_count(SymTab *st) { return st->count; }

// ================================================================ modules

Program *program_new(void) {
  Program *p = arena_alloc(g_arena, sizeof(Program), 8);
  memset(p, 0, sizeof(Program));
  vec_init(&p->sets, sizeof(SetOverride));
  return p;
}

static void program_add(Program *p, Module *m, Module *importer) {
  m->importer = importer;
  m->next = NULL;
  // append preserving load order
  if (!p->modules) {
    p->modules = m;
  } else {
    Module *t = p->modules;
    while (t->next)
      t = t->next;
    t->next = m;
  }
  p->nmodules++;
}

static Module *program_find(Program *p, const char *path) {
  for (Module *m = p->modules; m; m = m->next)
    if (strcmp(m->path, path) == 0)
      return m;
  return NULL;
}

static char *dir_of(Arena *a, const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return astrdup(a, ".");
  size_t n = (size_t)(slash - path);
  if (n == 0)
    return astrdup(a, "/");
  char *d = arena_alloc(a, n + 1, 1);
  memcpy(d, path, n);
  d[n] = 0;
  return d;
}

static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static bool dir_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Resolve one segment chain under a base directory. Returns:
//   0  not found under this base
//   1  file module  (out: path to .rho file)
//   2  package      (out: path to lib.rho)
// Ambiguity (dir has lib.rho AND sibling file module could match at a
// later base) is handled by the caller comparing bases.
static int resolve_under_base(Arena *a, const char *base, RefList *segs,
                              char **out) {
  char *cur = astrdup(a, base);
  for (size_t i = 0; i < reflist_len(segs); i++) {
    const char *seg = node_get(reflist_at(segs, i))->name;
    char *as_dir = aprintf(a, "%s/%s", cur, seg);
    char *as_file = aprintf(a, "%s/%s.rho", cur, seg);
    bool last = i == reflist_len(segs) - 1;
    if (last) {
      // file module wins only if no package facade exists; both = the
      // exactly-one-real-body law checked by the caller via two probes
      char *facade = aprintf(a, "%s/lib.rho", as_dir);
      if (file_exists(facade) && file_exists(as_file)) {
        *out = facade; // signal ambiguity: caller sees both via probes
        return 3;
      }
      if (file_exists(facade)) {
        *out = facade;
        return 2;
      }
      if (file_exists(as_file)) {
        *out = as_file;
        return 1;
      }
      return 0;
    }
    // intermediate segments descend into directories only
    if (dir_exists(as_dir)) {
      cur = as_dir;
      continue;
    }
    return 0;
  }
  return 0;
}

// Load (or find) the module a use-declaration names. Applies the two
// lookup bases in order, then std, and enforces the facade law.
static Module *resolve_use(Program *p, Module *importer, NodeRef use_r) {
  Node *u = node_get(use_r);
  RefList *segs = u->list;

  // std/ is the reserved directory (Phase 4 wires the real packages;
  // until then the name is reserved)
  const char *first = node_get(reflist_at(segs, 0))->name;
  if (strcmp(first, "std") == 0) {
    diag_at(DIAG_ERROR, importer->path, u->line, u->col,
            "'std' is reserved; std packages arrive with the std library "
            "(Phase 4)");
    return NULL;
  }

  const char *bases[2];
  bases[0] = dir_of(g_arena, importer->path);
  bases[1] = dir_of(g_arena, p->entry->path);
  for (int b = 0; b < 2; b++) {
    char *path = NULL;
    int r = resolve_under_base(g_arena, bases[b], segs, &path);
    if (r == 0)
      continue;
    if (r == 3) {
      diag_at(DIAG_ERROR, importer->path, u->line, u->col,
              "ambiguous module: both %s and a sibling file module "
              "provide '%s' (exactly one real body)",
              path, first);
      return NULL;
    }
    Module *found = program_find(p, path);
    if (!found) {
      found = module_load(g_arena, path);
      found->is_package = (r == 2);
      program_add(p, found, importer);
    }
    return found;
  }
  diag_at(DIAG_ERROR, importer->path, u->line, u->col,
          "unknown module '%s'", first);
  return NULL;
}

// load-time module preparation: collect symbols, resolve consts, fold
// dead branches — so a use inside a comptime-dead branch never loads
void module_prepare(Program *p, Module *m);

static Program *g_program_for_load;

static bool load_one_use(Module *m, NodeRef d) {
  Node *n = node_get(d);
  Module *t = resolve_use(g_program_for_load, m, d);
  if (!t)
    return true; // reported; keep walking for more diagnostics
  UseBind *ub = vec_push(&m->uses);
  ub->alias = n->name2 ? n->name2
                       : node_get(reflist_at(n->list,
                                             reflist_len(n->list) - 1))
                             ->name;
  ub->target = t;
  ub->decl = d;
  return true;
}


bool program_load_graph(Program *p, const char *entry_path) {
  g_program_for_load = p;
  // the prelude exists before ANY collection (load-time preparation
  // resolves ?T through it)
  if (!g_prelude_mod) {
    extern const char *prelude_src(void);
    g_prelude_mod = module_parse_src("<prelude>", prelude_src());
    program_add(p, g_prelude_mod, NULL);
    module_prepare(p, g_prelude_mod);
  }
  p->entry = module_load(g_arena, entry_path);
  program_add(p, p->entry, NULL);

  // BFS in load order (deterministic); each module is prepared (consts
  // folded, dead branches marked) before its uses are resolved
  for (Module *m = p->modules; m; m = m->next) {
    module_prepare(p, m);
    for_each_live_use(m, load_one_use);
  }
  return !g_had_error;
}

// ============================================================ prelude

extern const char *prelude_src(void); // prelude.c

// ============================================================ generics

static bool gscope_has(GScope *g, const char *name) {
  for (GScope *s = g; s; s = s->up)
    for (size_t i = 0; i < s->n; i++)
      if (strcmp(s->names[i], name) == 0)
        return true;
  return false;
}

// every generic parameter name of the type is in scope (the bare-name
// self-reference form is legal only then)
static bool gscope_has_all(GScope *g, const char **names, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!gscope_has(g, names[i]))
      return false;
  return true;
}

// ============================================================ resolve types

static Type *resolve_type(Module *m, NodeRef tr, GScope *g);

static Type *resolve_named(Program *p, Module *m, const char *name,
                           Type **args, size_t nargs, NodeRef tr, GScope *g) {
  Node *t = node_get(tr);
  // generic parameter?
  // (caller already checked via gscope; here: module types)
  Sym *sym = NULL;
  for (Module *mod = p->modules; mod; mod = mod->next) {
    Sym *s = symtab_get(mod->syms, name);
    if (s && (mod == m || mod == g_prelude_mod || s->pub)) {
      if (sym) {
        diag_at(DIAG_ERROR, m->path, t->line, t->col,
                "ambiguous type '%s' (visible from multiple modules)", name);
        return ty_i32;
      }
      sym = s;
    }
  }
  // root consts are not types; prelude enums are
  if (!sym) {
    diag_at(DIAG_ERROR, m->path, t->line, t->col, "unknown type '%s'",
            name);
    return ty_i32;
  }
  if (sym->kind == SYM_STRUCT) {
    StructDef *sd = sym->u.sdef;
    // a bare generic name inside its own method's signature (or a
    // recursive field) binds to the enclosing scope's parameters:
    // `self: *Pair` means Pair at the method's own A and B
    if (nargs == 0 && sd->ngparams > 0 && g &&
        gscope_has_all(g, sd->gparams, sd->ngparams)) {
      Type **pargs = arena_alloc(g_arena, sd->ngparams * sizeof(Type *), 8);
      for (size_t i = 0; i < sd->ngparams; i++)
        pargs[i] = type_param(sd->gparams[i]);
      return type_struct(sd, pargs, sd->ngparams);
    }
    if (nargs != sd->ngparams) {
      diag_at(DIAG_ERROR, m->path, t->line, t->col,
              "struct %s takes %zu generic argument(s), got %zu", name,
              sd->ngparams, nargs);
    }
    return type_struct(sd, args, nargs);
  }
  if (sym->kind == SYM_ENUM) {
    EnumDef *ed = sym->u.edef;
    if (nargs == 0 && ed->ngparams > 0 && g &&
        gscope_has_all(g, ed->gparams, ed->ngparams)) {
      Type **pargs = arena_alloc(g_arena, ed->ngparams * sizeof(Type *), 8);
      for (size_t i = 0; i < ed->ngparams; i++)
        pargs[i] = type_param(ed->gparams[i]);
      return type_enum(ed, pargs, ed->ngparams);
    }
    if (nargs != ed->ngparams) {
      diag_at(DIAG_ERROR, m->path, t->line, t->col,
              "enum %s takes %zu generic argument(s), got %zu", name,
              ed->ngparams, nargs);
    }
    return type_enum(ed, args, nargs);
  }
  diag_at(DIAG_ERROR, m->path, t->line, t->col,
          "'%s' is not a type (has kind %d)", name, (int)sym->kind);
  return ty_i32;
}

Type *resolve_type_pub(Module *m, NodeRef tr, GScope *g) {
  return resolve_type(m, tr, g);
}

static Type *resolve_type(Module *m, NodeRef tr, GScope *g) {
  if (tr == NO_REF)
    return ty_unit;
  Node *t = node_get(tr);
  switch (t->kind) {
  case NT_BUILTIN:
    if (strcmp(t->name, "i8") == 0) return ty_i8;
    if (strcmp(t->name, "i16") == 0) return ty_i16;
    if (strcmp(t->name, "i32") == 0) return ty_i32;
    if (strcmp(t->name, "i64") == 0) return ty_i64;
    if (strcmp(t->name, "u8") == 0) return ty_u8;
    if (strcmp(t->name, "u16") == 0) return ty_u16;
    if (strcmp(t->name, "u32") == 0) return ty_u32;
    if (strcmp(t->name, "u64") == 0) return ty_u64;
    if (strcmp(t->name, "usize") == 0) return ty_usize;
    if (strcmp(t->name, "f32") == 0) return ty_f32;
    if (strcmp(t->name, "f64") == 0) return ty_f64;
    if (strcmp(t->name, "bool") == 0) return ty_bool;
    if (strcmp(t->name, "string") == 0) return ty_string;
    break;
  case NT_PTR:
    return type_ptr(resolve_type(m, t->a, g));
  case NT_OPT: {
    Type *inner = resolve_type(m, t->a, g);
    // ?T is Option[T] sugar: build the prelude enum instantiation
    if (!g_prelude_mod)
      return inner;
    Sym *os = symtab_get(g_prelude_mod->syms, "Option");
    if (!os || os->kind != SYM_ENUM)
      return inner;
    Type **args = arena_alloc(g_arena, sizeof(Type *), 8);
    args[0] = inner;
    Type *r = type_enum(os->u.edef, args, 1);
    r->is_opt = true;
    return r;
  }
  case NT_SLICE:
    return type_slice(resolve_type(m, t->a, g));
  case NT_DYN: {
    // dyn Trait: resolve the trait
    for (Module *mod = g_program->modules; mod; mod = mod->next) {
      Sym *s = symtab_get(mod->syms, t->name);
      if (s && s->kind == SYM_TRAIT && (mod == m || s->pub))
        return type_dyn(s->u.tdef);
    }
    diag_at(DIAG_ERROR, m->path, t->line, t->col, "unknown trait '%s'",
            t->name);
    return ty_i32;
  }
  case NT_FNTYPE: {
    FnSig *sig = arena_alloc(g_arena, sizeof(FnSig), 8);
    sig->nparams = reflist_len(t->list);
    sig->params = arena_alloc(g_arena, sig->nparams * sizeof(ParamDef), 8);
    for (size_t i = 0; i < sig->nparams; i++) {
      sig->params[i].name = NULL;
      sig->params[i].ty = resolve_type(m, reflist_at(t->list, i), g);
      sig->params[i].variadic = false;
    }
    sig->ret = resolve_type(m, t->a, g);
    return type_fn(sig);
  }
  case NT_APP: {
    if (strcmp(t->name, "weak") == 0 && t->list &&
        reflist_len(t->list) == 1) {
      Type *w = new_type(TY_WEAK);
      w->base = resolve_type(m, reflist_at(t->list, 0), g);
      return w;
    }
    if (gscope_has(g, t->name))
      return type_param(t->name);
    size_t nargs = t->list ? reflist_len(t->list) : 0;
    Type **args = nargs ? arena_alloc(g_arena, nargs * sizeof(Type *), 8)
                        : NULL;
    for (size_t i = 0; i < nargs; i++)
      args[i] = resolve_type(m, reflist_at(t->list, i), g);
    return resolve_named(g_program, m, t->name, args, nargs, tr, g);
  }
  default:
    break;
  }
  diag_at(DIAG_ERROR, m->path, t->line, t->col, "invalid type syntax");
  return ty_i32;
}

// ============================================================ collection

static void collect_module(Program *p, Module *m) {
  (void)p;
  m->syms = arena_alloc(g_arena, sizeof(SymTab), 8);
  symtab_init(m->syms);

  // pass 1: types (struct/enum/trait)
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    NodeRef dr = reflist_at(m->decls, i);
    Node *d = node_get(dr);
    if (d->kind == NT_STRUCT) {
      Sym *ex = symtab_get(m->syms, d->name);
      if (ex) {
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "duplicate definition of '%s' in module %s", d->name,
                m->name);
        continue;
      }
      Sym *s = symtab_add(m->syms, d->name);
      s->kind = SYM_STRUCT;
      s->pub = d->bval;
      StructDef *sd = arena_alloc(g_arena, sizeof(StructDef), 8);
      memset(sd, 0, sizeof(StructDef));
      sd->name = d->name;
      sd->decl = dr;
      sd->mod = m;
      s->u.sdef = sd;
      // generic params
      if (d->a != NO_REF)
        sd->ngparams = reflist_len(node_get(d->a)->list);
    } else if (d->kind == NT_ENUM) {
      Sym *ex = symtab_get(m->syms, d->name);
      if (ex) {
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "duplicate definition of '%s' in module %s", d->name,
                m->name);
        continue;
      }
      Sym *s = symtab_add(m->syms, d->name);
      s->kind = SYM_ENUM;
      s->pub = d->bval;
      EnumDef *ed = arena_alloc(g_arena, sizeof(EnumDef), 8);
      memset(ed, 0, sizeof(EnumDef));
      ed->name = d->name;
      ed->decl = dr;
      ed->mod = m;
      ed->is_option = strcmp(d->name, "Option") == 0;
      ed->is_result = strcmp(d->name, "Result") == 0;
      s->u.edef = ed;
      if (d->a != NO_REF)
        ed->ngparams = reflist_len(node_get(d->a)->list);
    } else if (d->kind == NT_TRAIT) {
      Sym *ex = symtab_get(m->syms, d->name);
      if (!ex)
        ex = symtab_add(m->syms, d->name);
      if (ex->kind != 0 && ex->kind != SYM_TRAIT) {
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "duplicate definition of '%s' in module %s", d->name,
                m->name);
        continue;
      }
      ex->kind = SYM_TRAIT;
      ex->pub = d->bval;
      TraitDef *td = arena_alloc(g_arena, sizeof(TraitDef), 8);
      memset(td, 0, sizeof(TraitDef));
      td->name = d->name;
      td->decl = dr;
      td->mod = m;
      ex->u.tdef = td;
    }
  }

  // pass 2: fill struct fields / enum variants / trait sigs (types may
  // reference each other within the module)
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    NodeRef dr = reflist_at(m->decls, i);
    Node *d = node_get(dr);
    if (d->kind == NT_STRUCT) {
      Sym *s = symtab_get(m->syms, d->name);
      StructDef *sd = s->u.sdef;
      // generics scope
      GScope g = {0};
      if (d->a != NO_REF) {
        RefList *gps = node_get(d->a)->list;
        g.n = reflist_len(gps);
        g.names = arena_alloc(g_arena, g.n * sizeof(char *), 8);
        for (size_t j = 0; j < g.n; j++)
          g.names[j] = node_get(reflist_at(gps, j))->name;
        sd->gparams = g.names; // cached for method-implicit binding
      }
      sd->nfields = reflist_len(d->list);
      sd->fields = arena_alloc(g_arena, (sd->nfields ? sd->nfields : 1) *
                                            sizeof(FieldDef), 8);
      for (size_t j = 0; j < sd->nfields; j++) {
        Node *f = node_get(reflist_at(d->list, j));
        sd->fields[j].name = f->name;
        sd->fields[j].decl = reflist_at(d->list, j);
        sd->fields[j].ty = resolve_type(m, f->a, &g);
      }
    } else if (d->kind == NT_ENUM) {
      Sym *s = symtab_get(m->syms, d->name);
      EnumDef *ed = s->u.edef;
      GScope g = {0};
      if (d->a != NO_REF) {
        RefList *gps = node_get(d->a)->list;
        g.n = reflist_len(gps);
        g.names = arena_alloc(g_arena, g.n * sizeof(char *), 8);
        for (size_t j = 0; j < g.n; j++)
          g.names[j] = node_get(reflist_at(gps, j))->name;
        ed->gparams = g.names; // cached for method-implicit binding
      }
      ed->nvariants = reflist_len(d->list);
      ed->variants = arena_alloc(
          g_arena, (ed->nvariants ? ed->nvariants : 1) * sizeof(EnumVariant), 8);
      for (size_t j = 0; j < ed->nvariants; j++) {
        Node *v = node_get(reflist_at(d->list, j));
        ed->variants[j].name = v->name;
        ed->variants[j].form = v->op;
        ed->variants[j].tag = (int)j; // declared order, i32
        ed->variants[j].decl = reflist_at(d->list, j);
        size_t nf = reflist_len(v->list);
        ed->variants[j].nfields = nf;
        ed->variants[j].fields = arena_alloc(
            g_arena, (nf ? nf : 1) * sizeof(FieldDef), 8);
        for (size_t k = 0; k < nf; k++) {
          Node *pf = node_get(reflist_at(v->list, k));
          if (v->op == VAR_TUPLE) {
            // positional payload: synthesized names "0", "1", …
            ed->variants[j].fields[k].name =
                aprintf(g_arena, "%zu", k);
            ed->variants[j].fields[k].ty = resolve_type(m, reflist_at(v->list, k), &g);
            ed->variants[j].fields[k].decl = reflist_at(v->list, k);
          } else {
            ed->variants[j].fields[k].name = pf->name;
            ed->variants[j].fields[k].decl = reflist_at(v->list, k);
            ed->variants[j].fields[k].ty = resolve_type(m, pf->a, &g);
          }
        }
      }
    } else if (d->kind == NT_TRAIT) {
      Sym *s = symtab_get(m->syms, d->name);
      TraitDef *td = s->u.tdef;
      GScope g = {0};
      td->nsigs = reflist_len(d->list);
      td->sigs =
          arena_alloc(g_arena, (td->nsigs ? td->nsigs : 1) * sizeof(FnSig), 8);
      for (size_t j = 0; j < td->nsigs; j++) {
        Node *fn = node_get(reflist_at(d->list, j));
        FnSig *sig = &td->sigs[j];
        sig->nparams = reflist_len(fn->list);
        sig->params = arena_alloc(g_arena,
                                  (sig->nparams ? sig->nparams : 1) *
                                      sizeof(ParamDef), 8);
        for (size_t k = 0; k < sig->nparams; k++) {
          Node *pp = node_get(reflist_at(fn->list, k));
          sig->params[k].name = pp->name;
          sig->params[k].decl = reflist_at(fn->list, k);
          if (pp->op == 1) {
            // bare self: the implementing method supplies the receiver
            sig->params[k].ty = NULL;
          } else {
            sig->params[k].ty = resolve_type(m, pp->a, &g);
          }
          sig->params[k].variadic = pp->op == 2;
        }
        sig->ret = fn->c != NO_REF ? resolve_type(m, fn->c, &g) : ty_unit;
      }
    }
  }
}

// pass 3: consts, statics, externs, fn signatures
static void collect_one_fn(Program *p, Module *m, NodeRef dr);

static void collect_fns(Program *p, Module *m) {
  (void)p;
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    NodeRef dr = reflist_at(m->decls, i);
    Node *d = node_get(dr);
    if (d->kind == NT_IMPL) {
      // impl members are methods of the impl's target type (the trait
      // names the PROTOCOL, the target names the receiver)
      Node *tgt = d->b != NO_REF ? node_get(d->b) : NULL;
      const char *tn = tgt ? tgt->name : NULL;
      for (size_t k = 0; k < reflist_len(d->list); k++) {
        NodeRef fr = reflist_at(d->list, k);
        Node *f = node_get(fr);
        if (f->kind != NT_FN || !tn)
          continue;
        f->op = 1; // a method decl from here on
        f->name2 = tn;
        collect_one_fn(p, m, fr);
      }
      continue;
    }
    if (d->kind != NT_CONST && d->kind != NT_STATIC && d->kind != NT_EXTERN &&
        d->kind != NT_FN)
      continue;
    collect_one_fn(p, m, dr);
  }
}

static void collect_one_fn(Program *p, Module *m, NodeRef dr) {
  Node *d = node_get(dr);
  if (d->kind == NT_CONST) {
    Sym *ex = symtab_get(m->syms, d->name);
    if (ex) {
      diag_at(DIAG_ERROR, m->path, d->line, d->col,
              "duplicate definition of '%s' in module %s", d->name, m->name);
      return;
    }
    Sym *s = symtab_add(m->syms, d->name);
    s->kind = SYM_CONST;
    s->pub = d->bval;
    ConstDef *cd = arena_alloc(g_arena, sizeof(ConstDef), 8);
    memset(cd, 0, sizeof(ConstDef));
    cd->name = d->name;
    cd->init = d->b;
    cd->decl = dr;
    cd->mod = m;
    cd->is_root = (m == p->entry);
    s->u.konst = cd;
    // type resolved during checking (const inference, T1.6)
    return;
  }
  if (d->kind == NT_STATIC) {
    Sym *ex = symtab_get(m->syms, d->name);
    if (ex) {
      diag_at(DIAG_ERROR, m->path, d->line, d->col,
              "duplicate definition of '%s' in module %s", d->name, m->name);
      return;
    }
    Sym *s = symtab_add(m->syms, d->name);
    s->kind = SYM_STATIC;
    s->pub = false;
    // statics carry their own type slot; store in a ConstDef-shaped
    // holder reusing the same field layout
    ConstDef *cd = arena_alloc(g_arena, sizeof(ConstDef), 8);
    memset(cd, 0, sizeof(ConstDef));
    cd->name = d->name;
    cd->init = d->b;
    cd->mod = m;
    s->u.konst = cd;
    cd->ty = resolve_type(m, d->a, NULL);
    return;
  }
  if (d->kind == NT_EXTERN) {
    Sym *s = symtab_get(m->syms, d->name);
    if (!s)
      s = symtab_add(m->syms, d->name);
    s->kind = SYM_EXTERN;
    s->pub = d->bval;
    return;
  }
  if (d->kind == NT_FN) {
    // overloads: same name allowed with different signatures;
    // exact-duplicate signatures are an error
    Sym *s = symtab_get(m->syms, d->name);
    if (s && s->kind != SYM_FN) {
      diag_at(DIAG_ERROR, m->path, d->line, d->col,
              "duplicate definition of '%s' in module %s", d->name, m->name);
      return;
    }
    FnDef *fd = arena_alloc(g_arena, sizeof(FnDef), 8);
    memset(fd, 0, sizeof(FnDef));
    fd->name = d->name;
    fd->mod = m;
    fd->decl = dr;
    fd->is_pub = d->bval;
    fd->body = d->d;
    fd->is_method = d->op == 1 && reflist_len(d->list) > 0 &&
                    strcmp(node_get(reflist_at(d->list, 0))->name, "self") ==
                        0;
    fd->is_assoc = d->op == 1 && !fd->is_method;
    fd->recv = d->name2;
    // generic params
    if (d->a != NO_REF) {
      RefList *gps = node_get(d->a)->list;
      fd->ngparams = reflist_len(gps);
      fd->gparams = arena_alloc(g_arena, fd->ngparams * sizeof(char *), 8);
      for (size_t j = 0; j < fd->ngparams; j++)
        fd->gparams[j] = node_get(reflist_at(gps, j))->name;
    }
    if (!s) {
      s = symtab_add(m->syms, d->name);
      s->kind = SYM_FN;
      s->pub = d->bval;
      s->u.fns = fd;
    } else {
      // append to overload chain; signature dup checked in T1.5
      FnDef *t = s->u.fns;
      while (t->next_overload)
        t = t->next_overload;
      t->next_overload = fd;
    }
  }
}

void module_prepare(Program *p, Module *m) {
  if (m->prepared)
    return;
  g_program = p;
  collect_module(p, m);
  collect_fns(p, m);
  const_resolve_module_pub(m);
  prune_dead_uses(m);
  m->prepared = true;
}

bool check_program(Program *p) {
  g_program = p;
  g_program_ctx = p;

  // the prelude module was created at graph-load time
  g_entry_mod = p->entry;

  // collect: prelude first so its types exist for everyone; modules
  // prepared at load time are skipped
  for (Module *m = p->modules; m; m = m->next) {
    if (!m->prepared)
      collect_module(p, m);
  }
  for (Module *m = p->modules; m; m = m->next) {
    if (m->prepared)
      continue; // fns collected at load time
    collect_fns(p, m);
  }

  // --set overrides land after inference, before bodies see them
  {
    extern int sets_apply(Program *p);
    extern bool g_set_refused;
    if (sets_apply(p) != 0) {
      g_set_refused = true;
      return false;
    }
  }

  // fn signatures (needs struct/enum/trait tables complete)
  for (Module *m = p->modules; m; m = m->next) {
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN)
        continue;
      for (FnDef *fd = s->u.fns; fd; fd = fd->next_overload) {
        Node *d = node_get(fd->decl);
        // a method/assoc fn on a GENERIC type implicitly carries the
        // type's own generic parameters (bound from each receiver use):
        // `fn Pair.swap(self: *Pair)` is generic over Pair's A and B
        if ((fd->is_method || fd->is_assoc) && fd->recv) {
          for (Module *tm = p->modules; tm; tm = tm->next) {
            Sym *ts = symtab_get(tm->syms, fd->recv);
            if (!ts || !(tm == m || ts->pub))
              continue;
            size_t tn = ts->kind == SYM_STRUCT  ? ts->u.sdef->ngparams
                        : ts->kind == SYM_ENUM ? ts->u.edef->ngparams
                                               : 0;
            const char **tgp =
                ts->kind == SYM_STRUCT  ? ts->u.sdef->gparams
                : ts->kind == SYM_ENUM ? ts->u.edef->gparams
                                       : NULL;
            if (tn == 0 || !tgp)
              break;
            // prepend the type's params, dedup against the fn's own
            size_t own = fd->ngparams;
            size_t extra = 0;
            for (size_t i = 0; i < tn; i++) {
              bool dup = false;
              for (size_t j = 0; j < own; j++)
                if (strcmp(fd->gparams[j], tgp[i]) == 0)
                  dup = true;
              for (size_t j = 0; j < i; j++)
                if (strcmp(tgp[j], tgp[i]) == 0)
                  dup = true;
              if (!dup)
                extra++;
            }
            if (extra) {
              const char **ng = arena_alloc(
                  g_arena, (extra + own) * sizeof(char *), 8);
              size_t k = 0;
              for (size_t i = 0; i < tn; i++) {
                bool dup = false;
                for (size_t j = 0; j < own; j++)
                  if (strcmp(fd->gparams[j], tgp[i]) == 0)
                    dup = true;
                for (size_t j = 0; j < i; j++)
                  if (strcmp(tgp[j], tgp[i]) == 0)
                    dup = true;
                if (!dup)
                  ng[k++] = tgp[i];
              }
              for (size_t j = 0; j < own; j++)
                ng[k++] = fd->gparams[j];
              fd->nimplicit = k - own;
              fd->gparams = ng;
              fd->ngparams = k;
            }
            break;
          }
        }
        GScope g = {0};
        if (fd->ngparams) {
          g.n = fd->ngparams;
          g.names = fd->gparams;
        }
        FnSig *sig = arena_alloc(g_arena, sizeof(FnSig), 8);
        sig->nparams = reflist_len(d->list);
        sig->params = arena_alloc(
            g_arena, (sig->nparams ? sig->nparams : 1) * sizeof(ParamDef), 8);
        for (size_t k = 0; k < sig->nparams; k++) {
          Node *pp = node_get(reflist_at(d->list, k));
          sig->params[k].name = pp->name;
          sig->params[k].decl = reflist_at(d->list, k);
          sig->params[k].ty =
              pp->op == 1 ? NULL
                          : resolve_type(m, pp->a, &g);
          if (pp->op == 2 && sig->params[k].ty)
            sig->params[k].ty = type_slice(sig->params[k].ty); // []T
          sig->params[k].variadic = pp->op == 2;
        }
        sig->ret = d->c != NO_REF ? resolve_type(m, d->c, &g) : ty_unit;
        fd->sig = sig;
      }
    }
  }

  // bodies checked by check_bodies (T1.5/T1.6 drive it)
  {
    extern void check_impls(Program *p);
    check_impls(p);
    extern bool check_bodies(Program *p);
    check_bodies(p);
  }
  return !g_had_error;
}
