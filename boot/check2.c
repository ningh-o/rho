// check2.c — the body checker (T1.4 scoping + T1.5 the eleven rules).
// Walks every function body: names (locals → module → root consts →
// prelude), types (consumer-typed literals, overloads, methods, ==),
// statements (shadowing, labels, assignment lvalues).
#include "sem.h"

#include <stdarg.h>

typedef struct Local {
  const char *name;
  Type *ty;
  bool mut;
  int scope; // inner-shadowing discipline works per scope id
  NodeRef decl;
} Local;

typedef struct FnCtx {
  FnDef *fn;
  Module *mod;
  Vec locals;   // of Local
  int scope;
  Type *ret;
  Vec labels;   // of const char* — labels are function-unique
  const char **gparams;
  size_t ngparams;
  Type **gbinds; // generic instance: name → concrete bind (parallel to
                 // gparams; NULL for templates)
} FnCtx;

static void ctx_push_scope(FnCtx *c) { c->scope++; }

static void ctx_pop_scope(FnCtx *c) {
  // drop locals of this scope (vec truncate)
  size_t keep = 0;
  for (size_t i = 0; i < VLEN(c->locals); i++)
    if (VAT(c->locals, Local, i)->scope < c->scope)
      keep++;
  c->locals.len = keep;
  c->scope--;
}

static Local *ctx_find_local(FnCtx *c, const char *name) {
  for (size_t i = VLEN(c->locals); i > 0; i--)
    if (strcmp(VAT(c->locals, Local, i - 1)->name, name) == 0)
      return VAT(c->locals, Local, i - 1);
  return NULL;
}

static Local *ctx_decl_local(FnCtx *c, const char *name) {
  Local *l = VPUSH(c->locals, Local);
  l->name = name;
  l->scope = c->scope;
  return l;
}

// ---------------------------------------------------------------- lookup

typedef enum {
  LOOK_NONE = 0,
  LOOK_LOCAL,
  LOOK_CONST,   // const (immutable, folded)
  LOOK_STATIC,  // static mut (module-lifetime, mutable)
  LOOK_FN,      // overload set
  LOOK_ENUM,
  LOOK_STRUCT,
  LOOK_TRAIT,
  LOOK_MODULE,  // use binding
  LOOK_EXTERN,
} LookKind;

typedef struct Look {
  LookKind kind;
  Local *local;
  ConstDef *konst;
  FnDef *fns;
  EnumDef *edef;
  StructDef *sdef;
  Module *module;
} Look;

// names resolve: locals → module syms → use bindings (public facades) →
// root build params → prelude
static bool lookup(FnCtx *c, const char *name, Look *out) {
  memset(out, 0, sizeof(*out));
  Local *l = ctx_find_local(c, name);
  if (l) {
    out->kind = LOOK_LOCAL;
    out->local = l;
    return true;
  }
  Sym *s = symtab_get(c->mod->syms, name);
  if (s) {
    switch (s->kind) {
    case SYM_FN:
      out->kind = LOOK_FN;
      out->fns = s->u.fns;
      return true;
    case SYM_STRUCT:
      out->kind = LOOK_STRUCT;
      out->sdef = s->u.sdef;
      return true;
    case SYM_ENUM:
      out->kind = LOOK_ENUM;
      out->edef = s->u.edef;
      return true;
    case SYM_CONST:
      out->kind = LOOK_CONST;
      out->konst = s->u.konst;
      return true;
    case SYM_STATIC:
      out->kind = LOOK_STATIC;
      out->konst = s->u.konst;
      return true;
    case SYM_MODULE:
      out->kind = LOOK_MODULE;
      out->module = s->u.module;
      return true;
    default:
      return false;
    }
  }
  // use bindings: a module bound under its alias (member syntax
  // reaches its public items)
  for (size_t i = 0; i < VLEN(c->mod->uses); i++) {
    UseBind *ub = VAT(c->mod->uses, UseBind, i);
    if (strcmp(ub->alias, name) == 0) {
      out->kind = LOOK_MODULE;
      out->module = ub->target;
      return true;
    }
  }
  // root build parameters (prelude status)
  if (g_entry_mod) {
    Sym *rs = symtab_get(g_entry_mod->syms, name);
    if (rs && (rs->kind == SYM_CONST || rs->kind == SYM_STATIC)) {
      out->kind = LOOK_CONST;
      out->konst = rs->u.konst;
      return true;
    }
  }
  // prelude last — only its PUBLIC names are the prelude (§14);
  // private helpers stay invisible to programs
  if (g_prelude_mod) {
    Sym *ps = symtab_get(g_prelude_mod->syms, name);
    if (ps && ps->pub) {
      switch (ps->kind) {
      case SYM_FN:
        out->kind = LOOK_FN;
        out->fns = ps->u.fns;
        return true;
      case SYM_ENUM:
        out->kind = LOOK_ENUM;
        out->edef = ps->u.edef;
        return true;
      case SYM_STRUCT:
        out->kind = LOOK_STRUCT;
        out->sdef = ps->u.sdef;
        return true;
      case SYM_CONST:
        out->kind = LOOK_CONST;
        out->konst = ps->u.konst;
        return true;
      default:
        return false;
      }
    }
  }
  return false;
}

// resolve a type node inside a function context (generic params of the
// enclosing fn are in scope); inside a generic INSTANCE the names map
// to the instance's concrete binds
static Type *check_type_in_ctx(FnCtx *c, NodeRef tr, GScope *g) {
  (void)g;
  extern Type *resolve_type_pub(Module *, NodeRef, GScope *);
  GScope gs = {c->gparams, c->ngparams, NULL};
  Type *t = resolve_type_pub(c->mod, tr, &gs);
  if (c->gbinds && t && t->kind == TY_PARAM) {
    for (size_t i = 0; i < c->ngparams; i++)
      if (strcmp(c->gparams[i], t->pname) == 0)
        return c->gbinds[i] ? c->gbinds[i] : t;
  }
  return t;
}

// ---------------------------------------------------------------- literals

static bool int_fits(uint64_t v, Type *t) {
  switch (t->kind) {
  case TY_I8:
    return v <= 0x7F;
  case TY_I16:
    return v <= 0x7FFF;
  case TY_I32:
    return v <= 0x7FFFFFFF;
  case TY_I64:
    return v <= 0x7FFFFFFFFFFFFFFFULL;
  case TY_U8:
    return v <= 0xFF;
  case TY_U16:
    return v <= 0xFFFF;
  case TY_U32:
    return v <= 0xFFFFFFFF;
  case TY_U64:
  case TY_USIZE:
    return true;
  default:
    return false;
  }
}

static bool literal_adapts_to(FnCtx *c, Node *e, Type *t) {
  (void)c;
  if (!t)
    return false;
  if (e->kind == NT_UNARY && e->op == OP_NEG) {
    // -literal adapts THROUGH the negation: the magnitude may reach
    // MIN, one past max (128 fits i8 here — the fold negates it)
    Node *op0 = node_get(e->a);
    if (op0->kind == NT_FLOAT)
      return type_is_float(t);
    if (op0->kind != NT_INT)
      return false;
    if (!type_is_int(t))
      return false; // negative ints never adapt to unsigned targets
    if (op0->ival == 0)
      return int_fits(0, t); // -0
    return int_fits(op0->ival - 1, t);
  }
  if (e->kind == NT_INT) {
    if (type_is_int(t))
      return int_fits(e->ival, t);
    if (t->kind == TY_F32)
      return e->ival <= (1ULL << 24);
    if (t->kind == TY_F64)
      return e->ival <= (1ULL << 53);
    return false;
  }
  if (e->kind == NT_FLOAT)
    return type_is_float(t);
  return false;
}

static Type *default_lit_type(Node *e) {
  if (e->kind == NT_INT)
    return ty_i32;
  if (e->kind == NT_FLOAT)
    return ty_f64;
  return NULL;
}

// ---------------------------------------------------------------- helpers

static void err_at(FnCtx *c, Node *n, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  diag_at(DIAG_ERROR, c->mod->path, n->line, n->col, "%s", buf);
}

// the enum behind Option/Result (prelude singletons)
static EnumDef *prelude_enum(const char *name) {
  if (!g_prelude_mod)
    return NULL;
  Sym *s = symtab_get(g_prelude_mod->syms, name);
  return (s && s->kind == SYM_ENUM) ? s->u.edef : NULL;
}

static bool is_option_of(Type *t, EnumDef *opt) {
  return t->kind == TY_ENUM && t->edef == opt;
}

// ---------------------------------------------------------------- exprs

static Type *check_expr(FnCtx *c, NodeRef er, Type *expected);
static void check_block(FnCtx *c, NodeRef br);
static void check_stmt(FnCtx *c, NodeRef sr);
static Type *check_block_value(FnCtx *c, NodeRef br, Type *expected);
static Type *check_match(FnCtx *c, NodeRef er, Type *expected);
static void check_pattern(FnCtx *c, Node *p, Type *st);
static Type *check_call(FnCtx *c, NodeRef er, Type *expected);
static Type *check_method(FnCtx *c, NodeRef er, Type *expected);
static FnDef *instantiate_generic_seeded(FnCtx *c, FnDef *f, Node *call,
                                         Type *expected, Type **seed);
static bool ty_has_param(Type *t);
static bool ty_pattern_match(Type *pat, Type *val);
static FnDef **method_candidates(FnCtx *c, Type *t, const char *name,
                                 size_t *count);

// the scalar builtins (their methods may live in the prelude)
static bool type_is_scalar_builtin(Type *t) {
  switch (t->kind) {
  case TY_I8: case TY_I16: case TY_I32: case TY_I64:
  case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
  case TY_F32: case TY_F64: case TY_BOOL: case TY_STRING:
    return true;
  default:
    return false;
  }
}

// a trait visible from module m by name (pub / same module / prelude)
static TraitDef *find_trait(Module *m, const char *name) {
  extern Program *g_program_ctx;
  for (Module *mod = g_program_ctx->modules; mod; mod = mod->next) {
    Sym *s = mod->syms ? symtab_get(mod->syms, name) : NULL;
    if (s && s->kind == SYM_TRAIT &&
        (mod == m || mod == g_prelude_mod || s->pub))
      return s->u.tdef;
  }
  return NULL;
}

// trait satisfaction = name + signature match (§3.5), computed from
// the method tables (native methods, the caller's own module, the use
// closure — the same visibility as a method call)
static bool trait_satisfied(FnCtx *c, Type *t, TraitDef *td) {
  for (size_t i = 0; i < td->nsigs; i++) {
    const char *signame = NULL;
    // the trait sig's name rides its first param decl node (parse
    // stores the fn name on the NT_FN; trait sigs hold NT_FN bodies)
    Node *sn = node_get(reflist_at(node_get(td->decl)->list, i));
    signame = sn->name;
    size_t n = 0;
    FnDef **cands = method_candidates(c, t, signame, &n);
    bool hit = false;
    for (size_t k = 0; k < n && !hit; k++) {
      FnDef *f = cands[k];
      // signature match: params[1..] vs the trait sig's params[1..]
      // (bare self params are NULL-typed on both sides), ret match
      FnSig *a = f->sig, *b = &td->sigs[i];
      size_t na = a->nparams ? a->nparams : 0;
      if (na != b->nparams)
        continue;
      bool ok = true;
      for (size_t q = 0; q < na && ok; q++) {
        Type *pa = a->params[q].ty;
        Type *pb = b->params[q].ty;
        if (!pb) // the trait's bare self slot matches any receiver
          continue;
        if (!pa || !type_eq(pa, pb))
          ok = false;
      }
      if (ok && !type_eq(a->ret, b->ret))
        ok = false;
      if (ok)
        hit = true;
    }
    if (!hit)
      return false;
  }
  return true;
}
static Type *check_format_call(FnCtx *c, Node *call, Type *expected);
static void check_assign_target(FnCtx *c, Node *lv);
static void check_assign_target_base(FnCtx *c, NodeRef base);

