// fmt.c — the canonical formatter (T1.9). One true spelling per
// construct: 2-space indent, trailing commas on multiline lists,
// braces on every block. fmt output is a parse fixed point.
#include "sem.h"

#include <stdarg.h>

static Node *NG(NodeRef r) {
  return r == NO_REF ? NULL : node_get(r);
}

typedef struct F {
  char *p;
  size_t n, cap;
  int depth;
} F;

static void fneed(F *f, size_t k) {
  if (f->n + k > f->cap) {
    while (f->cap < f->n + k)
      f->cap *= 2;
    f->p = realloc(f->p, f->cap);
  }
}

static void fp(F *f, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *buf = malloc((size_t)n + 1);
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  fneed(f, (size_t)n);
  memcpy(f->p + f->n, buf, (size_t)n);
  f->n += (size_t)n;
  free(buf);
}

static void findent(F *f) {
  for (int i = 0; i < f->depth; i++)
    fp(f, "  ");
}

static void fmt_type(F *f, Node *t);
static void fmt_expr(F *f, Node *e);
static void fmt_stmt(F *f, Node *s);
static void fmt_pattern(F *f, Node *p);

static void fmt_type(F *f, Node *t) {
  if (!t)
    return;
  switch (t->kind) {
  case NT_BUILTIN:
    fp(f, "%s", t->name);
    return;
  case NT_PTR:
    fp(f, "*");
    fmt_type(f, NG(t->a));
    return;
  case NT_OPT:
    fp(f, "?");
    fmt_type(f, NG(t->a));
    return;
  case NT_SLICE:
    fp(f, "[]");
    fmt_type(f, NG(t->a));
    return;
  case NT_DYN:
    fp(f, "dyn %s", t->name);
    return;
  case NT_APP:
    fp(f, "%s", t->name);
    if (t->list && reflist_len(t->list)) {
      fp(f, "[");
      for (size_t i = 0; i < reflist_len(t->list); i++) {
        if (i)
          fp(f, ", ");
        fmt_type(f, NG(reflist_at(t->list, i)));
      }
      fp(f, "]");
    }
    return;
  case NT_FNTYPE:
    fp(f, "fn(");
    for (size_t i = 0; i < reflist_len(t->list); i++) {
      if (i)
        fp(f, ", ");
      fmt_type(f, NG(reflist_at(t->list, i)));
    }
    fp(f, ")");
    if (t->a != NO_REF) {
      fp(f, " -> ");
      fmt_type(f, NG(t->a));
    }
    return;
  default:
    fp(f, "?type");
  }
}

static void fmt_strlit(F *f, Str s) {
  // single-line quoted form with escapes; triple-quoted stays verbatim
  fp(f, "\"");
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    switch (c) {
    case '\n': fp(f, "\\n"); break;
    case '\r': fp(f, "\\r"); break;
    case '\t': fp(f, "\\t"); break;
    case '\\': fp(f, "\\\\"); break;
    case '"': fp(f, "\\\""); break;
    default:
      if (c < 0x20 || c == 0x7f)
        fp(f, "\\x%02x", c);
      else
        fp(f, "%c", c);
    }
  }
  fp(f, "\"");
}

// the receiver's written type is redundant when it spells exactly the
// derived form: *Recv for a struct/enum target, Recv itself for a
// builtin primitive (§18/T3.10) — the typed form is accepted, never
// required, and fmt canonicalizes it away
static bool recv_annotation_derived(Node *t, const char *recv) {
  if (!recv)
    return false;
  // a named type parses as NT_APP (bare name: list == NULL)
  if (t->kind == NT_PTR && t->a != NO_REF &&
      NG(t->a)->kind == NT_APP && !NG(t->a)->list &&
      strcmp(NG(t->a)->name, recv) == 0)
    return true;
  if (t->kind == NT_BUILTIN && strcmp(t->name, recv) == 0)
    return true;
  return false;
}

