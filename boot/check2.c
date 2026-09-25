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
  // prelude last
  if (g_prelude_mod) {
    Sym *ps = symtab_get(g_prelude_mod->syms, name);
    if (ps) {
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
// enclosing fn are in scope)
static Type *check_type_in_ctx(FnCtx *c, NodeRef tr, GScope *g) {
  (void)g;
  extern Type *resolve_type_pub(Module *, NodeRef, GScope *);
  GScope gs = {c->gparams, c->ngparams, NULL};
  return resolve_type_pub(c->mod, tr, &gs);
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
    Node *arg = node_get(a->a);
    if (arg->kind == NT_INT || arg->kind == NT_FLOAT) {
      if (!literal_adapts_to(c, arg, pt))
        return false;
      continue;
    }
    Type *at = check_expr(c, a->a, pt);
    if (!type_eq(at, pt))
      return false;
  }
  return true;
}

// resolve a call to one of an overload chain: exact-match-unique
static FnDef *resolve_overload(FnCtx *c, FnDef *chain, NodeRef call_r,
                               bool has_recv, const char *what) {
  FnDef *match = NULL;
  int nmatch = 0;
  for (FnDef *f = chain; f; f = f->next_overload) {
    if (sig_matches(c, f->sig, call_r, has_recv)) {
      match = f;
      nmatch++;
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
  const char *tn = t->kind == TY_STRUCT    ? t->sdef->name
                   : t->kind == TY_ENUM    ? t->edef->name
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
  // extension methods participate only from the use closure
  for (size_t i = 0; i < VLEN(c->mod->uses); i++) {
    UseBind *ub = VAT(c->mod->uses, UseBind, i);
    Module *um = ub->target;
    if (!um->syms)
      continue;
    for (Sym *s = um->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN || strcmp(s->name, name) != 0 || !s->pub)
        continue;
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
static bool is_intrinsic_fn(const char *n) {
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
    default:
      err_at(c, node_get(aw->a),
             "value of type %s is not printable (aggregates are not)",
             type_name(vt));
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
  if (lk.kind == LOOK_FN) {
    // intrinsics
    if (is_intrinsic_fn(callee->name) &&
        strcmp(callee->name, "len") != 0 && lk.fns->mod == g_prelude_mod) {
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
        return f->sig->ret;
      }
      err_at(c, m, "module %s has no public fn '%s'", recv->name, m->name);
      return ty_unit;
    }
  }

  // real method call on a value
  Type *rt = check_expr(c, m->a, NULL);
  size_t ncand = 0;
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
  for (size_t i = 0; i < ncand; i++) {
    FnDef *f = cands[i];
    // arity/type check with literal adaptation (params[0] is self)
    size_t nparams = f->sig->nparams;
    size_t nargv = reflist_len(m->list);
    bool variadic = nparams > 0 && f->sig->params[nparams - 1].variadic;
    size_t nfixed = variadic ? nparams - 1 : nparams;
    bool ok = variadic ? nargv + 1 >= nfixed : nargv + 1 == nfixed;
    for (size_t j = 0; ok && j < nargv; j++) {
      Node *aw = node_get(reflist_at(m->list, j));
      if (aw->kind != NT_POSARG) {
        ok = false;
        break;
      }
      Type *pt = j < nfixed ? f->sig->params[j + 1].ty
                            : f->sig->params[nfixed].ty;
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
        return bt->sdef->fields[i].ty;
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
      // -literal: the consumer type flows through the negation
      Type *t = check_expr(c, e->a, expected);
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
      case TY_PTR: case TY_STRUCT: case TY_ENUM:
        break;
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
    }
    Type *st = type_struct(sd, gargs, sd->ngparams);
    // field initializers: every field exactly once (pointer fields must
    // initialize — checked here structurally)
    RefList *inits = e->b != NO_REF ? node_get(e->b)->list : NULL;
    size_t ninits = inits ? reflist_len(inits) : 0;
    for (size_t i = 0; i < sd->nfields; i++) {
      FieldDef *f = &sd->fields[i];
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
        check_expr(c, fi->a, f->ty);
      }
      if (!found) {
        // zeroed default only for unmanaged fields
        if (type_is_managed(f->ty))
          err_at(c, e, "field '%s' (%s) must be initialized", f->name,
                 type_name(f->ty));
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
static Type *check_block_value(FnCtx *c, NodeRef br, Type *expected) {
  check_block(c, br);
  Node *b = node_get(br);
  if (b->kind == NT_EXPRSTMT && b->op == 3 && reflist_len(b->list) > 0) {
    NodeRef last = reflist_at(b->list, reflist_len(b->list) - 1);
    Node *ln = node_get(last);
    if (ln->kind == NT_EXPRSTMT && ln->bval) {
      // tail: re-check with the expected type so literals adapt — the
      // first pass typed it without one; literals default safely, so
      // the value type is whatever it produced
      return check_expr(c, ln->a, expected);
    }
  }
  return ty_unit;
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

bool check_bodies(Program *p) {
  for (Module *m = p->modules; m; m = m->next) {
    for (Sym *s = m->syms->order_head; s; s = s->order_next) {
      if (s->kind != SYM_FN)
        continue;
      for (FnDef *f = s->u.fns; f; f = f->next_overload) {
        if (f->body == NO_REF)
          continue;
        if (!f->sig) {
          // trait signatures have no bodies; safety net
          continue;
        }
        FnCtx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.fn = f;
        ctx.mod = m;
        vec_init(&ctx.locals, sizeof(Local));
        vec_init(&ctx.labels, sizeof(const char *));
        ctx.ret = f->sig->ret;
        ctx.gparams = f->gparams;
        ctx.ngparams = f->ngparams;
        // params live in the outermost scope of the body
        for (size_t i = 0; i < f->sig->nparams; i++) {
          // bare self in trait sig has NULL type; method self types
          // resolved at signature build
          Local *l = ctx_decl_local(&ctx, f->sig->params[i].name);
          l->ty = f->sig->params[i].ty ? f->sig->params[i].ty : ty_unit;
          l->mut = false;
          l->decl = f->sig->params[i].decl;
        }
        collect_labels(&ctx, f->body);
        check_block(&ctx, f->body);
        // impl members: verify their self param exists
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