// compare a call's args against a signature (with literal adaptation);
// returns match quality: 0 no, 1 yes
static bool sig_matches(FnCtx *c, FnSig *sig, NodeRef call_r,
                        bool has_recv) {
  Node *call = node_get(call_r);
  RefList *args = call->list;
  size_t nargv = reflist_len(args);
  size_t nparams = sig->nparams;
  size_t first = has_recv ? 1 : 0; // recv arg already consumed
  size_t nfixed = nparams;
  bool variadic = nparams > 0 && sig->params[nparams - 1].variadic;
  if (variadic)
    nfixed--;
  size_t napplied = nargv - first;
  if (variadic) {
    if (napplied < nfixed)
      return false;
  } else if (napplied != nfixed) {
    return false;
  }
  for (size_t i = 0; i < napplied; i++) {
    Type *vt = i < nfixed ? sig->params[i + first].ty
                          : sig->params[nfixed].ty;
    Type *pt = vt; // fixed: the param type; variadic: the ELEMENT type
    if (i >= nfixed && vt && vt->kind == TY_SLICE)
      pt = vt->base;
    Node *a = node_get(reflist_at(args, i + first));
    if (a->kind != NT_POSARG)
      return false; // named args only valid on constructors
    // a trailing spread passes the whole slice: it matches the FULL
    // variadic param type, not the element type (spec §12)
    if (a->bval && variadic && i == napplied - 1)
      pt = vt;
    if (pt && ty_has_param(pt))
      continue; // generic pattern: binds at instantiation
    if (pt && pt->kind == TY_PARAM)
      continue; // generic parameter: matches (bound at instantiation)
    Node *arg = node_get(a->a);
    if (arg->kind == NT_INT || arg->kind == NT_FLOAT) {
      if (!literal_adapts_to(c, arg, pt))
        return false;
      continue;
    }
    Type *at = check_expr(c, a->a, pt);
    if (!ty_pattern_match(pt, at))
      return false;
  }
  return true;
}

// resolve a call to one of an overload chain: exact-match-unique.
// Concrete signatures match before generic ones — a hand-written
// (i32, i32) overload wins over a generic [T: Show](T, T) at the same
// argument shape (both are "exact" once instantiated; specificity
// breaks the tie, keeping zero-or-many an error among peers).
static FnDef *resolve_overload(FnCtx *c, FnDef *chain, NodeRef call_r,
                               bool has_recv, const char *what) {
  FnDef *match = NULL;
  int nmatch = 0;
  for (FnDef *f = chain; f; f = f->next_overload) {
    if (!f->ngparams && sig_matches(c, f->sig, call_r, has_recv)) {
      match = f;
      nmatch++;
    }
  }
  if (nmatch == 0) {
    for (FnDef *f = chain; f; f = f->next_overload) {
      if (f->ngparams && sig_matches(c, f->sig, call_r, has_recv)) {
        match = f;
        nmatch++;
      }
    }
  }
  if (nmatch == 1)
    return match;
  Node *call = node_get(call_r);
  if (nmatch == 0) {
    err_at(c, call, "no overload of %s matches the arguments", what);
  } else {
    err_at(c, call,
           "ambiguous call to %s: %d overloads match exactly", what,
           nmatch);
    for (FnDef *f = chain; f; f = f->next_overload)
      if (sig_matches(c, f->sig, call_r, has_recv))
        err_at(c, node_get(f->decl), "  candidate: %s (module %s)",
               f->name, f->mod->name);
  }
  return NULL;
}

static Vec g_method_cands; // of FnDef* — reset per lookup

bool sig_same(FnSig *a, FnSig *b);

static FnDef **method_candidates(FnCtx *c, Type *t, const char *name,
                                 size_t *count) {
  if (!g_method_cands.data)
    vec_init(&g_method_cands, sizeof(FnDef *));
  g_method_cands.len = 0;
  if (t->kind == TY_PTR)
    t = t->base; // method calls auto-deref one level
  Module *tmod = NULL;
  if (t->kind == TY_STRUCT)
    tmod = t->sdef->mod;
  else if (t->kind == TY_ENUM)
    tmod = t->edef->mod;
  else if (type_is_scalar_builtin(t))
    tmod = g_prelude_mod; // builtins: their methods live in the prelude
  const char *tn = t->kind == TY_STRUCT    ? t->sdef->name
                   : t->kind == TY_ENUM    ? t->edef->name
                   : tmod                  ? type_name(t)
                                           : NULL;
  if (!tn) {
    *count = 0;
    return NULL;
  }
  // native methods travel with the type (its defining module)
  if (tmod && tmod->syms) {
    for (Sym *s = tmod->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN || strcmp(s->name, name) != 0)
        continue;
      for (FnDef *f = s->u.fns; f; f = f->next_overload)
        if (f->is_method && strcmp(f->recv, tn) == 0)
          *VPUSH(g_method_cands, FnDef *) = f;
    }
  }
  // extension methods participate from the caller's own module and its
  // use closure (the design's import-scoped visibility: your own
  // declarations are trivially imported)
  Module *extmods[64];
  size_t nexts = 0;
  extmods[nexts++] = c->mod;
  for (size_t i = 0; i < VLEN(c->mod->uses) && nexts < 64; i++) {
    UseBind *ub = VAT(c->mod->uses, UseBind, i);
    bool dup = false;
    for (size_t k = 0; k < nexts; k++)
      if (extmods[k] == ub->target)
        dup = true;
    if (!dup)
      extmods[nexts++] = ub->target;
  }
  for (size_t mi = 0; mi < nexts; mi++) {
    Module *um = extmods[mi];
    if (um == tmod)
      continue; // the type's own module was scanned natively above
    if (!um->syms)
      continue;
    for (Sym *s = um->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN || strcmp(s->name, name) != 0)
        continue;
      if (um != c->mod && !s->pub)
        continue; // own module: private ok; imports: pub only
      for (FnDef *f = s->u.fns; f; f = f->next_overload) {
        if (!f->is_method || strcmp(f->recv, tn) != 0)
          continue;
        for (size_t k = 0; k < VLEN(g_method_cands); k++) {
          FnDef *g = *VAT(g_method_cands, FnDef *, k);
          if (g->mod != f->mod && sig_same(g->sig, f->sig)) {
            diag_at(DIAG_ERROR, c->mod->path, 0, 0,
                    "method '%s' with one signature defined in both "
                    "module %s and module %s",
                    name, g->mod->name, f->mod->name);
            *count = 0;
            return NULL;
          }
        }
        *VPUSH(g_method_cands, FnDef *) = f;
      }
    }
  }
  *count = VLEN(g_method_cands);
  return *count ? (FnDef **)g_method_cands.data : NULL;
}

bool sig_same(FnSig *a, FnSig *b) {
  if (a->nparams != b->nparams)
    return false;
  for (size_t i = 0; i < a->nparams; i++) {
    if (!type_eq(a->params[i].ty, b->params[i].ty))
      return false;
    if (a->params[i].variadic != b->params[i].variadic)
      return false;
  }
  return type_eq(a->ret, b->ret);
}

// intrinsic call names the checker special-cases
__attribute__((unused)) static bool is_intrinsic_fn(const char *n) {
  return strcmp(n, "printf") == 0 || strcmp(n, "eprintf") == 0 ||
         strcmp(n, "format") == 0 || strcmp(n, "len") == 0 ||
         strcmp(n, "panic") == 0 || strcmp(n, "assert") == 0 ||
         strcmp(n, "exit") == 0;
}

// printf/eprintf/format: literal format string, {} counted at compile
// time, every value via to_str, aggregates are not printable
static Type *check_format_call(FnCtx *c, Node *call, Type *expected) {
  (void)expected;
  const char *cname = node_get(call->a)->kind == NT_PATH
                          ? node_get(call->a)->name
                          : "?";
  RefList *args = call->list;
  if (reflist_len(args) == 0 ||
      node_get(reflist_at(args, 0))->kind != NT_POSARG ||
      node_get(node_get(reflist_at(args, 0))->a)->kind != NT_STR) {
    err_at(c, call, "%s takes a literal format string first", cname);
    return ty_unit;
  }
  Node *fmt = node_get(node_get(reflist_at(args, 0))->a);
  size_t holes = 0;
  for (size_t i = 0; i + 1 < fmt->sval.n; i++) {
    if (fmt->sval.p[i] == '{' && fmt->sval.p[i + 1] == '{')
      i++; // {{ escape
    else if (fmt->sval.p[i] == '{' && fmt->sval.p[i + 1] == '}')
      holes++;
    else if (fmt->sval.p[i] == '}' && fmt->sval.p[i + 1] == '}')
      i++; // }} escape
  }
  if (holes != reflist_len(args) - 1) {
    err_at(c, call,
           "format string has %zu {} hole(s) but %zu value(s) given",
           holes, reflist_len(args) - 1);
  }
  for (size_t i = 1; i < reflist_len(args); i++) {
    Node *aw = node_get(reflist_at(args, i));
    if (aw->kind != NT_POSARG) {
      err_at(c, aw, "format values are positional");
      continue;
    }
    Type *vt = check_expr(c, aw->a, NULL);
    switch (vt->kind) {
    case TY_I8: case TY_I16: case TY_I32: case TY_I64:
    case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
    case TY_F32: case TY_F64: case TY_BOOL: case TY_STRING:
      break;
    case TY_DYN: {
      // a dyn prints through its trait's to_str (dispatched at run)
      TraitDef *td = vt->tdef;
      bool printable = false;
      if (td)
        for (size_t k = 0; k < td->nsigs; k++) {
          Node *sn = node_get(reflist_at(node_get(td->decl)->list, k));
          if (strcmp(sn->name, "to_str") == 0 &&
              td->sigs[k].nparams == 1 &&
              td->sigs[k].ret->kind == TY_STRING)
            printable = true;
        }
      if (!printable)
        err_at(c, node_get(aw->a),
               "dyn %s is not printable (its trait has no to_str)",
               td ? td->name : "?");
      break;
    }
    default: {
      // §8: every value prints via to_str — a type carrying a
      // to_str(self) -> string method IS printable
      size_t n = 0;
      FnDef **cands = method_candidates(c, vt, "to_str", &n);
      bool printable = false;
      for (size_t k = 0; k < n; k++)
        if (cands[k]->sig->nparams == 1 &&
            cands[k]->sig->ret->kind == TY_STRING)
          printable = true;
      if (!printable)
        err_at(c, node_get(aw->a),
               "value of type %s is not printable (no to_str)",
               type_name(vt));
      break;
    }
    }
  }
  return strcmp(cname, "format") == 0 ? ty_string : ty_unit;
}

// enum generic param names in declaration order
static const char **edef_gnames(EnumDef *ed) {
  if (!ed->ngparams)
    return NULL;
  static const char ***cache; // per-call-site stable not needed: arena
  (void)cache;
  const char **names = arena_alloc(g_arena, ed->ngparams * sizeof(char *), 8);
  RefList *gps = node_get(ed->decl)->a != NO_REF
                     ? node_get(node_get(ed->decl)->a)->list : NULL;
  for (size_t i = 0; i < ed->ngparams && gps; i++)
    names[i] = node_get(reflist_at(gps, i))->name;
  return names;
}

// substitute generic parameters through a type (deep)
typedef struct TBind {
  const char **names;
  Type **tys;
  size_t n;
} TBind;

// does the type mention a generic parameter (deep)?
static bool ty_has_param(Type *t) {
  if (!t)
    return false;
  if (t->kind == TY_PARAM)
    return true;
  if (t->base && ty_has_param(t->base))
    return true;
  for (size_t i = 0; i < t->nargs; i++)
    if (ty_has_param(t->args[i]))
      return true;
  return false;
}

// structural unification: bind the generic names in pat from val;
// false on a shape clash or a conflicting rebind
static bool tunify(Type *pat, Type *val, TBind *b) {
  if (!pat || !val)
    return pat == val;
  if (pat->kind == TY_PARAM) {
    for (size_t i = 0; i < b->n; i++)
      if (b->names[i] && strcmp(b->names[i], pat->pname) == 0) {
        if (b->tys[i] && !type_eq(b->tys[i], val))
          return false;
        b->tys[i] = val;
        return true;
      }
    return true; // a param of some outer scope: nothing to bind
  }
  switch (pat->kind) {
  case TY_PTR:
  case TY_SLICE:
  case TY_WEAK:
    return val->kind == pat->kind &&
           tunify(pat->base, val->base, b);
  case TY_STRUCT:
    if (val->kind != TY_STRUCT || val->sdef != pat->sdef ||
        val->nargs != pat->nargs)
      return false;
    for (size_t i = 0; i < pat->nargs; i++)
      if (!tunify(pat->args[i], val->args[i], b))
        return false;
    return true;
  case TY_ENUM:
    if (val->kind != TY_ENUM || val->edef != pat->edef ||
        val->nargs != pat->nargs)
      return false;
    for (size_t i = 0; i < pat->nargs; i++)
      if (!tunify(pat->args[i], val->args[i], b))
        return false;
    return true;
  default:
    return type_eq(pat, val);
  }
}