static void fmt_params(F *f, RefList *ps, const char *recv) {
  fp(f, "(");
  for (size_t i = 0; i < reflist_len(ps); i++) {
    Node *p = NG(reflist_at(ps, i));
    if (i)
      fp(f, ", ");
    if (p->bval)
      fp(f, "mut ");
    if (p->a != NO_REF && !(i == 0 && p->name &&
                            strcmp(p->name, "self") == 0 &&
                            recv_annotation_derived(NG(p->a), recv))) {
      fp(f, "%s: ", p->name);
      fmt_type(f, NG(p->a));
    } else {
      fp(f, "%s", p->name);
    }
    if (p->op == 2)
      fp(f, "...");
  }
  fp(f, ")");
}

static void fmt_block(F *f, Node *b) {
  fp(f, " {\n");
  f->depth++;
  for (size_t i = 0; i < reflist_len(b->list); i++) {
    Node *s = NG(reflist_at(b->list, i));
    findent(f);
    fmt_stmt(f, s);
    fp(f, "\n");
  }
  f->depth--;
  findent(f);
  fp(f, "}");
}

// block-bodied arms print inline; the trailing comma follows the arm
static void fmt_arm(F *f, Node *arm) {
  fmt_pattern(f, NG(arm->a));
  if (arm->c != NO_REF) {
    fp(f, " if ");
    fmt_expr(f, NG(arm->c));
  }
  fp(f, " => ");
  Node *v = NG(arm->b);
  if (v->kind == NT_EXPRSTMT && v->op == 3)
    fmt_block(f, v);
  else
    fmt_expr(f, v);
  fp(f, ",");
}

static void fmt_match(F *f, Node *m) {
  fmt_expr(f, NG(m->a));
  fp(f, " {\n");
  f->depth++;
  for (size_t i = 0; i < reflist_len(m->list); i++) {
    findent(f);
    fmt_arm(f, NG(reflist_at(m->list, i)));
    fp(f, "\n");
  }
  f->depth--;
  findent(f);
  fp(f, "}");
}

static const char *op_s(int op) { return op_spell(op); }

// compound assignment spelling: += -= *= … (OP_NONE prints plain =)
static const char *assign_s(int op) {
  switch (op) {
  case OP_NONE: return "=";
  case OP_ADD: return "+=";
  case OP_SUB: return "-=";
  case OP_MUL: return "*=";
  case OP_DIV: return "/=";
  case OP_MOD: return "%=";
  case OP_BAND: return "&=";
  case OP_BOR: return "|=";
  case OP_BXOR: return "^=";
  case OP_SHL: return "<<=";
  case OP_SHR: return ">>=";
  default: return "=";
  }
}

static void fmt_args(F *f, RefList *args) {
  fp(f, "(");
  for (size_t i = 0; i < reflist_len(args); i++) {
    Node *a = NG(reflist_at(args, i));
    if (i)
      fp(f, ", ");
    if (a->kind == NT_FIELDINIT) {
      fp(f, "%s: ", a->name);
      fmt_expr(f, NG(a->a));
    } else if (a->kind == NT_POSARG) {
      if (a->op == 1) {
        fp(f, "[]");
        fmt_type(f, NG(NG(a->a)->a));
      } else {
        fmt_expr(f, NG(a->a));
      }
      if (a->bval)
        fp(f, "...");
    }
  }
  fp(f, ")");
}

static void fmt_new(F *f, Node *n) {
  fp(f, "new %s", n->name);
  if (n->a != NO_REF && NG(n->a)->list) {
    fp(f, "[");
    for (size_t i = 0; i < reflist_len(NG(n->a)->list); i++) {
      if (i)
        fp(f, ", ");
      fmt_type(f, NG(reflist_at(NG(n->a)->list, i)));
    }
    fp(f, "]");
  }
  fp(f, " {");
  RefList *fs = n->b != NO_REF ? NG(n->b)->list : NULL;
  for (size_t i = 0; fs && i < reflist_len(fs); i++) {
    Node *fi = NG(reflist_at(fs, i));
    if (i)
      fp(f, ", ");
    fp(f, "%s: ", fi->name);
    fmt_expr(f, NG(fi->a));
  }
  fp(f, "}");
}

