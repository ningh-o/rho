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
  // zero the whole struct: the type walks (ty_has_param and friends)
  // branch on base/args unconditionally, and recycled malloc pages
  // hand back garbage that used to be walked as pointers
  memset(t, 0, sizeof(Type));
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
                              size_t nsegs, char **out) {
  char *cur = astrdup(a, base);
  for (size_t i = 0; i < nsegs; i++) {
    const char *seg = node_get(reflist_at(segs, i))->name;
    char *as_dir = aprintf(a, "%s/%s", cur, seg);
    char *as_file = aprintf(a, "%s/%s.rho", cur, seg);
    bool last = i == nsegs - 1;
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

static Type *resolve_type(Module *m, NodeRef tr, GScope *g);

static bool gscope_has(GScope *g, const char *name);
static Program *g_program_for_load;

// does the type expression mention a named type this module cannot
// see YET (its module loads later in the BFS)?
static bool type_needs_defer(Module *m, NodeRef tr, GScope *g) {
  if (tr == NO_REF)
    return false;
  Node *t = node_get(tr);
  switch (t->kind) {
  case NT_PTR:
  case NT_OPT:
  case NT_SLICE:
    return type_needs_defer(m, t->a, g);
  case NT_APP: {
    if (strcmp(t->name, "weak") == 0)
      return false;
    if (gscope_has(g, t->name))
      return false;
    if (m->syms && symtab_get(m->syms, t->name))
      return false;
    if (g_prelude_mod && g_prelude_mod->syms &&
        symtab_get(g_prelude_mod->syms, t->name))
      return false;
    // already-loaded foreign modules may hold it — only defer names
    // NO loaded module exposes publicly
    for (Module *mod = g_program_for_load->modules; mod; mod = mod->next) {
      Sym *s = mod->syms ? symtab_get(mod->syms, t->name) : NULL;
      if (s && (s->pub || mod == m))
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

// deferred field types: cross-module names whose module loads later
// in the BFS retry once the graph is complete
typedef struct DeferredTy {
  Module *m;
  NodeRef tr;     // the type node
  Type **slot;    // where the resolved type lands
  Node *at;       // for the diagnostic if it never resolves
} DeferredTy;
static Vec g_deferred_tys; // of DeferredTy

// Load (or find) the module a use-declaration names. Applies the two
// lookup bases in order, then std, and enforces the facade law.
static Module *resolve_use(Program *p, Module *importer, NodeRef use_r,
                           size_t nsegs) {
  Node *u = node_get(use_r);
  RefList *segs = u->list;

  // std/ is the reserved directory (Phase 4 wires the real packages;
  // until then the name is reserved)
  const char *first = node_get(reflist_at(segs, 0))->name;
  (void)nsegs;
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
    int r = resolve_under_base(g_arena, bases[b], segs, nsegs, &path);
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

// the last component of a directory path (a package's given name)
static const char *path_stem(const char *dir) {
  const char *s = strrchr(dir, '/');
  return s ? s + 1 : dir;
}

// same directory: the importer is a sibling (inside the package)
static bool importer_inside(Module *importer, Module *found) {
  return strcmp(dir_of(g_arena, importer->path),
                dir_of(g_arena, found->path)) == 0;
}

// the import-collision law (§2): one name, one binding — against the
// module's own declarations and against earlier import bindings; the
// error names the colliding name at the import's site
static bool use_collides(Module *m, Node *u, const char *alias) {
  Sym *there = m->syms ? symtab_get(m->syms, alias) : NULL;
  if (there && there->imported) {
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "two import bindings of '%s'", alias);
    return true;
  }
  if (there) {
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "the import '%s' collides with an existing name in this "
            "module",
            alias);
    return true;
  }
  for (size_t k = 0; k < VLEN(m->uses); k++) {
    UseBind *ub = VAT(m->uses, UseBind, k);
    if (strcmp(ub->alias, alias) == 0) {
      Node *u0 = ub->decl ? node_get(ub->decl) : NULL;
      if (u0)
        diag_at(DIAG_ERROR, m->path, u->line, u->col,
                "two import bindings of '%s' (the other at line %zu)",
                alias, u0->line);
      else
        diag_at(DIAG_ERROR, m->path, u->line, u->col,
                "two import bindings of '%s'", alias);
      return true;
    }
  }
  return false;
}

// reasons a module probe can end with
enum { PR_FOUND = 0, PR_UNKNOWN, PR_AMBIG, PR_REFUSED, PR_STD };

// the two-base module probe behind resolve_use, callable quietly: the
// caller owns every message. When loud is set the shape errors (std,
// facade ambiguity, package interior) are diagnosed here — exactly
// once per use, by whichever pass probes the path first.
static int probe_module_path(Program *p, Module *importer, Node *u,
                             RefList *segs, size_t nsegs, bool loud,
                             Module **out) {
  *out = NULL;
  const char *first = node_get(reflist_at(segs, 0))->name;
  if (strcmp(first, "std") == 0) {
    if (loud)
      diag_at(DIAG_ERROR, importer->path, u->line, u->col,
              "'std' is reserved; std packages arrive with the std "
              "library (Phase 4)");
    return PR_STD;
  }
  const char *bases[2];
  bases[0] = dir_of(g_arena, importer->path);
  bases[1] = dir_of(g_arena, p->entry->path);
  for (int b = 0; b < 2; b++) {
    char *path = NULL;
    int r = resolve_under_base(g_arena, bases[b], segs, nsegs, &path);
    if (r == 0)
      continue;
    if (r == 3) {
      if (loud)
        diag_at(DIAG_ERROR, importer->path, u->line, u->col,
                "ambiguous module: both %s and a sibling file module "
                "provide '%s' (exactly one real body)",
                path, first);
      return PR_AMBIG;
    }
    Module *found = program_find(p, path);
    if (!found) {
      found = module_load(g_arena, path);
      found->is_package = (r == 2);
      program_add(p, found, importer);
    }
    // the package interior is closed from outside (§4): a file inside
    // a package directory is importable only by the facade and its
    // siblings; the facade itself (r == 2) crosses freely
    if (!found->is_package && !importer_inside(importer, found)) {
      char *dp = dir_of(g_arena, found->path);
      char *facade = aprintf(g_arena, "%s/lib.rho", dp);
      if (file_exists(facade)) {
        if (loud)
          diag_at(DIAG_ERROR, importer->path, u->line, u->col,
                  "'%s' is interior to package '%s': import the "
                  "facade (use %s;) and reach it qualified",
                  found->path, path_stem(dp), path_stem(dp));
        return PR_REFUSED;
      }
    }
    *out = found;
    return PR_FOUND;
  }
  return PR_UNKNOWN;
}

// a dotted seg-list for diagnostics ("lex.token")
static char *segs_text(Arena *a, RefList *segs, size_t n) {
  char *s = NULL;
  for (size_t i = 0; i < n; i++) {
    const char *seg = node_get(reflist_at(segs, i))->name;
    s = s ? aprintf(a, "%s.%s", s, seg) : aprintf(a, "%s", seg);
  }
  return s ? s : (char *)"";
}

// load-time module preparation: collect symbols, resolve consts, fold
// dead branches — so a use inside a comptime-dead branch never loads.
// The unpruned variant stops before the fold (the load BFS applies
// --set overrides between the entry's resolve and its prune)
void module_prepare(Program *p, Module *m);
static void module_prepare_unpruned(Program *p, Module *m);

static Program *g_program_for_load;

static bool load_one_use(Module *m, NodeRef d) {
  Node *n = node_get(d);
  int form = n->op & 7;
  if (form == USE_BRACE)
    return true; // §4.4 sugar — the item pass expands it after the BFS
  const char *alias =
      n->name2 ? n->name2
               : node_get(reflist_at(n->list, reflist_len(n->list) - 1))
                     ->name;
  if (form == USE_PUB_ITEM || form == USE_PUB_AS) {
    // an item re-export: the owner is the module named by the prefix —
    // either a binding this module already holds, or a path to load;
    // no use binding (the facade exports the item, not the module)
    if (reflist_len(n->list) > 1) {
      const char *mod0 = node_get(reflist_at(n->list, 0))->name;
      bool bound = false;
      for (size_t k = 0; k < VLEN(m->uses); k++)
        if (strcmp(VAT(m->uses, UseBind, k)->alias, mod0) == 0)
          bound = true;
      if (!bound)
        resolve_use(g_program_for_load, m, d, reflist_len(n->list) - 1);
    }
    return true;
  }
  // a pub use may name a module already bound privately in this
  // module (use inner.core as core; pub use core;) — the binding, not
  // the filesystem, is the target
  if (form == USE_PUB_MOD || form == USE_PUB_STAR) {
    for (size_t k = 0; k < VLEN(m->uses); k++) {
      UseBind *ub0 = VAT(m->uses, UseBind, k);
      if (strcmp(ub0->alias, alias) == 0) {
        UseBind *ub = vec_push(&m->uses);
        ub->alias = alias;
        ub->target = ub0->target;
        ub->decl = d;
        return true;
      }
    }
  }
  size_t nsegs = reflist_len(n->list);
  if (form == USE_PLAIN && nsegs >= 2) {
    // module first (§2): a quiet probe — when no module matches, the
    // item pass owns the final segment after the BFS. Shape errors
    // (std, facade ambiguity, package interior) report right here.
    Module *t = NULL;
    probe_module_path(g_program_for_load, m, n, n->list, nsegs, true,
                      &t);
    if (!t)
      return true; // an item candidate — or already reported
    if (use_collides(m, n, alias))
      return true;
    UseBind *ub = vec_push(&m->uses);
    ub->alias = alias;
    ub->target = t;
    ub->decl = d;
    return true;
  }
  Module *t = resolve_use(g_program_for_load, m, d, nsegs);
  if (!t)
    return true; // reported; keep walking for more diagnostics
  if (use_collides(m, n, alias))
    return true;
  UseBind *ub = vec_push(&m->uses);
  ub->alias = alias;
  ub->target = t;
  ub->decl = d;
  return true;
}


// copy one pub symbol into a facade's namespace (re-export); the
// facade's own declarations win; the copy shares the underlying def
static void reexport_sym(Module *facade, const char *as, Sym *s,
                         Node *u) {
  if (!s || !s->pub)
    return;
  if (symtab_get(facade->syms, as))
    return; // the facade's own name (or an earlier re-export) wins
  Sym *c = symtab_add(facade->syms, as);
  c->kind = s->kind;
  c->pub = true;
  c->u = s->u;
  (void)u;
}

// expand every live pub use in every module: the facade law (§10) —
// flatten re-exports the target's public items, item forms bring one
// (optionally renamed)
static void expand_pub_uses(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    if (!m->decls)
      continue;
    for (size_t i = 0; i < reflist_len(m->decls); i++) {
      NodeRef dr = reflist_at(m->decls, i);
      Node *u = node_get(dr);
      if (u->kind != NT_USE || (u->op & USE_DEAD))
        continue;
      int form = u->op & 7;
      if (form != USE_PUB_MOD && form != USE_PUB_ITEM &&
          form != USE_PUB_AS && form != USE_PUB_STAR)
        continue;
      if (!m->syms)
        continue;
      // find the binding this pub use created (load_one_use ran first)
      const char *alias = u->name2
                              ? u->name2
                              : node_get(reflist_at(
                                            u->list, reflist_len(u->list) - 1))
                                    ->name;
      Module *tgt = NULL;
      for (size_t k = 0; k < VLEN(m->uses); k++) {
        UseBind *ub = VAT(m->uses, UseBind, k);
        if (ub->decl == dr) {
          tgt = ub->target;
          alias = ub->alias;
          break;
        }
      }
      if (form == USE_PUB_ITEM || form == USE_PUB_AS) {
        // the target module is segs[0..n-1), the item is the last seg
        const char *item = node_get(reflist_at(
                                        u->list, reflist_len(u->list) - 1))
                               ->name;
        Module *owner = NULL;
        if (reflist_len(u->list) > 1) {
          const char *mod0 = node_get(reflist_at(u->list, 0))->name;
          for (size_t k = 0; k < VLEN(m->uses); k++) {
            UseBind *ub = VAT(m->uses, UseBind, k);
            if (strcmp(ub->alias, mod0) == 0)
              owner = ub->target;
          }
          if (!owner) {
            // a bare item re-export (no `use inner;` beside it): the
            // prefix was loaded at load_one_use — find it quietly
            Module *t2 = NULL;
            probe_module_path(p, m, u, u->list,
                              reflist_len(u->list) - 1, false, &t2);
            owner = t2;
          }
        } else
          owner = tgt; // one seg: item of the same path
        if (owner && owner->syms)
          reexport_sym(m, alias, symtab_get(owner->syms, item), u);
        continue;
      }
      // flatten (mod or star): every pub item of the target rides
      Module *src = NULL;
      for (size_t k = 0; k < VLEN(m->uses); k++) {
        UseBind *ub = VAT(m->uses, UseBind, k);
        if (ub->decl == dr)
          src = ub->target;
      }
      if (src && src->syms)
        for (Sym *s = src->syms->order_head; s; s = s->order_next)
          reexport_sym(m, s->name, s, u);
    }
  }
}

// ==================================================== item imports

// bind one imported item under `alias` in the importer's symbol table
// (a copy sharing the owner's def); the collision law names both
static void bind_imported_item(Module *m, Node *u, Module *owner,
                               const char *item, const char *alias) {
  Sym *s = owner->syms ? symtab_get(owner->syms, item) : NULL;
  if (!s) {
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "no item '%s' in %s", item, owner->path);
    return;
  }
  if (!s->pub) {
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "item '%s' of %s is not public", item, owner->path);
    return;
  }
  if (use_collides(m, u, alias))
    return;
  Sym *c = symtab_add(m->syms, alias);
  c->kind = s->kind;
  c->pub = false;
  c->imported = true;
  c->u = s->u;
}

// a module loaded after the BFS (a brace prefix, a facade probed for
// the ambiguity check) gets the turn the load loop would have given it
static void late_load_turn(Program *p, Module *m) {
  module_prepare_unpruned(p, m);
  prune_dead_uses(m);
  for_each_live_use(m, load_one_use);
}

// the final segment of one plain use (or one brace item): module
// first, then a public item of the module the prefix names (§2)
static void resolve_use_final(Program *p, Module *m, NodeRef use_r,
                              RefList *segs, const char *alias,
                              bool bfs_probed) {
  Node *u = node_get(use_r);
  size_t nsegs = reflist_len(segs);
  const char *item = node_get(reflist_at(segs, nsegs - 1))->name;

  // the module the full path names: the BFS binding for a plain use
  // (its probe already reported every shape error for this path), or
  // a loud probe for a brace item
  Module *mod = NULL;
  if (bfs_probed) {
    for (size_t k = 0; k < VLEN(m->uses); k++)
      if (VAT(m->uses, UseBind, k)->decl == use_r) {
        mod = VAT(m->uses, UseBind, k)->target;
        break;
      }
    if (!mod) {
      // the BFS probe said no quietly OR reported a shape error;
      // re-probe quietly to tell the two apart
      Module *t = NULL;
      int pr = probe_module_path(p, m, u, segs, nsegs, false, &t);
      if (pr != PR_UNKNOWN)
        return; // std / ambiguity / interior — reported at the BFS
    }
  } else {
    Module *t = NULL;
    int pr = probe_module_path(p, m, u, segs, nsegs, true, &t);
    if (pr == PR_FOUND) {
      mod = t;
      if (!mod->prepared)
        late_load_turn(p, mod);
    } else if (pr != PR_UNKNOWN) {
      return; // reported right here
    }
  }

  // the module the prefix names — a binding this module already holds
  // wins, then the filesystem (loud: shape errors surface here once)
  Module *owner = NULL;
  if (nsegs >= 2) {
    size_t npfx = nsegs - 1;
    if (npfx == 1) {
      const char *p0 = node_get(reflist_at(segs, 0))->name;
      for (size_t k = 0; k < VLEN(m->uses); k++)
        if (strcmp(VAT(m->uses, UseBind, k)->alias, p0) == 0) {
          owner = VAT(m->uses, UseBind, k)->target;
          break;
        }
    }
    if (!owner) {
      Module *t = NULL;
      int pr = probe_module_path(p, m, u, segs, npfx, true, &t);
      if (pr == PR_FOUND) {
        owner = t;
        if (!owner->prepared)
          late_load_turn(p, owner);
      } else if (pr != PR_UNKNOWN) {
        return; // reported right here
      }
    }
  }

  Sym *is = owner && owner->syms ? symtab_get(owner->syms, item) : NULL;
  bool item_pub = is && is->pub;

  // both bases match: the across-kinds ambiguity (§2/§3)
  if (mod && item_pub) {
    char *full = segs_text(g_arena, segs, nsegs);
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "ambiguous use of '%s': both the module %s and the public "
            "item '%s' of %s match",
            full, mod->path, item, owner->path);
    return;
  }
  if (mod)
    return; // the module binding stands (made at the BFS)
  if (owner && owner->is_package && !importer_inside(m, owner)) {
    const char *dp = dir_of(g_arena, owner->path);
    diag_at(DIAG_ERROR, m->path, u->line, u->col,
            "items of package '%s' are reachable only through its "
            "facade: use %s; then qualify",
            path_stem(dp), path_stem(dp));
    return;
  }
  if (owner) {
    bind_imported_item(m, u, owner, item, alias);
    return;
  }
  diag_at(DIAG_ERROR, m->path, u->line, u->col,
          "unknown module or item '%s'", segs_text(g_arena, segs, nsegs));
}

// the item half of the module law (§2): plain uses whose final
// segment names a public item, and the brace form's expansion — run
// after the BFS so every possible owner is loaded and prepared
static void bind_item_imports(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    if (!m->decls)
      continue;
    for (size_t i = 0; i < reflist_len(m->decls); i++) {
      NodeRef dr = reflist_at(m->decls, i);
      Node *u = node_get(dr);
      if (u->kind != NT_USE || (u->op & USE_DEAD))
        continue;
      int form = u->op & 7;
      if (form == USE_PLAIN) {
        size_t nsegs = reflist_len(u->list);
        if (nsegs < 2)
          continue;
        const char *item =
            node_get(reflist_at(u->list, nsegs - 1))->name;
        resolve_use_final(p, m, dr, u->list,
                          u->name2 ? u->name2 : item, true);
      } else if (form == USE_BRACE) {
        if (!u->d)
          continue;
        Node *items = node_get(u->d);
        size_t nsegs = reflist_len(u->list);
        for (size_t j = 0; j < reflist_len(items->list); j++) {
          NodeRef it = reflist_at(items->list, j);
          Node *in = node_get(it);
          RefList *full = reflist();
          for (size_t si = 0; si < nsegs; si++)
            reflist_add(full, reflist_at(u->list, si));
          reflist_add(full, it);
          resolve_use_final(p, m, dr, full,
                            in->name2 ? in->name2 : in->name, false);
        }
      }
    }
  }
}