// a generic signature's type is a PATTERN: params match any type at
// their position (binding happens at instantiation)
static bool ty_pattern_match(Type *pat, Type *val) {
  if (!pat || !val)
    return pat == val;
  if (pat->kind == TY_PARAM)
    return true;
  if (!ty_has_param(pat))
    return type_eq(pat, val);
  switch (pat->kind) {
  case TY_PTR:
  case TY_SLICE:
  case TY_WEAK:
    return val->kind == pat->kind && ty_pattern_match(pat->base, val->base);
  case TY_STRUCT:
    if (val->kind != TY_STRUCT || val->sdef != pat->sdef ||
        val->nargs != pat->nargs)
      return false;
    for (size_t i = 0; i < pat->nargs; i++)
      if (!ty_pattern_match(pat->args[i], val->args[i]))
        return false;
    return true;
  case TY_ENUM:
    if (val->kind != TY_ENUM || val->edef != pat->edef ||
        val->nargs != pat->nargs)
      return false;
    for (size_t i = 0; i < pat->nargs; i++)
      if (!ty_pattern_match(pat->args[i], val->args[i]))
        return false;
    return true;
  default:
    return type_eq(pat, val);
  }
}

static Type *tsubst_impl(Type *t, TBind *b) {
  if (!t)
    return t;
  if (t->kind == TY_PARAM) {
    if (!b->names || !b->tys)
      return t;
    for (size_t i = 0; i < b->n; i++)
      if (b->names[i] && strcmp(b->names[i], t->pname) == 0)
        return b->tys[i] ? b->tys[i] : t;
    return t;
  }
  if (t->kind == TY_PTR)
    return type_ptr(tsubst(t->base, b));
  if (t->kind == TY_SLICE)
    return type_slice(tsubst(t->base, b));
  if (t->kind == TY_STRUCT || t->kind == TY_ENUM) {
    if (!t->nargs)
      return t;
    Type **args = arena_alloc(g_arena, t->nargs * sizeof(Type *), 8);
    for (size_t i = 0; i < t->nargs; i++)
      args[i] = tsubst(t->args[i], b);
    return t->kind == TY_STRUCT ? type_struct(t->sdef, args, t->nargs)
                                : type_enum(t->edef, args, t->nargs);
  }
  return t;
}

// infer a generic enum instantiation at a constructor site: seed from
// the expected type, then from constructor arguments; leftovers error
static Type *instantiate_enum_ctor(FnCtx *c, EnumDef *ed, Type *expected,
                                   EnumVariant *var, RefList *args) {
  size_t ng = ed->ngparams;
  if (ng == 0)
    return type_enum(ed, NULL, 0);
  const char **names = edef_gnames(ed);
  Type **tys = arena_alloc(g_arena, ng * sizeof(Type *), 8);
  memset(tys, 0, ng * sizeof(Type *));
  if (expected && expected->kind == TY_ENUM && expected->edef == ed) {
    for (size_t i = 0; i < ng && i < expected->nargs; i++)
      tys[i] = expected->args[i];
  }
  for (size_t i = 0; i < ng; i++) {
    if (tys[i])
      continue;
    // find the first constructor field of exactly this param type
    for (size_t k = 0; k < var->nfields && !tys[i]; k++) {
      Type *ft = var->fields[k].ty;
      if (ft->kind == TY_PARAM && strcmp(ft->pname, names[i]) == 0 &&
          k < reflist_len(args)) {
        Node *aw = node_get(reflist_at(args, k));
        if (aw->kind == NT_POSARG)
          tys[i] = check_expr(c, aw->a, NULL);
      }
    }
  }
  for (size_t i = 0; i < ng; i++)
    if (!tys[i]) {
      diag_at(DIAG_ERROR, c->mod->path, var->decl ? node_get(var->decl)->line : 0,
              var->decl ? node_get(var->decl)->col : 0,
              "cannot infer generic argument '%s' of %s here", names[i],
              ed->name);
      tys[i] = ty_i32;
    }
  return type_enum(ed, tys, ng);
}

static Type *check_call(FnCtx *c, NodeRef er, Type *expected) {
  Node *call = node_get(er);
  RefList *args = call->list;

  // named arguments are only legal on constructor calls (variant ctor
  // via method syntax); plain calls reject them
  for (size_t i = 0; i < reflist_len(args); i++) {
    Node *a = node_get(reflist_at(args, i));
    if (a->kind == NT_FIELDINIT) {
      err_at(c, a, "named arguments are only valid in constructors");
      return ty_unit;
    }
    if (a->kind == NT_POSARG && a->bval && i != reflist_len(args) - 1) {
      err_at(c, a, "spread '...' is only valid as the last argument");
    }
  }

  // callee: a name (function, possibly module-qualified) or any
  // expression of fn type (closures, fn values)
  Node *callee = node_get(call->a);
  if (callee->kind != NT_PATH) {
    Type *ct = check_expr(c, call->a, NULL);
    if (ct->kind == TY_FN) {
      FnSig *sig = ct->sig;
      size_t nfixed = sig->nparams;
      if (reflist_len(args) != nfixed)
        err_at(c, call, "call takes %zu argument(s), got %zu", nfixed,
               reflist_len(args));
      for (size_t i = 0; i < reflist_len(args); i++) {
        Node *aw = node_get(reflist_at(args, i));
        if (aw->kind != NT_POSARG) {
          err_at(c, aw, "named arguments are only valid in constructors");
          continue;
        }
        check_expr(c, aw->a, i < nfixed ? sig->params[i].ty : NULL);
      }
      return sig->ret;
    }
    err_at(c, callee, "calling a non-function value");
    return ty_unit;
  }
  Look lk;
  if (!lookup(c, callee->name, &lk)) {
    err_at(c, callee, "unknown name '%s'", callee->name);
    return ty_unit;
  }
  if (lk.kind == LOOK_LOCAL && lk.local->ty &&
      lk.local->ty->kind == TY_FN) {
    // fn value / closure call
    FnSig *sig = lk.local->ty->sig;
    size_t nfixed = sig->nparams;
    if (reflist_len(args) != nfixed)
      err_at(c, call, "call takes %zu argument(s), got %zu", nfixed,
             reflist_len(args));
    for (size_t i = 0; i < reflist_len(args); i++) {
      Node *aw = node_get(reflist_at(args, i));
      if (aw->kind != NT_POSARG) {
        err_at(c, aw, "named arguments are only valid in constructors");
        continue;
      }
      check_expr(c, aw->a, i < nfixed ? sig->params[i].ty : NULL);
    }
    return sig->ret;
  }
  if (lk.kind == LOOK_FN) {
    // intrinsics
    if ((strcmp(callee->name, "printf") == 0 ||
         strcmp(callee->name, "eprintf") == 0 ||
         strcmp(callee->name, "format") == 0) &&
        lk.fns->mod == g_prelude_mod) {
      return check_format_call(c, call, expected);
    }
    if (strcmp(callee->name, "make") == 0 &&
        lk.fns->mod == g_prelude_mod) {
      if (reflist_len(args) >= 1 &&
          node_get(reflist_at(args, 0))->kind == NT_POSARG &&
          node_get(reflist_at(args, 0))->op == 1) {
        // type-argument form: make([]T, n) — the parser stashed the
        // element type on the marker arg
        Node *targ = node_get(node_get(reflist_at(args, 0))->a);
        GScope g2 = {0};
        Type *el = resolve_type_pub(c->mod, targ->a, &g2);
        Type *st = el && el->kind == TY_SLICE
                       ? el
                       : (el ? type_slice(el) : NULL);
        if (st) {
          if (reflist_len(args) > 1) {
            Node *cnt = node_get(reflist_at(args, 1));
            if (cnt->kind == NT_POSARG)
              check_expr(c, cnt->a, ty_usize);
          }
          return st;
        }
      }
      err_at(c, call, "make takes a slice type and a length");
      return ty_unit;
    }
    if (strcmp(callee->name, "len") == 0 && lk.fns->mod == g_prelude_mod) {
      if (reflist_len(args) != 1 ||
          node_get(reflist_at(args, 0))->kind != NT_POSARG) {
        err_at(c, call, "len takes exactly one value");
        return ty_usize;
      }
      Type *vt = check_expr(c, node_get(reflist_at(args, 0))->a, NULL);
      if (vt->kind != TY_SLICE && vt->kind != TY_STRING)
        err_at(c, call, "len takes a string or slice, got %s",
               type_name(vt));
      return ty_usize;
    }
    FnDef *f = resolve_overload(c, lk.fns, er, false, callee->name);
    if (!f)
      return ty_unit;
    if (f->ngparams) {
      // monomorphize: bind gparams from args (and expected), reuse
      // identical instantiations, emit-check per instance
      extern FnDef *instantiate_generic(FnCtx * c, FnDef * f, Node *call,
                                        Type * expected);
      FnDef *inst = instantiate_generic(c, f, node_get(er), expected);
      if (!inst)
        return ty_unit;
      node_get(er)->sem2 = inst;
      f = inst;
      // re-check the args against the substituted signature
      for (size_t i = 0; i < reflist_len(call->list); i++) {
        Node *aw = node_get(reflist_at(call->list, i));
        if (aw->kind != NT_POSARG)
          continue;
        size_t fixed = f->sig->nparams;
        bool variadic2 = fixed > 0 && f->sig->params[fixed - 1].variadic;
        Type *pt = NULL;
        if (variadic2 && i >= fixed - 1) {
          pt = f->sig->params[fixed - 1].ty;
          if (pt && pt->kind == TY_SLICE)
            pt = pt->base;
        } else if (i < fixed)
          pt = f->sig->params[i].ty;
        check_expr(c, aw->a, pt);
      }
      return f->sig->ret;
    }
    node_get(er)->sem2 = f; // the chosen overload rides with the call
    // check args against the chosen signature (proper, with consumers)
    for (size_t i = 0; i < reflist_len(args); i++) {
      Node *aw = node_get(reflist_at(args, i));
      if (aw->kind != NT_POSARG)
        continue;
      size_t fixed = f->sig->nparams;
      bool variadic = fixed > 0 && f->sig->params[fixed - 1].variadic;
      Type *pt;
      if (variadic && i >= fixed - 1) {
        pt = f->sig->params[fixed - 1].ty;
        if (pt && pt->kind == TY_SLICE)
          pt = pt->base; // element face for the trailing args
      } else if (i < fixed)
        pt = f->sig->params[i].ty;
      else
        pt = NULL;
      check_expr(c, aw->a, pt);
    }
    return f->sig->ret;
  }
  if (lk.kind == LOOK_STRUCT) {
    err_at(c, callee, "struct %s constructs with 'new %s { … }'",
           callee->name, callee->name);
    return ty_unit;
  }
  err_at(c, callee, "'%s' is not callable", callee->name);
  return ty_unit;
}