static void fmt_expr(F *f, Node *e) {
  if (!e)
    return;
  switch (e->kind) {
  case NT_INT:
    fp(f, "%llu", (unsigned long long)e->ival);
    return;
  case NT_FLOAT: {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", e->fval);
    fp(f, "%s", buf);
    return;
  }
  case NT_BOOL:
    fp(f, "%s", e->bval ? "true" : "false");
    return;
  case NT_STR:
    fmt_strlit(f, e->sval);
    return;
  case NT_PATH:
    fp(f, "%s", e->name);
    return;
  case NT_CALL:
    fmt_expr(f, NG(e->a));
    fmt_args(f, e->list);
    return;
  case NT_METHOD:
    fmt_expr(f, NG(e->a));
    fp(f, ".%s", e->name);
    fmt_args(f, e->list);
    return;
  case NT_FIELD_E:
    fmt_expr(f, NG(e->a));
    fp(f, ".%s", e->name);
    return;
  case NT_INDEX:
    fmt_expr(f, NG(e->a));
    fp(f, "[");
    fmt_expr(f, NG(e->b));
    fp(f, "]");
    return;
  case NT_SLICE_E:
    fmt_expr(f, NG(e->a));
    fp(f, "[");
    if (e->b != NO_REF)
      fmt_expr(f, NG(e->b));
    fp(f, "..");
    if (e->c != NO_REF)
      fmt_expr(f, NG(e->c));
    fp(f, "]");
    return;
  case NT_UNARY:
    if (e->op == OP_DEREF)
      fp(f, "*");
    else
      fp(f, "%s", op_s(e->op));
    fp(f, "(");
    fmt_expr(f, NG(e->a));
    fp(f, ")");
    return;
  case NT_BINARY:
    fp(f, "(");
    fmt_expr(f, NG(e->a));
    fp(f, " %s ", op_s(e->op));
    fmt_expr(f, NG(e->b));
    fp(f, ")");
    return;
  case NT_AS:
    fp(f, "(");
    fmt_expr(f, NG(e->a));
    fp(f, " as ");
    fmt_type(f, NG(e->b));
    fp(f, ")");
    return;
  case NT_NEW:
    fmt_new(f, e);
    return;
  case NT_SLICE_LIT:
    fp(f, "[");
    for (size_t i = 0; i < reflist_len(e->list); i++) {
      Node *el = NG(reflist_at(e->list, i));
      if (i)
        fp(f, ", ");
      fmt_expr(f, el->kind == NT_POSARG ? NG(el->a) : el);
    }
    fp(f, "]");
    return;
  case NT_CLOSURE:
    fp(f, "fn");
    fmt_params(f, e->list, NULL);
    if (e->b != NO_REF) {
      fp(f, " -> ");
      fmt_type(f, NG(e->b));
    }
    fmt_block(f, NG(e->c));
    return;
  case NT_QMARK:
    fmt_expr(f, NG(e->a));
    fp(f, "?");
    return;
  case NT_IF_EXPR:
    fp(f, "if ");
    fmt_expr(f, NG(e->a));
        fmt_block(f, NG(e->b));
    if (e->c != NO_REF) {
      fp(f, " else ");
      Node *els = NG(e->c);
      if (els->kind == NT_IF_EXPR) {
        fmt_expr(f, els);
      } else {
        fmt_block(f, els);
      }
    }
    return;
  case NT_MATCH_EXPR:
    fp(f, "match ");
    fmt_match(f, e);
    return;
  default:
    fp(f, "?expr");
  }
}