bool program_load_graph(Program *p, const char *entry_path) {
  g_program_for_load = p;
  // the prelude exists before ANY collection (load-time preparation
  // resolves ?T through it). It is a process-global singleton, but
  // EVERY program carries it in its module list — a verb run compiles
  // many programs in one process, and a program whose module list
  // lacks the prelude cannot resolve Show/Option/Result at all.
  if (!g_prelude_mod) {
    extern const char *prelude_src(void);
    g_prelude_mod = module_parse_src("<prelude>", prelude_src());
    module_prepare(p, g_prelude_mod);
  }
  program_add(p, g_prelude_mod, NULL);
  p->entry = module_load(g_arena, entry_path);
  program_add(p, p->entry, NULL);
  // the entry is the fold root from load time on: body folds during
  // the BFS read root consts through it
  g_entry_mod = p->entry;

  // BFS in load order (deterministic); each module is prepared (consts
  // folded, dead branches marked) before its uses are resolved. The
  // --set overrides land the moment the entry's consts exist — before
  // ANY body fold runs — so comptime conditions see the overridden
  // values, not the declared ones (§7)
  {
    extern int sets_apply(Program *p);
    extern bool g_set_refused;
    bool sets_done = false;
    for (Module *m = p->modules; m; m = m->next) {
      module_prepare_unpruned(p, m);
      if (!sets_done && m == p->entry) {
        if (sets_apply(p) != 0) {
          g_set_refused = true;
          return false;
        }
        sets_done = true;
      }
      prune_dead_uses(m);
      for_each_live_use(m, load_one_use);
    }
  }
  // facades re-export now — every target is loaded and prepared
  expand_pub_uses(p);
  // item imports bind last — every possible owner is prepared, so the
  // module-first-then-item law can see both candidates (§2)
  bind_item_imports(p);
  // deferred field types resolve against the complete graph
  for (size_t i = 0; i < VLEN(g_deferred_tys); i++) {
    DeferredTy *dt = VAT(g_deferred_tys, DeferredTy, i);
    GScope g2 = {0};
    Type *t = resolve_type(dt->m, dt->tr, &g2);
    if (t == ty_i32 && g_had_error == false) {
      Node *tn = node_get(dt->tr);
      diag_at(DIAG_ERROR, dt->m->path, dt->at->line, dt->at->col,
              "unknown type '%s'", tn->kind == NT_APP ? tn->name : "?");
    }
    *dt->slot = t;
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

static const char *sym_kind_word(SymKind k) {
  switch (k) {
  case SYM_FN: return "function";
  case SYM_STRUCT: return "struct";
  case SYM_ENUM: return "enum";
  case SYM_TRAIT: return "trait";
  case SYM_CONST: return "const";
  case SYM_STATIC: return "static";
  case SYM_EXTERN: return "extern";
  case SYM_MODULE: return "module";
  }
  return "value";
}

static Type *resolve_named(Program *p, Module *m, const char *name,
                           Type **args, size_t nargs, NodeRef tr, GScope *g) {
  Node *t = node_get(tr);
  // generic parameter?
  // (caller already checked via gscope; here: module types)
  Sym *sym = NULL;
  for (Module *mod = p->modules; mod; mod = mod->next) {
    if (!mod->syms)
      continue; // not collected yet (load order)
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
          "'%s' is not a type (it names a %s)", name, sym_kind_word(sym->kind));
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
        if (type_needs_defer(m, f->a, &g)) {
          if (!g_deferred_tys.data)
            vec_init(&g_deferred_tys, sizeof(DeferredTy));
          DeferredTy *dt = VPUSH(g_deferred_tys, DeferredTy);
          dt->m = m;
          dt->tr = f->a;
          dt->slot = &sd->fields[j].ty;
          dt->at = f;
          sd->fields[j].ty = ty_i32; // placeholder until the retry
          continue;
        }
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
          sig->params[k].is_mut = pp->bval; // `mut self` in the trait sig
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
static void collect_one_test(Program *p, Module *m, NodeRef dr);

// the bare receiver's type, derived from the method's home: *T for a
// struct/enum target (the type's own generic parameters ride as
// TY_PARAMs), the value itself for a builtin primitive; NULL when the
// receiver names no type in scope (existing NULL-typed behavior)
static Type *bare_self_type(Program *p, Module *m, const char *recv) {
  extern Type *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16,
      *ty_u32, *ty_u64, *ty_usize, *ty_f32, *ty_f64, *ty_bool,
      *ty_string;
  if (!strcmp(recv, "i8")) return ty_i8;
  if (!strcmp(recv, "i16")) return ty_i16;
  if (!strcmp(recv, "i32")) return ty_i32;
  if (!strcmp(recv, "i64")) return ty_i64;
  if (!strcmp(recv, "u8")) return ty_u8;
  if (!strcmp(recv, "u16")) return ty_u16;
  if (!strcmp(recv, "u32")) return ty_u32;
  if (!strcmp(recv, "u64")) return ty_u64;
  if (!strcmp(recv, "usize")) return ty_usize;
  if (!strcmp(recv, "f32")) return ty_f32;
  if (!strcmp(recv, "f64")) return ty_f64;
  if (!strcmp(recv, "bool")) return ty_bool;
  if (!strcmp(recv, "string")) return ty_string;
  for (Module *tm = p->modules; tm; tm = tm->next) {
    Sym *ts = tm->syms ? symtab_get(tm->syms, recv) : NULL;
    if (!ts || !(tm == m || ts->pub))
      continue;
    if (ts->kind == SYM_STRUCT) {
      StructDef *sd = ts->u.sdef;
      Type **args = NULL;
      if (sd->ngparams) {
        args = arena_alloc(g_arena, sd->ngparams * sizeof(Type *), 8);
        for (size_t i = 0; i < sd->ngparams; i++)
          args[i] = type_param(sd->gparams[i]);
      }
      return type_ptr(type_struct(sd, args, sd->ngparams));
    }
    if (ts->kind == SYM_ENUM) {
      EnumDef *ed = ts->u.edef;
      Type **args = NULL;
      if (ed->ngparams) {
        args = arena_alloc(g_arena, ed->ngparams * sizeof(Type *), 8);
        for (size_t i = 0; i < ed->ngparams; i++)
          args[i] = type_param(ed->gparams[i]);
      }
      return type_ptr(type_enum(ed, args, ed->ngparams));
    }
  }
  return NULL;
}

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
    if (d->kind == NT_TEST) {
      collect_one_test(p, m, dr);
      continue;
    }
    if (d->kind != NT_CONST && d->kind != NT_STATIC && d->kind != NT_EXTERN &&
        d->kind != NT_FN)
      continue;
    collect_one_fn(p, m, dr);
  }
}

// a test block is a synthesized void fn in its own module (§17): it
// sees everything the module sees (white-box) and is judged by panic
// versus clean return. The symbol name carries the 'test:' prefix so
// no user identifier can ever reach it.
static void collect_one_test(Program *p, Module *m, NodeRef dr) {
  (void)p;
  Node *d = node_get(dr);
  const char *sym = aprintf(g_arena, "test:%s", d->name);
  if (symtab_get(m->syms, sym)) {
    diag_at(DIAG_ERROR, m->path, d->line, d->col, "duplicate test '%s'",
            d->name);
    return;
  }
  Sym *s = symtab_add(m->syms, sym);
  s->kind = SYM_FN;
  s->pub = false;
  FnDef *fd = arena_alloc(g_arena, sizeof(FnDef), 8);
  memset(fd, 0, sizeof(FnDef));
  fd->name = sym;
  fd->mod = m;
  fd->decl = dr;
  fd->body = d->d;
  fd->sig = arena_alloc(g_arena, sizeof(FnSig), 8);
  fd->sig->params = NULL;
  fd->sig->nparams = 0;
  fd->sig->ret = ty_unit;
  s->u.fns = fd;
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
    cd->decl = dr;
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

// collect + const resolve WITHOUT the dead-branch fold: the load BFS
// applies --set overrides between the entry's resolve and its prune
// (the fold must see the overridden values, §7)
static void module_prepare_unpruned(Program *p, Module *m) {
  if (m->prepared)
    return;
  g_program = p;
  collect_module(p, m);
  collect_fns(p, m);
  const_resolve_module_pub(m);
  m->prepared = true;
}

// §5 layout law: a by-value cycle (struct S { s: S }, its A↔B mutual
// form) has no finite layout — reject at check, at the def. The walk
// follows resolved field types through value edges; an instance's own
// generics substitute into its fields and bare generic parameters are
// leaves. Depth cap plus visit budget bound what the walk can spend;
// a cycle hidden behind a generic instantiation still surfaces later
// at layout (emit's own guard).
#define SHAPE_WALK_CAP 1000
#define SHAPE_WALK_BUDGET 2000000
static size_t g_shape_visits;

// layout-compatible with check2.c's TBind (tsubst takes void*)
typedef struct ShapeBind {
  const char **names;
  Type **tys;
  size_t n;
} ShapeBind;

static bool shape_walk(Type *t, int depth, Module *m, NodeRef decl,
                       const char *name) {
  if (++g_shape_visits > SHAPE_WALK_BUDGET || depth > SHAPE_WALK_CAP) {
    Node *d = node_get(decl);
    diag_at(DIAG_ERROR, m->path, d->line, d->col,
            "type '%s' nests too deeply while checking layout (a "
            "recursive type without indirection has no finite layout)",
            name);
    return true;
  }
  if (t->kind == TY_STRUCT) {
    StructDef *sd = t->sdef;
    for (size_t i = 0; i < sd->nfields; i++) {
      Type *ft = sd->fields[i].ty;
      if (t->nargs > 0) {
        ShapeBind b = {sd->gparams, t->args,
                       t->nargs < sd->ngparams ? t->nargs : sd->ngparams};
        ft = tsubst(ft, &b);
      }
      if (shape_walk(ft, depth + 1, m, decl, name))
        return true;
    }
  } else if (t->kind == TY_ENUM) {
    EnumDef *ed = t->edef;
    for (size_t i = 0; i < ed->nvariants; i++) {
      for (size_t k = 0; k < ed->variants[i].nfields; k++) {
        Type *ft = ed->variants[i].fields[k].ty;
        if (t->nargs > 0) {
          ShapeBind b = {ed->gparams, t->args,
                         t->nargs < ed->ngparams ? t->nargs : ed->ngparams};
          ft = tsubst(ft, &b);
        }
        if (shape_walk(ft, depth + 1, m, decl, name))
          return true;
      }
    }
  }
  return false;
}

static void check_shape_cycles(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      Type *self = NULL;
      const char *name = NULL;
      NodeRef decl = NO_REF;
      if (s->kind == SYM_STRUCT) {
        self = type_struct(s->u.sdef, NULL, 0);
        name = s->u.sdef->name;
        decl = s->u.sdef->decl;
      } else if (s->kind == SYM_ENUM) {
        self = type_enum(s->u.edef, NULL, 0);
        name = s->u.edef->name;
        decl = s->u.edef->decl;
      } else {
        continue;
      }
      g_shape_visits = 0;
      shape_walk(self, 0, m, decl, name);
    }
  }
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

  // fn signatures (needs struct/enum/trait tables complete)
  for (Module *m = p->modules; m; m = m->next) {
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN)
        continue;
      for (FnDef *fd = s->u.fns; fd; fd = fd->next_overload) {
        // a test block's signature was built at collection time (no
        // params, unit return); its decl is not an NT_FN
        if (node_get(fd->decl)->kind == NT_TEST)
          continue;
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
          sig->params[k].is_mut = pp->bval;
          sig->params[k].ty =
              pp->op == 1 ? NULL
                          : resolve_type(m, pp->a, &g);
          if (pp->op == 2 && sig->params[k].ty)
            sig->params[k].ty = type_slice(sig->params[k].ty); // []T
          sig->params[k].variadic = pp->op == 2;
          // the mut view law (§18): `mut` marks a handle-typed view
          // parameter (*T, []T, string, dyn); on a value it is refused
          if (pp->bval && sig->params[k].ty &&
              sig->params[k].ty->kind != TY_PTR &&
              sig->params[k].ty->kind != TY_SLICE &&
              sig->params[k].ty->kind != TY_STRING &&
              sig->params[k].ty->kind != TY_DYN &&
              pp->op != 1) // bare self: the receiver's mut is decided
                           // by the receiver type, checked at the call
            diag_at(DIAG_ERROR, m->path, pp->line, pp->col,
                    "'mut' marks a view parameter (*T, []T, string, "
                    "dyn); '%s' is a value",
                    pp->name ? pp->name : "?");
        }
        sig->ret = d->c != NO_REF ? resolve_type(m, d->c, &g) : ty_unit;
        // a bare receiver (§18/T3.10): the type comes from the
        // method's home — *T for a struct/enum target, the value
        // itself for a builtin primitive. The fully-typed form stays
        // legal (accepted, never required); fmt canonicalizes it away
        if (fd->is_method && fd->recv && sig->nparams > 0 &&
            sig->params[0].ty == NULL)
          sig->params[0].ty = bare_self_type(p, m, fd->recv);
        fd->sig = sig;
      }
    }
  }

  // the layout law before any body is checked: reject value cycles
  // with the def's own location
  check_shape_cycles(p);

  // bodies checked by check_bodies (T1.5/T1.6 drive it)
  {
    extern void check_impls(Program *p);
    check_impls(p);
    extern bool check_bodies(Program *p);
    check_bodies(p);
  }
  return !g_had_error;
}