static Type *check_method(FnCtx *c, NodeRef er, Type *expected) {
  (void)expected;
  Node *m = node_get(er);

  // weak.from(ptr): the weak constructor
  Node *recv0 = node_get(m->a);
  if (recv0->kind == NT_PATH && strcmp(recv0->name, "weak") == 0 &&
      strcmp(m->name, "from") == 0 &&
      !lookup(c, "weak", &((Look){0}))) {
    if (reflist_len(m->list) == 1 &&
        node_get(reflist_at(m->list, 0))->kind == NT_POSARG) {
      Type *pt = check_expr(c, node_get(reflist_at(m->list, 0))->a,
                            NULL);
      if (pt->kind != TY_PTR) {
        err_at(c, m, "weak.from takes *T, got %s", type_name(pt));
        return ty_unit;
      }
      Type *w = make_type_public(TY_WEAK);
      w->base = pt->base;
      return w;
    }
    err_at(c, m, "weak.from takes exactly one pointer");
    return ty_unit;
  }
  // intrinsics.*: the unsafe window (spec §2). f64_bits is the float
  // formatter's only bit access; the namespace never resolves as a
  // module, so a user module named `intrinsics` wins
  if (recv0->kind == NT_PATH && strcmp(recv0->name, "intrinsics") == 0 &&
      !lookup(c, "intrinsics", &((Look){0}))) {
    if (strcmp(m->name, "f64_bits") == 0) {
      if (reflist_len(m->list) == 1 &&
          node_get(reflist_at(m->list, 0))->kind == NT_POSARG) {
        Type *pt = check_expr(c, node_get(reflist_at(m->list, 0))->a,
                              ty_f64);
        if (pt->kind != TY_F64) {
          err_at(c, m, "intrinsics.f64_bits takes f64, got %s",
                 type_name(pt));
          return ty_unit;
        }
        node_get(er)->op = 4; // intrinsic marker for the emitter
        return ty_u64;
      }
      err_at(c, m, "intrinsics.f64_bits takes exactly one f64");
      return ty_unit;
    }
    err_at(c, m, "unknown intrinsic '%s'", m->name);
    return ty_unit;
  }
  // Type.assoc_fn(args): the receiver names a struct/enum type and
  // the call hits an associated fn (declared without self)
  Node *recvA = node_get(m->a);
  if (recvA->kind == NT_PATH) {
    Look lkA;
    if (lookup(c, recvA->name, &lkA) &&
        (lkA.kind == LOOK_STRUCT || lkA.kind == LOOK_ENUM)) {
      StructDef *sd = lkA.kind == LOOK_STRUCT ? lkA.sdef : NULL;
      EnumDef *ed = lkA.kind == LOOK_ENUM ? lkA.edef : NULL;
      Module *tmodA = sd ? sd->mod : ed->mod;
      if (tmodA && tmodA->syms) {
        Sym *sA = symtab_get(tmodA->syms, m->name);
        if (sA && sA->kind == SYM_FN) {
          for (FnDef *fA = sA->u.fns; fA; fA = fA->next_overload) {
            if (fA->is_assoc && !fA->is_method) {
              // exact-match unique across assoc candidates
              FnDef *hit = NULL;
              size_t nhits = 0;
              for (FnDef *fB = sA->u.fns; fB; fB = fB->next_overload) {
                if (!fB->is_assoc)
                  continue;
                if (sig_matches(c, fB->sig, er, false)) {
                  hit = fB;
                  nhits++;
                }
              }
              if (nhits == 1) {
                // a generic assoc fn instantiates at the call site
                // (implicit type params bind from args / the expected
                // return, exactly like a plain generic fn)
                if (hit->ngparams) {
                  extern FnDef *instantiate_generic(FnCtx * c, FnDef * f,
                                                    Node * call,
                                                    Type * expected);
                  FnDef *inst = instantiate_generic(c, hit, m, expected);
                  if (!inst)
                    return ty_unit;
                  node_get(er)->op = 5;
                  node_get(er)->sem2 = inst;
                  for (size_t ai = 0; ai < reflist_len(m->list); ai++) {
                    Node *aw = node_get(reflist_at(m->list, ai));
                    if (aw->kind != NT_POSARG)
                      continue;
                    size_t np = inst->sig->nparams;
                    bool va = np > 0 && inst->sig->params[np - 1].variadic;
                    size_t nfx = va ? np - 1 : np;
                    Type *pt2 = ai < nfx ? inst->sig->params[ai].ty
                                  : va ? inst->sig->params[nfx].ty : NULL;
                    check_expr(c, aw->a, pt2);
                  }
                  return inst->sig->ret;
                }
                // sig_matches' literal fast path leaves arg sems unset;
                // check every arg for real (the emitter reads them)
                for (size_t ai = 0; ai < reflist_len(m->list); ai++) {
                  Node *aw = node_get(reflist_at(m->list, ai));
                  if (aw->kind != NT_POSARG)
                    continue;
                  size_t nfixed2 = hit->sig->nparams;
                  bool variadic2 = nfixed2 > 0 &&
                      hit->sig->params[nfixed2 - 1].variadic;
                  if (variadic2)
                    nfixed2--;
                  Type *pt2 = ai < nfixed2
                                  ? hit->sig->params[ai].ty
                                  : (variadic2
                                         ? hit->sig->params[nfixed2].ty
                                         : NULL);
                  if (pt2 && pt2->kind == TY_SLICE)
                    pt2 = pt2->base; // variadic element type
                  check_expr(c, aw->a, pt2);
                }
                node_get(er)->op = 5; // assoc call marker for the emitter
                node_get(er)->sem2 = hit;
                return hit->sig->ret;
              }
              err_at(c, m, "no overload of %s.%s matches the arguments "
                     "(%zu candidates)", recvA->name, m->name, nhits);
              return ty_unit;
            }
          }
        }
      }
    }
  }
  // Enum.Variant construction and module calls: recv is a bare name
  // that resolves to an enum or a use binding — checked before the
  // recv is typed as an expression
  Node *recv = node_get(m->a);
  if (recv->kind == NT_PATH) {
    Look lk;
    if (lookup(c, recv->name, &lk) && lk.kind == LOOK_ENUM) {
      for (size_t v = 0; v < lk.edef->nvariants; v++) {
        EnumVariant *var = &lk.edef->variants[v];
        if (strcmp(var->name, m->name) != 0)
          continue;
        Type *inst = instantiate_enum_ctor(c, lk.edef, expected, var,
                                           m->list);
        // bind the declaration's generic params to the instance args
        TBind tb = {edef_gnames(lk.edef), inst->args,
                    inst->args ? lk.edef->ngparams : 0};
        node_get(er)->sem2 = var; // the chosen variant
        // tuple-form constructor: positional args (or named for struct
        // variants)
        size_t nfields = var->nfields;
        size_t nargv = reflist_len(m->list);
        if (nargv != nfields) {
          err_at(c, m, "variant %s.%s takes %zu value(s), got %zu",
                 recv->name, m->name, nfields, nargv);
          return ty_unit;
        }
        // named args must cover all fields exactly once
        if (var->form == VAR_STRUCT && nargv > 0 &&
            node_get(reflist_at(m->list, 0))->kind == NT_FIELDINIT) {
          for (size_t i = 0; i < nfields; i++) {
            bool found = false;
            for (size_t j = 0; j < nargv; j++) {
              Node *aw = node_get(reflist_at(m->list, j));
              if (aw->kind == NT_FIELDINIT &&
                  strcmp(aw->name, var->fields[i].name) == 0) {
                if (found)
                  err_at(c, aw, "field '%s' set twice", aw->name);
                found = true;
                check_expr(c, aw->a, tsubst(var->fields[i].ty, &tb));
              }
            }
            if (!found)
              err_at(c, m, "missing field '%s' in %s.%s",
                     var->fields[i].name, recv->name, m->name);
          }
          // no unknown fields
          for (size_t j = 0; j < nargv; j++) {
            Node *aw = node_get(reflist_at(m->list, j));
            bool known = false;
            for (size_t i = 0; i < nfields; i++)
              if (strcmp(aw->name, var->fields[i].name) == 0)
                known = true;
            if (!known)
              err_at(c, aw, "unknown field '%s' in %s.%s", aw->name,
                     recv->name, m->name);
          }
        } else {
          for (size_t j = 0; j < nargv; j++) {
            Node *aw = node_get(reflist_at(m->list, j));
            if (aw->kind == NT_FIELDINIT)
              err_at(c, aw, "named arguments need a struct-form variant");
            else
              check_expr(c, aw->a, tsubst(var->fields[j].ty, &tb));
          }
        }
        return inst;
      }
      err_at(c, m, "enum %s has no variant '%s'", recv->name, m->name);
      return ty_unit;
    }
    // module-qualified call: mod.fn(args)
    if (lookup(c, recv->name, &lk) && lk.kind == LOOK_MODULE) {
      Sym *s = lk.module->syms ? symtab_get(lk.module->syms, m->name) : NULL;
      if (s && s->kind == SYM_FN && s->pub) {
        FnDef *f = resolve_overload(c, s->u.fns, er, false, m->name);
        if (!f)
          return ty_unit;
        for (size_t i = 0; i < reflist_len(m->list); i++) {
          Node *aw = node_get(reflist_at(m->list, i));
          if (aw->kind == NT_POSARG)
            check_expr(c, aw->a,
                       i < f->sig->nparams ? f->sig->params[i].ty : NULL);
        }
        node_get(er)->op = 5; // a plain call on the module's namespace
        node_get(er)->sem2 = f;
        return f->sig->ret;
      }
      err_at(c, m, "module %s has no public fn '%s'", recv->name, m->name);
      return ty_unit;
    }
  }

  // real method call on a value
  Type *rt = check_expr(c, m->a, NULL);
  if (rt->kind == TY_WEAK && strcmp(m->name, "get") == 0) {
    Sym *os2 = g_prelude_mod ? symtab_get(g_prelude_mod->syms, "Option")
                             : NULL;
    if (os2 && os2->kind == SYM_ENUM) {
      Type **args2 = arena_alloc(g_arena, sizeof(Type *), 8);
      args2[0] = type_ptr(rt->base);
      Type *r = type_enum(os2->u.edef, args2, 1);
      r->is_opt = true;
      node_get(er)->op = 3; // weak.get marker for the emitter
      return r;
    }
  }
  size_t ncand = 0;
  // dyn dispatch: the receiver is a fat {vtable, obj}; the method
  // resolves inside the trait and lowers to an indirect call
  if (rt->kind == TY_DYN && rt->tdef) {
    TraitDef *td = rt->tdef;
    for (size_t i = 0; i < td->nsigs; i++) {
      Node *sn = node_get(reflist_at(node_get(td->decl)->list, i));
      if (strcmp(sn->name, m->name) != 0)
        continue;
      FnSig *sig = &td->sigs[i];
      size_t nparams = sig->nparams;
      size_t nargv = reflist_len(m->list);
      if (nparams != nargv + 1) {
        err_at(c, m, "method '%s' of %s takes %zu argument(s), got %zu",
               m->name, td->name, nparams - 1, nargv);
        return ty_unit;
      }
      for (size_t j = 0; j < nargv; j++) {
        Node *aw = node_get(reflist_at(m->list, j));
        if (aw->kind != NT_POSARG) {
          err_at(c, aw, "named arguments are only valid in constructors");
          continue;
        }
        Type *pt = sig->params[j + 1].ty;
        check_expr(c, aw->a, pt);
      }
      node_get(er)->op = 6; // dyn dispatch marker for the emitter
      node_get(er)->ival = i;
      node_get(er)->sem2 = td;
      return sig->ret;
    }
    err_at(c, m, "trait %s has no method '%s'", td->name, m->name);
    return ty_unit;
  }
  FnDef **cands = method_candidates(c, rt, m->name, &ncand);
  if (!ncand) {
    err_at(c, m, "no method '%s' for type %s (native or in the use "
                 "closure)",
           m->name, type_name(rt));
    return ty_unit;
  }
  // exact-match-unique over candidates (recv already consumed)
  int nmatch = 0;
  FnDef *chosen = NULL;
  // a generic method's implicit params bind from the receiver
  // instantiation (self: Opt binds T from Opt[i32])
  Type *rbase = rt->kind == TY_PTR ? rt->base : rt;
  for (size_t i = 0; i < ncand; i++) {
    FnDef *f = cands[i];
    // arity/type check with literal adaptation (params[0] is self)
    size_t nparams = f->sig->nparams;
    size_t nargv = reflist_len(m->list);
    bool variadic = nparams > 0 && f->sig->params[nparams - 1].variadic;
    size_t nfixed = variadic ? nparams - 1 : nparams;
    bool ok = variadic ? nargv + 1 >= nfixed : nargv + 1 == nfixed;
    // substitute the implicit binds through the param types first
    Type *selfty = NULL;
    TBind itb = {0};
    if (ok && f->nimplicit &&
        (rbase->kind == TY_STRUCT || rbase->kind == TY_ENUM) &&
        rbase->nargs == f->nimplicit) {
      itb.names = f->gparams;
      itb.tys = arena_alloc(g_arena, f->nimplicit * sizeof(Type *), 8);
      for (size_t b = 0; b < f->nimplicit; b++)
        itb.tys[b] = rbase->args[b];
      itb.n = f->nimplicit;
      selfty = tsubst(f->sig->params[0].ty, &itb);
      if (!type_eq(selfty, rt))
        ok = false;
    } else if (ok && f->nimplicit) {
      ok = false; // generic method needs an instantiated receiver
    }
    for (size_t j = 0; ok && j < nargv; j++) {
      Node *aw = node_get(reflist_at(m->list, j));
      if (aw->kind != NT_POSARG) {
        ok = false;
        break;
      }
      Type *pt = j < nfixed ? f->sig->params[j + 1].ty
                            : f->sig->params[nfixed].ty;
      if (itb.n)
        pt = tsubst(pt, &itb); // implicit binds applied
      Node *arg = node_get(aw->a);
      if (arg->kind == NT_INT || arg->kind == NT_FLOAT) {
        if (!literal_adapts_to(c, arg, pt))
          ok = false;
      } else {
        Type *at = check_expr(c, aw->a, pt);
        if (!type_eq(at, pt))
          ok = false;
      }
    }
    if (ok) {
      nmatch++;
      chosen = f;
    }
  }
  if (nmatch != 1) {
    if (nmatch == 0)
      err_at(c, m, "no overload of method '%s' matches the arguments",
             m->name);
    else
      err_at(c, m, "ambiguous method call '%s' (%d exact matches)",
             m->name, nmatch);
    return ty_unit;
  }
  if (chosen->ngparams) {
    // monomorphize the generic method: implicit params seeded from the
    // receiver, the rest from args/expected, then the args re-check
    // against the substituted signature
    Type **seed = NULL;
    if (chosen->nimplicit) {
      seed = arena_alloc(g_arena, chosen->ngparams * sizeof(Type *), 8);
      memset(seed, 0, chosen->ngparams * sizeof(Type *));
      for (size_t b = 0; b < chosen->nimplicit && b < rbase->nargs; b++)
        seed[b] = rbase->args[b];
    }
    FnDef *inst =
        instantiate_generic_seeded(c, chosen, m, expected, seed);
    if (!inst)
      return ty_unit;
    node_get(er)->sem2 = inst;
    node_get(er)->op = 2;
    for (size_t j = 0; j < reflist_len(m->list); j++) {
      Node *aw = node_get(reflist_at(m->list, j));
      if (aw->kind != NT_POSARG)
        continue;
      size_t nparams = inst->sig->nparams;
      bool variadic = nparams > 0 && inst->sig->params[nparams - 1].variadic;
      size_t nfixed = variadic ? nparams - 1 : nparams;
      Type *pt = j < nfixed ? inst->sig->params[j + 1].ty
                            : variadic ? inst->sig->params[nfixed].ty : NULL;
      check_expr(c, aw->a, pt);
    }
    return inst->sig->ret;
  }
  node_get(er)->sem2 = chosen; // the chosen method (emit reads it)
  node_get(er)->op = 2;        // marks: real method call
  for (size_t j = 0; j < reflist_len(m->list); j++) {
    Node *aw = node_get(reflist_at(m->list, j));
    if (aw->kind != NT_POSARG)
      continue;
    size_t nparams = chosen->sig->nparams;
    bool variadic = nparams > 0 && chosen->sig->params[nparams - 1].variadic;
    size_t nfixed = variadic ? nparams - 1 : nparams;
    Type *pt = j < nfixed ? chosen->sig->params[j + 1].ty
                          : variadic ? chosen->sig->params[nfixed].ty : NULL;
    check_expr(c, aw->a, pt);
  }
  return chosen->sig->ret;
}