static void fmt_pattern(F *f, Node *p) {
  switch (p->kind) {
  case NT_PLIT: {
    char buf[64];
    if (p->op == 0)
      snprintf(buf, sizeof buf, "%llu", (unsigned long long)p->ival);
    else if (p->op == 1)
      snprintf(buf, sizeof buf, "%.17g", p->fval);
    else if (p->op == 2)
      snprintf(buf, sizeof buf, "%s", p->bval ? "true" : "false");
    else
      buf[0] = 0;
    if (p->op == 3)
      fmt_strlit(f, p->sval);
    else
      fp(f, "%s", buf);
    return;
  }
  case NT_PBIND:
    fp(f, "%s", p->name);
    return;
  case NT_PWILD:
    fp(f, "_");
    return;
  case NT_POR:
    for (size_t i = 0; i < reflist_len(p->list); i++) {
      if (i)
        fp(f, " | ");
      fmt_pattern(f, NG(reflist_at(p->list, i)));
    }
    return;
  case NT_PVAR: {
    fp(f, "%s", p->name);
    if (!reflist_len(p->list))
      return;
    if (p->op == VAR_TUPLE) {
      fp(f, "(");
      for (size_t i = 0; i < reflist_len(p->list); i++) {
        if (i)
          fp(f, ", ");
        Node *s = NG(reflist_at(p->list, i));
        if (s->kind == NT_FIELD)
          fmt_pattern(f, NG(s->a));
        else
          fmt_pattern(f, s);
      }
      fp(f, ")");
    } else {
      fp(f, " {");
      for (size_t i = 0; i < reflist_len(p->list); i++) {
        Node *s = NG(reflist_at(p->list, i));
        if (i)
          fp(f, ", ");
        if (p->op == VAR_STRUCT && s->kind == NT_FIELD) {
          if (s->a != NO_REF && NG(s->a)->kind == NT_PBIND &&
              strcmp(NG(s->a)->name, s->name) == 0)
            fp(f, "%s", s->name);
          else {
            fp(f, "%s: ", s->name);
            fmt_pattern(f, NG(s->a));
          }
        } else {
          fmt_pattern(f, s);
        }
      }
      fp(f, "}");
    }
    return;
  }
  default:
    fp(f, "?pat");
  }
}

static void fmt_stmt(F *f, Node *s) {
  switch (s->kind) {
  case NT_LET:
    fp(f, "let %s%s", s->bval ? "mut " : "", s->name);
    if (s->a != NO_REF) {
      fp(f, ": ");
      fmt_type(f, NG(s->a));
    }
    fp(f, " = ");
    fmt_expr(f, NG(s->b));
    fp(f, ";");
    return;
  case NT_ASSIGN:
    fmt_expr(f, NG(s->a));
    fp(f, " %s ", assign_s(s->op));
    fmt_expr(f, NG(s->b));
    fp(f, ";");
    return;
  case NT_IF:
    if (s->name)
      fp(f, "%s: ", s->name);
    fp(f, "if ");
    fmt_expr(f, NG(s->a));
        fmt_block(f, NG(s->b));
    if (s->c != NO_REF) {
      fp(f, " else ");
      Node *els = NG(s->c);
      if (els->kind == NT_IF)
        fmt_stmt(f, els);
      else
        fmt_block(f, els);
    }
    return;
  case NT_WHILE:
    if (s->name)
      fp(f, "%s: ", s->name);
    fp(f, "while ");
    fmt_expr(f, NG(s->a));
    fmt_block(f, NG(s->b));
    return;
  case NT_LOOP:
    if (s->name)
      fp(f, "%s: ", s->name);
    fp(f, "loop");
    fmt_block(f, NG(s->b));
    return;
  case NT_MATCH:
    fp(f, "match ");
    fmt_match(f, s);
    return;
  case NT_RETURN:
    if (s->a == NO_REF) {
      fp(f, "return;");
    } else {
      fp(f, "return ");
      fmt_expr(f, NG(s->a));
      fp(f, ";");
    }
    return;
  case NT_DEFER: {
    Node *act = NG(s->a);
    fp(f, "defer ");
    if (act && act->kind == NT_ASSIGN)
      fmt_stmt(f, act);
    else {
      fmt_expr(f, act);
      fp(f, ";");
    }
    return;
  }
  case NT_BREAK:
    if (s->name)
      fp(f, "break %s;", s->name);
    else
      fp(f, "break;");
    return;
  case NT_CONTINUE:
    if (s->name)
      fp(f, "continue %s;", s->name);
    else
      fp(f, "continue;");
    return;
  case NT_EXPRSTMT:
    if (s->op == 3) {
      fmt_block(f, s);
      return;
    }
    fmt_expr(f, NG(s->a));
    fp(f, ";");
    return;
  default:
    fp(f, "?stmt");
  }
}