static Type *check_expr_inner(FnCtx *c, NodeRef er, Type *expected) {
  if (er == NO_REF)
    return ty_unit;
  Node *e = node_get(er);
  switch (e->kind) {
  case NT_INT: {
    Type *t = expected;
    if (!t || !literal_adapts_to(c, e, t)) {
      t = default_lit_type(e);
      if (expected && !type_eq(t, expected) &&
          !literal_adapts_to(c, e, expected)) {
        // fall through: report below when expected exists and mismatches
      }
      if (expected && !literal_adapts_to(c, e, expected) &&
          !type_eq(t, expected)) {
        err_at(c, e, "integer literal does not fit %s",
               type_name(expected));
        return expected;
      }
    }
    if (t->kind == TY_F32 || t->kind == TY_F64)
      return t; // exact integer→float literal adaptation
    if (!int_fits(e->ival, t))
      err_at(c, e, "integer literal %llu does not fit %s",
             (unsigned long long)e->ival, type_name(t));
    return t;
  }
  case NT_FLOAT: {
    Type *t = expected && type_is_float(expected) ? expected : ty_f64;
    if (expected && !type_is_float(expected)) {
      err_at(c, e, "float literal cannot be %s", type_name(expected));
      return expected;
    }
    return t;
  }
  case NT_BOOL:
    if (expected && expected->kind != TY_BOOL)
      err_at(c, e, "expected %s, found bool", type_name(expected));
    return ty_bool;
  case NT_STR:
    if (expected && expected->kind != TY_STRING)
      err_at(c, e, "expected %s, found string", type_name(expected));
    return ty_string;
  case NT_PATH: {
    Look lk;
    if (!lookup(c, e->name, &lk)) {
      err_at(c, e, "unknown name '%s'", e->name);
      return ty_unit;
    }
    switch (lk.kind) {
    case LOOK_LOCAL:
      return lk.local->ty;
    case LOOK_CONST:
    case LOOK_STATIC: {
      // const value; type from inference (T1.6 fills; string/num here)
      if (!lk.konst->ty)
        lk.konst->ty = ty_i32; // provisional; T1.6 infers properly
      return lk.konst->ty;
    }
    case LOOK_FN: {
      // fn value (non-variadic only)
      FnDef *f = lk.fns;
      if (f->next_overload) {
        err_at(c, e, "ambiguous function value '%s' (overloaded)", e->name);
        return ty_unit;
      }
      if (f->sig->nparams && f->sig->params[f->sig->nparams - 1].variadic) {
        err_at(c, e, "variadic functions are not first-class values");
      }
      return type_fn(f->sig);
    }
    case LOOK_ENUM:
      err_at(c, e, "enum %s is a type, not a value (use %s.Variant)",
             e->name, e->name);
      return ty_unit;
    case LOOK_STRUCT:
      err_at(c, e, "struct %s constructs with 'new'", e->name);
      return ty_unit;
    case LOOK_MODULE:
      err_at(c, e, "module %s is not a value", e->name);
      return ty_unit;
    default:
      return ty_unit;
    }
  }
  case NT_CALL:
    return check_call(c, er, expected);
  case NT_METHOD:
    return check_method(c, er, expected);
  case NT_FIELD_E: {
    // module.const / module.fn-value / Enum.unit_variant / expr.field
    Node *recv = node_get(e->a);
    if (recv->kind == NT_PATH) {
      Look lk;
      if (lookup(c, recv->name, &lk)) {
        if (lk.kind == LOOK_MODULE && lk.module->syms) {
          Sym *s = symtab_get(lk.module->syms, e->name);
          if (!s || !s->pub) {
            err_at(c, e, "module %s has no public '%s'", recv->name,
                   e->name);
            return ty_unit;
          }
          if (s->kind == SYM_CONST) {
            if (!s->u.konst->ty)
              s->u.konst->ty = ty_i32;
            return s->u.konst->ty;
          }
          if (s->kind == SYM_FN) {
            err_at(c, e, "call it: %s.%s(…)", recv->name, e->name);
            return ty_unit;
          }
          err_at(c, e, "'%s' is not a value", e->name);
          return ty_unit;
        }
        if (lk.kind == LOOK_ENUM) {
          for (size_t v = 0; v < lk.edef->nvariants; v++)
            if (strcmp(lk.edef->variants[v].name, e->name) == 0 &&
                lk.edef->variants[v].form == VAR_UNIT) {
              node_get(er)->sem2 = &lk.edef->variants[v];
              // generic enums instantiate from the expected type
              if (expected && expected->kind == TY_ENUM &&
                  expected->edef == lk.edef)
                return expected;
              if (lk.edef->ngparams == 0)
                return type_enum(lk.edef, NULL, 0);
              err_at(c, e,
                     "cannot infer the generic arguments of %s.%s here "
                     "— annotate the binding",
                     lk.edef->name, e->name);
              return type_enum(lk.edef, NULL, lk.edef->ngparams);
            }
          err_at(c, e, "enum %s has no unit variant '%s'", recv->name,
                 e->name);
          return ty_unit;
        }
      }
    }
    Type *bt = check_expr(c, e->a, NULL);
    if (bt->kind == TY_PTR && bt->base->kind == TY_STRUCT)
      bt = bt->base; // one implicit deref (no -> operator exists)
    if (bt->kind != TY_STRUCT) {
      err_at(c, e, "type %s has no fields", type_name(bt));
      return ty_unit;
    }
    for (size_t i = 0; i < bt->sdef->nfields; i++)
      if (strcmp(bt->sdef->fields[i].name, e->name) == 0) {
        node_get(er)->op = (int)i; // field index for the emitter
        // the field type lives through the receiver instantiation
        Type *ft = bt->sdef->fields[i].ty;
        if (bt->nargs && bt->sdef->gparams) {
          TBind fb = {bt->sdef->gparams, bt->args, bt->nargs};
          ft = tsubst(ft, &fb);
        }
        return ft;
      }
    err_at(c, e, "struct %s has no field '%s'", bt->sdef->name, e->name);
    return ty_unit;
  }
  case NT_INDEX: {
    Type *bt = check_expr(c, e->a, NULL);
    Type *it = check_expr(c, e->b, ty_usize);
    if (it->kind != TY_USIZE)
      err_at(c, e, "index is %s, want usize", type_name(it));
    if (bt->kind == TY_SLICE)
      return bt->base;
    if (bt->kind == TY_STRING)
      return ty_u8;
    err_at(c, e, "type %s is not indexable", type_name(bt));
    return ty_unit;
  }
  case NT_SLICE_E: {
    Type *bt = check_expr(c, e->a, NULL);
    if (e->b != NO_REF)
      check_expr(c, e->b, ty_usize);
    if (e->c != NO_REF)
      check_expr(c, e->c, ty_usize);
    if (bt->kind != TY_STRING && bt->kind != TY_SLICE)
      err_at(c, e, "type %s is not sliceable", type_name(bt));
    return bt;
  }
  case NT_UNARY: {
    Node *operand = node_get(e->a);
    if (e->op == OP_NEG &&
        (operand->kind == NT_INT || operand->kind == NT_FLOAT)) {
      // -literal: the consumer flows through the negation — including
      // the MIN magnitudes that only fit after the fold
      if (expected && literal_adapts_to(c, e, expected)) {
        operand->sem = expected;
        return expected;
      }
      Type *t = check_expr(c, e->a, NULL);
      if (!type_is_num(t))
        err_at(c, e, "cannot negate %s", type_name(t));
      return t;
    }
    Type *ot = check_expr(c, e->a, NULL);
    switch (e->op) {
    case OP_NEG:
      if (!type_is_num(ot))
        err_at(c, e, "cannot negate %s", type_name(ot));
      return ot;
    case OP_NOT:
      if (ot->kind != TY_BOOL)
        err_at(c, e, "! takes bool, got %s", type_name(ot));
      return ty_bool;
    case OP_BITNOT:
      if (!type_is_int(ot))
        err_at(c, e, "~ takes an integer, got %s", type_name(ot));
      return ot;
    case OP_DEREF: {
      if (ot->kind != TY_PTR) {
        err_at(c, e, "cannot dereference %s", type_name(ot));
        return ty_unit;
      }
      return ot->base;
    }
    default:
      err_at(c, e, "bad unary operator");
      return ty_unit;
    }
  }
  case NT_BINARY: {
    Type *lt, *rt;
    // literal adaptation: one side determines for the other
    Node *l = node_get(e->a), *r = node_get(e->b);
    if ((l->kind == NT_INT || l->kind == NT_FLOAT) &&
        !(r->kind == NT_INT || r->kind == NT_FLOAT)) {
      rt = check_expr(c, e->b, NULL);
      lt = check_expr(c, e->a, rt);
    } else if ((r->kind == NT_INT || r->kind == NT_FLOAT) &&
               !(l->kind == NT_INT || l->kind == NT_FLOAT)) {
      lt = check_expr(c, e->a, NULL);
      rt = check_expr(c, e->b, lt);
    } else if (expected && type_is_num(expected) &&
               (l->kind == NT_INT || l->kind == NT_FLOAT) &&
               (r->kind == NT_INT || r->kind == NT_FLOAT)) {
      // two literals under a consumer: the consumer types the pair —
      // `0 - 2147483648` folds into an i32 without either side
      // overflowing on its own
      lt = check_expr(c, e->a, expected);
      rt = check_expr(c, e->b, expected);
    } else {
      lt = check_expr(c, e->a, NULL);
      rt = check_expr(c, e->b, lt);
    }
    int op = e->op;
    if (op == OP_AND || op == OP_OR) {
      if (lt->kind != TY_BOOL || rt->kind != TY_BOOL)
        err_at(c, e, "&&/|| take bool, got %s and %s", type_name(lt),
               type_name(rt));
      return ty_bool;
    }
    bool cmp = op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE ||
               op == OP_GT || op == OP_GE;
    if (!type_eq(lt, rt)) {
      err_at(c, e, "type mismatch: %s vs %s", type_name(lt),
             type_name(rt));
      return cmp ? ty_bool : lt;
    }
    switch (op) {
    case OP_EQ:
    case OP_NE: {
      // the == comparability law
      switch (lt->kind) {
      case TY_BOOL: case TY_F32: case TY_F64: case TY_STRING:
      case TY_I8: case TY_I16: case TY_I32: case TY_I64:
      case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
      case TY_PTR: case TY_WEAK: case TY_STRUCT: case TY_ENUM:
        break; // weak compares identity, like pointers (§10)
      case TY_SLICE:
        err_at(c, e, "slices never compare (write a loop)");
        break;
      case TY_FN:
        err_at(c, e, "fn values never compare");
        break;
      case TY_DYN:
        err_at(c, e, "dyn values never compare");
        break;
      default:
        err_at(c, e, "%s never compares", type_name(lt));
      }
      return ty_bool;
    }
    case OP_LT: case OP_LE: case OP_GT: case OP_GE:
      if (!type_is_num(lt) && lt->kind != TY_STRING)
        err_at(c, e, "ordering needs numbers or strings, got %s",
               type_name(lt));
      return ty_bool;
    default: {
      if (op == OP_ADD && lt->kind == TY_STRING)
        return ty_string; // + concatenates strings
      if (op == OP_MOD && type_is_float(lt)) {
        err_at(c, e, "%% needs integers (floats are IEEE: no remainder "
               "op, §11)");
        return lt;
      }
      if (!(op == OP_BAND || op == OP_BOR || op == OP_BXOR ||
            op == OP_SHL || op == OP_SHR)) {
        if (!type_is_num(lt))
          err_at(c, e, "operator needs numbers, got %s", type_name(lt));
      } else if (!type_is_int(lt)) {
        err_at(c, e, "bitwise operators take integers, got %s",
               type_name(lt));
      }
      return lt;
    }
    }
  }
  case NT_AS: {
    Type *ft = check_expr(c, e->a, NULL);
    GScope dummy = {0};
    Type *tt = check_type_in_ctx(c, e->b, &dummy);
    bool ok = false;
    if (type_is_num(ft) && type_is_num(tt))
      ok = true;
    else if (ft->kind == TY_ENUM && tt->kind == TY_I32)
      ok = true; // enum → tag
    if (!ok)
      err_at(c, e, "as converts between numbers, or enum→i32 tag; "
                   "%s as %s is not a conversion",
             type_name(ft), type_name(tt));
    return tt;
  }
  case NT_NEW: {
    // new T { fields } → *T (heap); generic args from a-wrapper
    Look lk;
    if (!lookup(c, e->name, &lk) || lk.kind != LOOK_STRUCT) {
      // generic param struct instantiation lookup by name in scope
      err_at(c, e, "unknown struct '%s' for new", e->name);
      return ty_unit;
    }
    StructDef *sd = lk.sdef;
    Type **gargs = NULL;
    if (e->a != NO_REF && node_get(e->a)->list) {
      size_t n = reflist_len(node_get(e->a)->list);
      gargs = arena_alloc(g_arena, n * sizeof(Type *), 8);
      GScope dummy = {0};
      for (size_t i = 0; i < n; i++)
        gargs[i] = check_type_in_ctx(c, reflist_at(node_get(e->a)->list, i),
                                     &dummy);
      if (n != sd->ngparams) {
        err_at(c, e, "struct %s takes %zu generic argument(s), got %zu",
               sd->name, sd->ngparams, n);
        gargs = NULL;
      }
    } else if (sd->ngparams) {
      err_at(c, e, "cannot infer the generic arguments of %s here "
                   "— spell them: new %s[T, …]", sd->name, sd->name);
      return ty_unit;
    }
    Type *st = type_struct(sd, gargs, sd->ngparams);
    // field initializers: every field exactly once (pointer fields must
    // initialize — checked here structurally); field types live through
    // the instantiation's binds
    TBind nb = {0};
    if (gargs && sd->gparams) {
      nb.names = sd->gparams;
      nb.tys = gargs;
      nb.n = sd->ngparams;
    }
    RefList *inits = e->b != NO_REF ? node_get(e->b)->list : NULL;
    size_t ninits = inits ? reflist_len(inits) : 0;
    for (size_t i = 0; i < sd->nfields; i++) {
      FieldDef *f = &sd->fields[i];
      Type *fty = nb.n ? tsubst(f->ty, &nb) : f->ty;
      bool found = false;
      for (size_t j = 0; j < ninits; j++) {
        Node *fi = node_get(reflist_at(inits, j));
        if (fi->kind != NT_FIELDINIT)
          continue;
        if (strcmp(fi->name, f->name) != 0)
          continue;
        if (found)
          err_at(c, fi, "field '%s' initialized twice", f->name);
        found = true;
        check_expr(c, fi->a, fty);
      }
      if (!found) {
        // zeroed default only for unmanaged fields
        if (type_is_managed(fty))
          err_at(c, e, "field '%s' (%s) must be initialized", f->name,
                 type_name(fty));
      }
    }
    for (size_t j = 0; j < ninits; j++) {
      Node *fi = node_get(reflist_at(inits, j));
      if (fi->kind != NT_FIELDINIT)
        continue;
      bool known = false;
      for (size_t i = 0; i < sd->nfields; i++)
        if (strcmp(sd->fields[i].name, fi->name) == 0)
          known = true;
      if (!known)
        err_at(c, fi, "struct %s has no field '%s'", sd->name, fi->name);
    }
    return type_ptr(st);
  }
  case NT_SLICE_LIT: {
    // element type from expected or first element
    Type *elem = NULL;
    if (expected && expected->kind == TY_SLICE)
      elem = expected->base;
    if (reflist_len(e->list) > 0) {
      Node *f = node_get(node_get(reflist_at(e->list, 0))->a);
      if (!elem) {
        if (f->kind == NT_INT)
          elem = ty_i32;
        else if (f->kind == NT_FLOAT)
          elem = ty_f64;
        else
          elem = check_expr(c, reflist_at(e->list, 0), NULL);
      }
      for (size_t i = 0; i < reflist_len(e->list); i++)
        check_expr(c, reflist_at(e->list, i), elem);
    }
    if (!elem) {
      err_at(c, e, "empty slice needs a consumer type");
      return ty_unit;
    }
    return type_slice(elem);
  }
  case NT_CLOSURE: {
    // (the checker-annotated fn type carries this closure's sig)
    // params + body; captures checked when used (immutables by copy —
    // enforced because closures see the outer immutable locals)
    GScope dummy = {0};
    FnSig *sig = arena_alloc(g_arena, sizeof(FnSig), 8);
    sig->nparams = reflist_len(e->list);
    sig->params = arena_alloc(g_arena,
                              (sig->nparams ? sig->nparams : 1) *
                                  sizeof(ParamDef), 8);
    for (size_t i = 0; i < sig->nparams; i++) {
      Node *pp = node_get(reflist_at(e->list, i));
      sig->params[i].name = pp->name;
      sig->params[i].ty = check_type_in_ctx(c, pp->a, &dummy);
      sig->params[i].variadic = false;
    }
    sig->ret = e->b != NO_REF ? check_type_in_ctx(c, e->b, &dummy) : ty_unit;
    // body: a nested scope with the closure's params
    ctx_push_scope(c);
    for (size_t i = 0; i < sig->nparams; i++) {
      Local *l = ctx_decl_local(c, sig->params[i].name);
      l->ty = sig->params[i].ty;
      l->mut = false;
    }
    // temporarily retarget the return context for return statements
    Type *saved_ret = c->ret;
    c->ret = sig->ret;
    check_block(c, e->c);
    c->ret = saved_ret;
    ctx_pop_scope(c);
    return type_fn(sig);
  }
  case NT_QMARK: {
    // the operand is Option/Result; the value is the payload; the
    // enclosing return must be compatible (Option: any payload;
    // Result: exact error type)
    Type *ot = check_expr(c, e->a, NULL);
    EnumDef *opt = prelude_enum("Option");
    EnumDef *res = prelude_enum("Result");
    Type *payload = NULL;
    if (is_option_of(ot, opt) && ot->nargs == 1) {
      payload = ot->args[0];
      if (c->ret) {
        if (!(is_option_of(c->ret, opt) && c->ret->nargs == 1))
          err_at(c, e, "? on Option needs the enclosing fn to return "
                       "Option, it returns %s",
                 type_name(c->ret));
      }
    } else if (is_option_of(ot, res) && ot->nargs == 2) {
      payload = ot->args[0];
      if (c->ret) {
        if (!(is_option_of(c->ret, res) && c->ret->nargs == 2 &&
              type_eq(c->ret->args[1], ot->args[1])))
          err_at(c, e, "? on Result needs the enclosing fn to return "
                       "Result with error type %s, it returns %s",
                 type_name(ot->args[1]), type_name(c->ret));
      }
    } else {
      err_at(c, e, "? applies to Option or Result, got %s",
             type_name(ot));
      return ty_unit;
    }
    return payload;
  }
  case NT_IF_EXPR: {
    Type *ct = check_expr(c, e->a, ty_bool);
    if (ct->kind != TY_BOOL)
      err_at(c, e, "if condition is %s, want bool", type_name(ct));
    Type *t = check_block_value(c, e->b, expected);
    if (e->c == NO_REF) {
      err_at(c, e, "if-expression requires else");
      return ty_unit;
    }
    Type *et = check_block_value(c, e->c, expected);
    if (!type_eq(t, et)) {
      err_at(c, e, "if arms disagree: %s vs %s", type_name(t),
             type_name(et));
      return t;
    }
    return t;
  }
  case NT_MATCH_EXPR:
    return check_match(c, er, expected);
  default:
    err_at(c, e, "unexpected expression form");
    return ty_unit;
  }
}

// the value of a block: its tail expression's type (unit if none)
static Type *check_block_value(FnCtx *c, NodeRef br, Type *expected);
static Type *check_if_tail_value(FnCtx *c, Node *s, Type *expected);

// an if in block-final position carries its value (§9); branches are
// block values, else-if chains recurse
static Type *check_if_tail_value(FnCtx *c, Node *s, Type *expected) {
  Type *ct = check_expr(c, s->a, ty_bool);
  if (ct->kind != TY_BOOL)
    err_at(c, s, "if condition is %s, want bool", type_name(ct));
  if (s->c == NO_REF) {
    err_at(c, s, "if-expression requires else");
    s->sem = ty_unit;
    return ty_unit;
  }
  ctx_push_scope(c);
  Type *t = check_block_value(c, s->b, expected);
  ctx_pop_scope(c);
  ctx_push_scope(c);
  Type *et;
  if (node_get(s->c)->kind == NT_IF)
    et = check_if_tail_value(c, node_get(s->c), expected);
  else
    et = check_block_value(c, s->c, expected);
  ctx_pop_scope(c);
  if (!type_eq(t, et)) {
    err_at(c, s, "if arms disagree: %s vs %s", type_name(t),
           type_name(et));
    s->sem = t;
    return t;
  }
  s->sem = t; // the value the emitter reads
  return t;
}

// a block in value position: one scoped walk; the final statement may
// be a bare expression, an if, or a match — it types the block
static Type *check_block_value(FnCtx *c, NodeRef br, Type *expected) {
  Node *b = node_get(br);
  Type *tail_ty = ty_unit;
  ctx_push_scope(c);
  size_t n = reflist_len(b->list);
  for (size_t i = 0; i < n; i++) {
    bool last = i + 1 == n;
    Node *sn = node_get(reflist_at(b->list, i));
    if (!last) {
      check_stmt(c, reflist_at(b->list, i));
      continue;
    }
    if (sn->kind == NT_EXPRSTMT && sn->bval) {
      tail_ty = check_expr(c, sn->a, expected);
    } else if (sn->kind == NT_IF && sn->op != 1 && sn->c != NO_REF) {
      tail_ty = check_if_tail_value(c, sn, expected);
    } else if (sn->kind == NT_MATCH_EXPR) {
      tail_ty = check_match(c, reflist_at(b->list, i), expected);
    } else {
      check_stmt(c, reflist_at(b->list, i));
    }
  }
  b->sem = tail_ty;
  ctx_pop_scope(c);
  return tail_ty;
}