void fmt_program(FILE *out, Program *p) {
  F f;
  f.cap = 1 << 14;
  f.n = 0;
  f.depth = 0;
  f.p = malloc(f.cap);
  for (Module *m = p->modules; m; m = m->next) {
    if (m == g_prelude_mod) // the embedded prelude never prints
      continue;
    if (m != p->entry) // one file in, one file out: imports stay put
      continue;
    for (size_t i = 0; i < reflist_len(m->decls); i++) {
      Node *d = NG(reflist_at(m->decls, i));
      switch (d->kind) {
      case NT_FN: {
        if (d->bval)
          fp(&f, "pub ");
        fp(&f, "fn ");
        if (d->name2)
          fp(&f, "%s.%s", d->name2, d->name);
        else
          fp(&f, "%s", d->name);
        if (d->a != NO_REF && NG(d->a)->list &&
            reflist_len(NG(d->a)->list)) {
          fp(&f, "[");
          RefList *gps = NG(d->a)->list;
          for (size_t g = 0; g < reflist_len(gps); g++) {
            if (g)
              fp(&f, ", ");
            Node *gp = NG(reflist_at(gps, g));
            fp(&f, "%s", gp->name);
          }
          fp(&f, "]");
        }
        fmt_params(&f, d->list, d->name2);
        if (d->c != NO_REF) {
          fp(&f, " -> ");
          fmt_type(&f, NG(d->c));
        }
        fmt_block(&f, NG(d->d));
        fp(&f, "\n");
        break;
      }
      case NT_STRUCT: {
        if (d->bval)
          fp(&f, "pub ");
        fp(&f, "struct %s", d->name);
        if (d->a != NO_REF && NG(d->a)->list &&
            reflist_len(NG(d->a)->list)) {
          RefList *gps = NG(d->a)->list;
          fp(&f, "[");
          for (size_t g = 0; g < reflist_len(gps); g++) {
            if (g)
              fp(&f, ", ");
            fp(&f, "%s", NG(reflist_at(gps, g))->name);
          }
          fp(&f, "]");
        }
        fp(&f, " {\n");
        f.depth++;
        for (size_t fi = 0; fi < reflist_len(d->list); fi++) {
          Node *fd = NG(reflist_at(d->list, fi));
          findent(&f);
          fp(&f, "%s: ", fd->name);
          fmt_type(&f, NG(fd->a));
          fp(&f, ",\n");
        }
        f.depth--;
        findent(&f);
        fp(&f, "}\n\n");
        break;
      }
      case NT_ENUM: {
        if (d->bval)
          fp(&f, "pub ");
        fp(&f, "enum %s", d->name);
        if (d->a != NO_REF && NG(d->a)->list &&
            reflist_len(NG(d->a)->list)) {
          RefList *gps = NG(d->a)->list;
          fp(&f, "[");
          for (size_t g = 0; g < reflist_len(gps); g++) {
            if (g)
              fp(&f, ", ");
            fp(&f, "%s", NG(reflist_at(gps, g))->name);
          }
          fp(&f, "]");
        }
        fp(&f, " {\n", d->name);
        f.depth++;
        for (size_t vi = 0; vi < reflist_len(d->list); vi++) {
          Node *v = NG(reflist_at(d->list, vi));
          findent(&f);
          fp(&f, "%s", v->name);
          if (v->op == VAR_TUPLE) {
            fp(&f, "(");
            for (size_t k = 0; k < reflist_len(v->list); k++) {
              if (k)
                fp(&f, ", ");
              fmt_type(&f, NG(reflist_at(v->list, k)));
            }
            fp(&f, "),\n");
          } else if (v->op == VAR_STRUCT) {
            fp(&f, " {\n");
            f.depth++;
            for (size_t k = 0; k < reflist_len(v->list); k++) {
              Node *fd = NG(reflist_at(v->list, k));
              findent(&f);
              fp(&f, "%s: ", fd->name);
              fmt_type(&f, NG(fd->a));
              fp(&f, ",\n");
            }
            f.depth--;
            findent(&f);
            fp(&f, "},\n");
          } else {
            fp(&f, ",\n");
          }
        }
        f.depth--;
        findent(&f);
        fp(&f, "}\n\n");
        break;
      }
      case NT_TRAIT: {
        if (d->bval)
          fp(&f, "pub ");
        fp(&f, "trait %s {\n", d->name);
        f.depth++;
        for (size_t si = 0; si < reflist_len(d->list); si++) {
          Node *sig = NG(reflist_at(d->list, si));
          findent(&f);
          fp(&f, "fn %s", sig->name);
          fmt_params(&f, sig->list, NULL);
          if (sig->c != NO_REF) {
            fp(&f, " -> ");
            fmt_type(&f, NG(sig->c));
          }
          fp(&f, ",\n");
        }
        f.depth--;
        findent(&f);
        fp(&f, "}\n\n");
        break;
      }
      case NT_IMPL: {
        fp(&f, "impl %s for ", d->name);
        fmt_type(&f, NG(d->b));
        fp(&f, " {\n");
        f.depth++;
        // members may write the receiver redundantly; the impl target
        // is the derivation home (§18/T3.10)
        const char *irecv =
            d->b != NO_REF ? NG(d->b)->name : NULL;
        for (size_t fi = 0; fi < reflist_len(d->list); fi++) {
          Node *fn = NG(reflist_at(d->list, fi));
          findent(&f);
          fp(&f, "fn %s", fn->name);
          fmt_params(&f, fn->list, irecv);
          if (fn->c != NO_REF) {
            fp(&f, " -> ");
            fmt_type(&f, NG(fn->c));
          }
          fmt_block(&f, NG(fn->d));
          fp(&f, "\n");
        }
        f.depth--;
        findent(&f);
        fp(&f, "}\n\n");
        break;
      }
      case NT_TEST: {
        fp(&f, "test \"%s\"", d->name);
        fmt_block(&f, NG(d->d));
        fp(&f, "\n");
        break;
      }
      case NT_CONST: {
        if (d->bval)
          fp(&f, "pub ");
        fp(&f, "const %s", d->name);
        if (d->a != NO_REF) {
          fp(&f, ": ");
          fmt_type(&f, NG(d->a));
        }
        fp(&f, " = ");
        fmt_expr(&f, NG(d->b));
        fp(&f, ";\n\n");
        break;
      }
      case NT_STATIC: {
        fp(&f, "static mut %s: ", d->name);
        fmt_type(&f, NG(d->a));
        fp(&f, " = ");
        fmt_expr(&f, NG(d->b));
        fp(&f, ";\n\n");
        break;
      }
      case NT_USE: {
        static const char *forms[] = {"use", "pub use", "pub use",
                                      "pub use", "pub use"};
        fp(&f, "%s ", forms[d->op & 7]);
        for (size_t si = 0; si < reflist_len(d->list); si++) {
          if (si)
            fp(&f, ".");
          fp(&f, "%s", NG(reflist_at(d->list, si))->name);
        }
        if ((d->op & 7) == USE_PUB_AS)
          fp(&f, " as %s", d->name2);
        if ((d->op & 7) == USE_PUB_STAR)
          fp(&f, ".*");
        if ((d->op & 7) == USE_PLAIN && d->name2)
          fp(&f, " as %s", d->name2);
        fp(&f, ";\n\n");
        break;
      }
      default:
        break;
      }
    }
  }
  fwrite(f.p, 1, f.n, out);
  free(f.p);
}