static Type *check_match(FnCtx *c, NodeRef er, Type *expected) {
  Node *m = node_get(er);
  Type *st = check_expr(c, m->a, NULL);
  Type *arm_ty = NULL;
  bool saw_wild = false;

  if (st->kind == TY_ENUM) {
    // exhaustive unless _ present: cover every variant
    bool *covered = arena_alloc(
        g_arena, (st->edef->nvariants ? st->edef->nvariants : 1), 1);
    memset(covered, 0, st->edef->nvariants ? st->edef->nvariants : 1);
    for (size_t i = 0; i < reflist_len(m->list); i++) {
      Node *arm = node_get(reflist_at(m->list, i));
      Node *pat = node_get(arm->a);
      if (pat->kind == NT_PWILD)
        saw_wild = true;
      if (pat->kind == NT_PVAR) {
        for (size_t v = 0; v < st->edef->nvariants; v++)
          if (strcmp(st->edef->variants[v].name, pat->name +
                         (strchr(pat->name, '.')
                              ? (size_t)(strchr(pat->name, '.') - pat->name + 1)
                              : 0)) == 0) {
            // prefix check: pat->name is "Enum.Variant"
            const char *dot = strchr(pat->name, '.');
            if (dot && strcmp(dot + 1, st->edef->variants[v].name) == 0)
              covered[v] = true;
          }
      }
    }
    if (!saw_wild)
      for (size_t v = 0; v < st->edef->nvariants; v++)
        if (!covered[v]) {
          err_at(c, m, "match on %s is not exhaustive: missing %s.%s",
                 st->edef->name, st->edef->name,
                 st->edef->variants[v].name);
        }
  }

  for (size_t i = 0; i < reflist_len(m->list); i++) {
    Node *arm = node_get(reflist_at(m->list, i));
    Node *pat = node_get(arm->a);
    if (pat->kind == NT_PWILD)
      saw_wild = true;
    // pattern binders scope the arm value
    ctx_push_scope(c);
    check_pattern(c, pat, st);
    Type *vt;
    if (node_get(arm->b)->kind == NT_EXPRSTMT && node_get(arm->b)->op == 3)
      vt = check_block_value(c, arm->b, expected);
    else
      vt = check_expr(c, arm->b, expected);
    if (!arm_ty)
      arm_ty = vt;
    else if (!type_eq(arm_ty, vt)) {
      err_at(c, node_get(arm->b), "match arms disagree: %s vs %s",
             type_name(arm_ty), type_name(vt));
    }
    ctx_pop_scope(c);
  }
  return arm_ty ? arm_ty : ty_unit;
}

static void check_pattern(FnCtx *c, Node *p, Type *st) {
  switch (p->kind) {
  case NT_PWILD:
    return;
  case NT_PBIND: {
    // duplicate binder check within this pattern is by scope: same name
    // twice in one scope = error
    if (ctx_find_local(c, p->name) &&
        ctx_find_local(c, p->name)->scope == c->scope) {
      err_at(c, p, "binder '%s' appears twice", p->name);
      return;
    }
    Local *l = ctx_decl_local(c, p->name);
    l->ty = st;
    l->mut = false;
    l->decl = NO_REF;
    p->sem = st; // the emitter reads the binder's type
    return;
  }
  case NT_PLIT:
    return; // literal pattern (values checked against subject in emit)
  case NT_PVAR: {
    // Enum.Variant pattern: must be a variant of the subject enum
    if (st->kind != TY_ENUM) {
      err_at(c, p, "variant pattern on non-enum %s", type_name(st));
      return;
    }
    const char *dot = strchr(p->name, '.');
    if (!dot) {
      err_at(c, p, "variant patterns are written Enum.Variant");
      return;
    }
    if (strncmp(p->name, st->edef->name, (size_t)(dot - p->name)) != 0) {
      err_at(c, p, "pattern %s does not belong to enum %s", p->name,
             st->edef->name);
      return;
    }
    TBind tb = {edef_gnames(st->edef), st->args,
                 st->args ? st->edef->ngparams : 0};
    for (size_t v = 0; v < st->edef->nvariants; v++) {
      EnumVariant *var = &st->edef->variants[v];
      if (strcmp(dot + 1, var->name) != 0)
        continue;
      if ((var->form == VAR_UNIT) != (reflist_len(p->list) == 0)) {
        err_at(c, p, "variant %s.%s takes %zu payload field(s)",
               st->edef->name, var->name, var->nfields);
        return;
      }
      for (size_t i = 0; i < reflist_len(p->list); i++) {
        Node *sub = node_get(reflist_at(p->list, i));
        if (p->op == VAR_STRUCT) {
          // named binder: sub is NT_FIELD(name, a=pattern)
          bool found = false;
          for (size_t k = 0; k < var->nfields; k++)
            if (strcmp(var->fields[k].name, sub->name) == 0) {
              check_pattern(c, node_get(sub->a),
                            tsubst(var->fields[k].ty, &tb));
              found = true;
            }
          if (!found)
            err_at(c, sub, "variant %s has no payload field '%s'",
                   var->name, sub->name);
        } else if (sub->kind == NT_FIELD) {
          err_at(c, sub, "tuple variants bind positionally");
        } else {
          check_pattern(c, sub,
                        var->nfields > i ? tsubst(var->fields[i].ty, &tb)
                                         : ty_unit);
        }
      }
      return;
    }
    err_at(c, p, "enum %s has no variant '%s'", st->edef->name, dot + 1);
    return;
  }
  default:
    err_at(c, p, "bad pattern");
  }
}

// the public entry: annotate every expression node with its type so
// the emitter reads types straight off the tree
static Type *check_expr(FnCtx *c, NodeRef er, Type *expected) {
  Type *t = check_expr_inner(c, er, expected);
  if (er != NO_REF)
    node_get(er)->sem = t;
  // dyn coercion at every expected-type site: a *T whose T satisfies
  // the trait becomes the fat value {vtable, obj} (§ traits). The
  // node's sem keeps the NATURAL pointer type — the emitter wraps at
  // the consumer when it sees a dyn destination under a pointer sem.
  if (expected && expected->kind == TY_DYN && t && t->kind == TY_PTR &&
      t->base->kind == TY_STRUCT && er != NO_REF) {
    if (trait_satisfied(c, t, expected->tdef))
      return expected;
    err_at(c, node_get(er),
           "%s does not satisfy %s (a dyn coercion needs every trait "
           "method)",
           t->base->sdef->name, expected->tdef->name);
  }
  return t;
}

// ---------------------------------------------------------------- stmts

static void check_assign_target(FnCtx *c, Node *lv) {
  switch (lv->kind) {
  case NT_PATH: {
    Look lk;
    if (!lookup(c, lv->name, &lk)) {
      err_at(c, lv, "unknown name '%s'", lv->name);
      return;
    }
    if (lk.kind == LOOK_STATIC)
      return; // module-lifetime mutable state
    if (lk.kind != LOOK_LOCAL) {
      err_at(c, lv, "cannot assign to %s '%s'",
             lk.kind == LOOK_CONST ? "const" : "non-local", lv->name);
      return;
    }
    if (!lk.local->mut)
      err_at(c, lv, "'%s' is immutable (declare with let mut)", lv->name);
    return;
  }
  case NT_FIELD_E: {
    Type *bt = check_expr(c, lv->a, NULL);
    if (bt->kind == TY_PTR)
      return; // the pointee is mutable through a pointer
    if (bt->kind != TY_STRUCT) {
      err_at(c, lv, "cannot assign through %s", type_name(bt));
      return;
    }
    check_assign_target_base(c, lv->a);
    return;
  }
  case NT_INDEX:
    // element stores through a slice mutate the view, not the binding
    check_expr(c, lv->a, NULL);
    check_expr(c, lv->b, ty_usize);
    return;
  default:
    err_at(c, lv, "invalid assignment target");
  }
}

// mutability for a path of field/index: the base must be a mutable
// binding or reached through a pointer (pointers grant mutability)
static void check_assign_target_base(FnCtx *c, NodeRef base) {
  Node *b = node_get(base);
  if (b->kind == NT_PATH) {
    Look lk;
    if (lookup(c, b->name, &lk) && lk.kind == LOOK_LOCAL && !lk.local->mut)
      err_at(c, b, "'%s' is immutable", b->name);
    return;
  }
  // through a pointer the pointee is mutable; nothing more to check
  if (b->kind == NT_UNARY && b->op == OP_DEREF)
    return;
}

static void check_stmt(FnCtx *c, NodeRef sr) {
  Node *s = node_get(sr);
  switch (s->kind) {
  case NT_LET: {
    // inner shadowing allowed (a new scope level or a deeper block);
    // same-scope rebind is an error
    if (ctx_find_local(c, s->name) &&
        ctx_find_local(c, s->name)->scope == c->scope) {
      err_at(c, s, "'%s' is already bound in this scope "
                   "(inner shadowing needs a deeper scope)",
             s->name);
    }
    // root build params may never be shadowed
    if (g_entry_mod && symtab_get(g_entry_mod->syms, s->name)) {
      err_at(c, s, "'%s' shadows a root build parameter", s->name);
    }
    Type *ann = NULL;
    if (s->a != NO_REF)
      ann = check_type_in_ctx(c, s->a, NULL);
    Type *it = check_expr(c, s->b, ann);
    Local *l = ctx_decl_local(c, s->name);
    l->mut = s->bval;
    l->ty = ann ? ann : it;
    l->decl = sr;
    node_get(sr)->sem = l->ty; // the emitter allocates from this face
    if (ann && !type_eq(ann, it)) {
      // literal-style adaptation is exact (check_expr applied the
      // consumer); a real mismatch is an error
      err_at(c, s, "let %s: %s = … (value is %s)", s->name,
             type_name(ann), type_name(it));
    }
    return;
  }
  case NT_ASSIGN: {
    if (s->a == NO_REF)
      check_assign_target(c, node_get(s->b)); // defer assignment form
    else
      check_assign_target(c, node_get(s->a));
    Type *lt = s->a != NO_REF ? check_expr(c, s->a, NULL) : NULL;
    Type *vt = check_expr(c, s->b, lt);
    if (lt && !type_eq(lt, vt)) {
      if (s->op == OP_NONE)
        err_at(c, s, "assignment type mismatch: %s vs %s", type_name(lt),
               type_name(vt));
      else
        err_at(c, s, "compound assignment needs %s, got %s",
               type_name(lt), type_name(vt));
    }
    return;
  }
  case NT_IF: {
    if (s->op == 1) {
      // comptime-folded at load: only the live branch exists
      if (s->ival) {
        ctx_push_scope(c);
        check_block(c, s->b);
        ctx_pop_scope(c);
      } else if (s->c != NO_REF) {
        ctx_push_scope(c);
        if (node_get(s->c)->kind == NT_IF)
          check_stmt(c, s->c);
        else
          check_block(c, s->c);
        ctx_pop_scope(c);
      }
      return;
    }
    Type *ct = check_expr(c, s->a, ty_bool);
    if (ct->kind != TY_BOOL)
      err_at(c, s, "if condition is %s, want bool", type_name(ct));
    ctx_push_scope(c);
    check_block(c, s->b);
    if (s->c != NO_REF) {
      ctx_push_scope(c);
      if (node_get(s->c)->kind == NT_IF)
        check_stmt(c, s->c);
      else
        check_block(c, s->c);
      ctx_pop_scope(c);
    }
    ctx_pop_scope(c);
    return;
  }
  case NT_WHILE: {
    Type *ct = check_expr(c, s->a, ty_bool);
    if (ct->kind != TY_BOOL)
      err_at(c, s, "while condition is %s, want bool", type_name(ct));
    ctx_push_scope(c);
    check_block(c, s->b);
    ctx_pop_scope(c);
    return;
  }
  case NT_LOOP: {
    ctx_push_scope(c);
    check_block(c, s->b);
    ctx_pop_scope(c);
    return;
  }
  case NT_MATCH: {
    // statement position: the value is discarded
    check_match(c, sr, NULL);
    return;
  }
  case NT_RETURN: {
    if (s->a == NO_REF) {
      if (c->ret && c->ret->kind != TY_UNIT)
        err_at(c, s, "return needs a %s value", type_name(c->ret));
      return;
    }
    Type *vt = check_expr(c, s->a, c->ret);
    if (c->ret && !type_eq(vt, c->ret))
      err_at(c, s, "return type mismatch: fn returns %s, got %s",
             type_name(c->ret), type_name(vt));
    return;
  }
  case NT_DEFER: {
    if (node_get(s->a)->kind == NT_ASSIGN)
      check_stmt(c, s->a);
    else
      check_expr(c, s->a, NULL);
    return;
  }
  case NT_BREAK:
  case NT_CONTINUE: {
    if (s->name) {
      for (size_t i = 0; i < VLEN(c->labels); i++)
        if (strcmp(*VAT(c->labels, const char *, i), s->name) == 0)
          return;
      err_at(c, s, "unknown label '%s'", s->name);
    }
    return;
  }
  case NT_EXPRSTMT:
    if (s->op == 3) {
      check_block(c, sr);
      return;
    }
    check_expr(c, s->a, NULL);
    return;
  default:
    err_at(c, s, "unexpected statement");
  }
}

static void collect_labels(FnCtx *c, NodeRef br) {
  Node *b = node_get(br);
  if (b->kind != NT_EXPRSTMT || b->op != 3)
    return;
  for (size_t i = 0; i < reflist_len(b->list); i++) {
    NodeRef sr = reflist_at(b->list, i);
    Node *s = node_get(sr);
    if ((s->kind == NT_WHILE || s->kind == NT_LOOP) && s->name) {
      for (size_t j = 0; j < VLEN(c->labels); j++)
        if (strcmp(*VAT(c->labels, const char *, j), s->name) == 0)
          err_at(c, s, "label '%s' declared twice in this function",
                 s->name);
      *VPUSH(c->labels, const char *) = s->name;
      collect_labels(c, s->b);
    } else if (s->kind == NT_IF) {
      collect_labels(c, s->b);
      if (s->c != NO_REF)
        collect_labels(c, s->c);
    }
  }
}

static void check_block(FnCtx *c, NodeRef br) {
  Node *b = node_get(br);
  ctx_push_scope(c);
  for (size_t i = 0; i < reflist_len(b->list); i++)
    check_stmt(c, reflist_at(b->list, i));
  ctx_pop_scope(c);
}

// ---------------------------------------------------------------- bodies

// check one function body (shared by the module walk and generic
// instance checking)
static void check_fn_body(Program *p, FnDef *f) {
  (void)p;
  Module *m = f->mod;
  if (f->body == NO_REF || !f->sig)
    return;
  FnCtx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.fn = f;
  ctx.mod = m;
  vec_init(&ctx.locals, sizeof(Local));
  vec_init(&ctx.labels, sizeof(const char *));
  ctx.ret = f->sig->ret;
  ctx.gparams = f->gparams;
  ctx.ngparams = f->ngparams;
  ctx.gbinds = f->ibinds; // instances: names map to concrete binds
  for (size_t i = 0; i < f->sig->nparams; i++) {
    Local *l = ctx_decl_local(&ctx, f->sig->params[i].name);
    l->ty = f->sig->params[i].ty ? f->sig->params[i].ty : ty_unit;
    l->mut = false;
    l->decl = f->sig->params[i].decl;
  }
  collect_labels(&ctx, f->body);
  check_block(&ctx, f->body);
}

bool check_bodies(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN)
        continue;
      for (FnDef *f = s->u.fns; f; f = f->next_overload) {
        // generic templates are checked per instantiation (their bodies
        // only make sense with concrete binds — the design's law)
        if (f->ngparams)
          continue;
        check_fn_body(p, f);
      }
    }
  }
  // the entry module must define main
  Sym *main = p->entry->syms ? symtab_get(p->entry->syms, "main") : NULL;
  if (!main || main->kind != SYM_FN) {
    diag_at(DIAG_ERROR, p->entry->path, 0, 0,
            "the entry module must define fn main");
  }
  return !g_had_error;
}

// public shim (sem.h): TBind is checker-internal
Type *tsubst(Type *t, void *b) { return tsubst_impl(t, (TBind *)b); }

// ============================================================ generics

// monomorphize a generic call: bind gparams from argument types (and
// the expected type through the return), reuse identical instances
FnDef *instantiate_generic(FnCtx *c, FnDef *f, Node *call, Type *expected) {
  return instantiate_generic_seeded(c, f, call, expected, NULL);
}

static FnDef *instantiate_generic_seeded(FnCtx *c, FnDef *f, Node *call,
                                         Type *expected, Type **seed) {
  const char **names = f->gparams;
  size_t ng = f->ngparams;
  Type **binds = arena_alloc(g_arena, ng * sizeof(Type *), 8);
  memset(binds, 0, ng * sizeof(Type *));
  // seeded binds win (a method's implicit params come from the receiver
  // instantiation; plain fns pass NULL)
  TBind tb0 = {names, binds, ng};
  if (seed)
    for (size_t b = 0; b < ng; b++)
      binds[b] = seed[b];
  // bind from the expected type through the return FIRST (a literal
  // argument has no type of its own until the consumer is known)
  Type *rt = f->sig->ret;
  if (rt && expected && ty_has_param(rt))
    tunify(rt, expected, &tb0);
  // bind from params/args by structural unification (literals defer to
  // the re-check — they have no independent type)
  for (size_t i = 0; i < f->sig->nparams && i < reflist_len(call->list);
       i++) {
    Type *pt = f->sig->params[i].ty;
    if (!pt || !ty_has_param(pt))
      continue;
    Node *aw = node_get(reflist_at(call->list, i));
    if (aw->kind != NT_POSARG)
      continue;
    Node *arg = node_get(aw->a);
    if (arg->kind == NT_INT || arg->kind == NT_FLOAT ||
        (arg->kind == NT_UNARY && arg->op == OP_NEG &&
         (node_get(arg->a)->kind == NT_INT ||
          node_get(arg->a)->kind == NT_FLOAT)))
      continue; // literal: adapted by the post-instantiation re-check
    Type *at = check_expr(c, aw->a, NULL);
    tunify(pt, at, &tb0);
  }
  for (size_t b = 0; b < ng; b++) {
    if (binds[b])
      continue;
    // literals bind their parameter at the DEFAULT type (§3.1: no
    // consumer → i32/f64) — the unification pass skipped them on
    // purpose so an expected type could bind first
    for (size_t i = 0; !binds[b] && i < f->sig->nparams &&
                       i < reflist_len(call->list);
         i++) {
      Type *pt = f->sig->params[i].ty;
      if (!pt || pt->kind != TY_PARAM ||
          strcmp(names[b], pt->pname) != 0)
        continue;
      Node *aw = node_get(reflist_at(call->list, i));
      if (aw->kind != NT_POSARG)
        continue;
      Node *arg = node_get(aw->a);
      Type *dt = NULL;
      if (arg->kind == NT_INT)
        dt = ty_i32;
      else if (arg->kind == NT_FLOAT)
        dt = ty_f64;
      else if (arg->kind == NT_UNARY && arg->op == OP_NEG) {
        Node *op0 = node_get(arg->a);
        dt = op0->kind == NT_INT    ? ty_i32
             : op0->kind == NT_FLOAT ? ty_f64
                                     : NULL;
      }
      if (dt)
        binds[b] = dt;
    }
    if (!binds[b]) {
      err_at(c, call, "cannot infer generic parameter '%s' of %s",
             names[b], f->name);
      binds[b] = ty_i32;
    }
  }
  // bounds verified per instantiation (§3.8): every [T: Trait] checks
  // the concrete bind carries the trait's methods
  if (f->decl != NO_REF) {
    Node *d = node_get(f->decl);
    if (d->kind == NT_FN && d->a != NO_REF &&
        node_get(d->a)->list) {
      RefList *gps = node_get(d->a)->list;
      for (size_t i = 0; i < reflist_len(gps); i++) {
        Node *gp = node_get(reflist_at(gps, i));
        if (!gp->list)
          continue;
        for (size_t b = 0; b < ng; b++)
          if (names[b] && strcmp(names[b], gp->name) == 0) {
            for (size_t k = 0; k < reflist_len(gp->list); k++) {
              const char *bn = node_get(reflist_at(gp->list, k))->name;
              TraitDef *td = find_trait(c->mod, bn);
              if (!td) {
                err_at(c, call, "unknown trait '%s' in the bounds of %s",
                       bn, f->name);
                continue;
              }
              if (!trait_satisfied(c, binds[b], td))
                err_at(c, call,
                       "%s does not satisfy %s: %s bound to %s here",
                       f->name, bn, names[b], type_name(binds[b]));
            }
            break;
          }
      }
    }
  }
  // reuse identical instantiations (structural binds)
  for (FnDef *g = f->instances; g; g = g->next_instance) {
    bool same = true;
    for (size_t b = 0; b < ng && same; b++)
      if (!type_eq(g->ibinds[b], binds[b]))
        same = false;
    if (same)
      return g;
  }
  // build the instance
  FnDef *inst = arena_alloc(g_arena, sizeof(FnDef), 8);
  memset(inst, 0, sizeof(FnDef));
  inst->name = aprintf(g_arena, "%s__i%zu", f->name,
                       (size_t)(f->instances ? (size_t)1 : (size_t)0) +
                           (size_t)0);
  // count existing instances for a stable name
  size_t ninst = 0;
  for (FnDef *g = f->instances; g; g = g->next_instance)
    ninst++;
  inst->name = aprintf(g_arena, "%s__i%zu", f->name, ninst);
  inst->mod = f->mod;
  inst->decl = f->decl;
  inst->body = f->body;
  inst->is_pub = f->is_pub;
  inst->sig = arena_alloc(g_arena, sizeof(FnSig), 8);
  inst->sig->nparams = f->sig->nparams;
  inst->sig->params = arena_alloc(
      g_arena, (inst->sig->nparams ? inst->sig->nparams : 1) *
                   sizeof(ParamDef), 8);
  TBind tb = {names, binds, ng};
  for (size_t i = 0; i < inst->sig->nparams; i++) {
    inst->sig->params[i].name = f->sig->params[i].name;
    inst->sig->params[i].decl = f->sig->params[i].decl;
    inst->sig->params[i].ty = tsubst(f->sig->params[i].ty, &tb);
    inst->sig->params[i].variadic = f->sig->params[i].variadic;
  }
  inst->sig->ret = tsubst(f->sig->ret, &tb);
  inst->ibinds = binds;
  // names stay for instance-body type resolution (check_fn_body maps
  // them through ibinds); nimplicit=0 marks "fully instantiated" so no
  // call site ever re-instantiates an instance
  inst->gparams = f->gparams;
  inst->ngparams = f->ngparams;
  inst->nimplicit = 0;
  // append (creation order)
  if (!f->instances) {
    f->instances = inst;
  } else {
    FnDef *t = f->instances;
    while (t->next_instance)
      t = t->next_instance;
    t->next_instance = inst;
  }
  // per-instance body copy: node annotations (types) must not be
  // shared across instantiations
  extern NodeRef clone_node_tree(NodeRef r);
  inst->body = clone_node_tree(f->body);
  inst->sig = inst->sig; // sig built above
  { extern Program *g_program_ctx; check_fn_body(g_program_ctx, inst); }
  return inst;
}

// eager impl-block check (§3.5): impl Trait for T requires T to carry
// every trait method with a matching signature — checked the moment
// the impl exists, not at the first call
void check_impls(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    if (!m->decls)
      continue;
    for (size_t i = 0; i < reflist_len(m->decls); i++) {
      Node *d = node_get(reflist_at(m->decls, i));
      if (d->kind != NT_IMPL)
        continue;
      TraitDef *td = find_trait(m, d->name);
      if (!td) {
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "impl of unknown trait '%s'", d->name);
        continue;
      }
      Type *tgt = resolve_type_pub(m, d->b, NULL);
      if (!tgt)
        continue;
      FnCtx ctx;
      memset(&ctx, 0, sizeof ctx);
      ctx.mod = m;
      if (!trait_satisfied(&ctx, tgt, td))
        diag_at(DIAG_ERROR, m->path, d->line, d->col,
                "impl %s for %s: the type does not carry every trait "
                "method with a matching signature",
                d->name, type_name(tgt));
    }
  }
}
