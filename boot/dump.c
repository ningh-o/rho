// dump.c — canonical S-expression AST dump (`rho dump-ast`).
// Deterministic: output order is node order, payloads are exact.
#include "rho.h"

void dump_node(FILE *out, Node *n); // public, also used by selftest

static void dump_str(FILE *out, Str s) {
  fputc('"', out);
  for (size_t i = 0; i < s.n; i++) {
    unsigned char c = (unsigned char)s.p[i];
    switch (c) {
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    case 0:
      fputs("\\0", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '"':
      fputs("\\\"", out);
      break;
    default:
      if (c < 0x20 || c == 0x7f)
        fprintf(out, "\\x%02x", c);
      else
        fputc(c, out);
    }
  }
  fputc('"', out);
}

static void dump_fval(FILE *out, double f) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.17g", f);
  fputs(buf, out);
}

static void dump_opt_node(FILE *out, NodeRef r) {
  if (r == NO_REF) {
    fputs("-", out);
    return;
  }
  dump_node(out, node_get(r));
}

static void dump_list(FILE *out, RefList *l) {
  if (!l)
    return;
  for (size_t i = 0; i < reflist_len(l); i++) {
    fputc(' ', out);
    dump_node(out, node_get(reflist_at(l, i)));
  }
}

void dump_pattern(FILE *out, Node *p) {
  switch (p->kind) {
  case NT_PLIT:
    switch (p->op) {
    case 0:
      fprintf(out, "%llu", (unsigned long long)p->ival);
      break;
    case 1:
      dump_fval(out, p->fval);
      break;
    case 2:
      fputs(p->bval ? "true" : "false", out);
      break;
    default:
      dump_str(out, p->sval);
      break;
    }
    break;
  case NT_PBIND:
    fprintf(out, "%s", p->name);
    break;
  case NT_PWILD:
    fputs("_", out);
    break;
  case NT_PVAR: {
    static const char *forms[] = {"unit", "tuple", "struct"};
    fprintf(out, "(pvar %s %s", p->name, forms[p->op]);
    dump_list(out, p->list);
    fputc(')', out);
    break;
  }
  default:
  case NT_POR: {
    fprintf(out, "(or");
    dump_list(out, p->list);
    fputc(')', out);
    return;
  }
    fprintf(out, "(?pattern %s)", node_kind_name(p->kind));
  }
}

void dump_node(FILE *out, Node *n) {
  switch (n->kind) {
  // ---- types
  case NT_BUILTIN:
    fprintf(out, "%s", n->name);
    return;
  case NT_PTR:
    fputs("(* ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_OPT:
    fputs("(? ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_SLICE:
    fputs("([] ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_FNTYPE:
    fputs("(fn-type (", out);
    dump_list(out, n->list);
    fputs(") ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_APP:
    fprintf(out, "(%s [", n->name);
    dump_list(out, n->list);
    fputs("])", out);
    return;
  case NT_DYN:
    fprintf(out, "(dyn %s)", n->name);
    return;

  // ---- declarations
  case NT_FN:
    if (n->op == 2)
      fprintf(out, "(impl-fn %s (", n->name);
    else if (n->name2)
      fprintf(out, "(fn %s.%s (", n->name2, n->name);
    else
      fprintf(out, "(fn %s (", n->name);
    if (n->a != NO_REF && node_get(n->a)->list) {
      fputs("[", out);
      dump_list(out, node_get(n->a)->list);
      fputs("] ", out);
    }
    dump_list(out, n->list); // params
    fputs(") ", out);
    dump_opt_node(out, n->c); // ret
    fputc(' ', out);
    dump_opt_node(out, n->d); // body
    fputc(')', out);
    return;
  case NT_STRUCT:
    fprintf(out, "(struct %s", n->name);
    if (n->list && reflist_len(n->list)) {
      fputs(" (", out);
      dump_list(out, n->list);
      fputc(')', out);
    }
    fputc(')', out);
    return;
  case NT_ENUM:
    fprintf(out, "(enum %s", n->name);
    if (n->list && reflist_len(n->list)) {
      fputs(" (", out);
      dump_list(out, n->list);
      fputc(')', out);
    }
    fputc(')', out);
    return;
  case NT_TRAIT:
    fprintf(out, "(trait %s (", n->name);
    dump_list(out, n->list);
    fputs("))", out);
    return;
  case NT_IMPL:
    fputs("(impl ", out);
    fprintf(out, "%s for ", n->name);
    dump_opt_node(out, n->b);
    dump_list(out, n->list);
    fputc(')', out);
    return;
  case NT_CONST:
    fprintf(out, "(const %s ", n->name);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_STATIC:
    fprintf(out, "(static mut %s ", n->name);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_EXTERN:
    fprintf(out, "(extern %s ", n->name);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_TEST:
    fprintf(out, "(test \"%s\" ", n->name);
    dump_opt_node(out, n->d);
    fputc(')', out);
    return;
  case NT_USE: {
    static const char *forms[] = {"use", "pub-use-mod", "pub-use-item",
                                  "pub-use-as", "pub-use-star",
                                  "use-brace"};
    fprintf(out, "(%s", forms[n->op & 7]);
    dump_list(out, n->list);
    if (n->name2)
      fprintf(out, " as %s", n->name2);
    if ((n->op & 7) == USE_BRACE && n->d)
      dump_list(out, node_get(n->d)->list);
    fputc(')', out);
    return;
  }

  // ---- helper nodes
  case NT_GPARAM:
    fprintf(out, "(gparam %s", n->name);
    dump_list(out, n->list);
    fputc(')', out);
    return;
  case NT_FIELD:
    fprintf(out, "(%s: ", n->name);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_ENUMVAR: {
    static const char *forms[] = {"unit", "tuple", "struct"};
    fprintf(out, "(variant %s %s", n->name, forms[n->op]);
    dump_list(out, n->list);
    fputc(')', out);
    return;
  }
  case NT_ARM:
    fputs("(arm ", out);
    if (n->a != NO_REF)
      dump_pattern(out, node_get(n->a));
    fputs(" => ", out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_FIELDINIT:
    fprintf(out, "(%s: ", n->name);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_POSARG:
    dump_opt_node(out, n->a);
    return;
  case NT_PARAM:
    if (n->op == 1) { // bare self
      fputs("(param self)", out);
      return;
    }
    fprintf(out, "(param %s%s ", n->name, n->bval ? " mut" : "");
    dump_opt_node(out, n->a);
    if (n->op == 2)
      fputs(" ...", out);
    fputc(')', out);
    return;
  case NT_SEG:
    fputs(n->name, out);
    return;

  // ---- statements
  case NT_LET:
    fprintf(out, "(let%s %s ", n->bval ? " mut" : "", n->name);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_ASSIGN:
    fprintf(out, "(assign %s ", op_spell(n->op));
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_IF:
    fprintf(out, "(if%s ", n->name ? n->name : "");
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(' ', out);
    dump_opt_node(out, n->c);
    fputc(')', out);
    return;
  case NT_WHILE:
    fprintf(out, "(while%s ", n->name ? n->name : "");
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_LOOP:
    fprintf(out, "(loop%s ", n->name ? n->name : "");
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_MATCH:
    fputs("(match ", out);
    dump_opt_node(out, n->a);
    dump_list(out, n->list);
    fputc(')', out);
    return;
  case NT_RETURN:
    fputs("(return ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_DEFER:
    fputs("(defer ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_BREAK:
    fprintf(out, "(break %s)", n->name ? n->name : "-");
    return;
  case NT_CONTINUE:
    fprintf(out, "(continue %s)", n->name ? n->name : "-");
    return;
  case NT_EXPRSTMT:
    if (n->op == 3) { // block wrapper
      fputs("(block", out);
      dump_list(out, n->list);
      fputc(')', out);
      return;
    }
    dump_opt_node(out, n->a);
    return;

  // ---- expressions
  case NT_INT:
    fprintf(out, "%llu", (unsigned long long)n->ival);
    return;
  case NT_FLOAT:
    dump_fval(out, n->fval);
    return;
  case NT_BOOL:
    fputs(n->bval ? "true" : "false", out);
    return;
  case NT_STR:
    dump_str(out, n->sval);
    return;
  case NT_PATH:
    fprintf(out, "%s", n->name);
    return;
  case NT_CALL: {
    fputs("(call ", out);
    dump_opt_node(out, n->a);
    dump_list(out, n->list);
    if (n->bval)
      fputs(" ...", out);
    fputc(')', out);
    return;
  }
  case NT_METHOD:
    fprintf(out, "(.%s ", n->name);
    dump_opt_node(out, n->a);
    dump_list(out, n->list);
    fputc(')', out);
    return;
  case NT_FIELD_E:
    fprintf(out, "(.%s ", n->name);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_INDEX:
    fputs("(index ", out);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_UNARY:
    fprintf(out, "(%s ", op_spell(n->op));
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_BINARY:
    fprintf(out, "(%s ", op_spell(n->op));
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_AS:
    fputs("(as ", out);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(')', out);
    return;
  case NT_NEW: {
    fprintf(out, "(new %s", n->name);
    if (n->a != NO_REF && node_get(n->a)->list) {
      fputs(" [", out);
      dump_list(out, node_get(n->a)->list);
      fputc(']', out);
    }
    if (n->b != NO_REF && node_get(n->b)->list &&
        reflist_len(node_get(n->b)->list)) {
      fputs(" {", out);
      dump_list(out, node_get(n->b)->list);
      fputc('}', out);
    }
    fputc(')', out);
    return;
  }
  case NT_SLICE_LIT:
    fputs("[", out);
    dump_list(out, n->list);
    fputc(']', out);
    return;
  case NT_SLICE_E:
    fputs("(slice ", out);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(' ', out);
    dump_opt_node(out, n->c);
    fputc(')', out);
    return;
  case NT_CLOSURE:
    fputs("(closure (", out);
    dump_list(out, n->list);
    fputs(") ", out);
    dump_opt_node(out, n->b);
    fputc(' ', out);
    dump_opt_node(out, n->c);
    fputc(')', out);
    return;
  case NT_QMARK:
    fputs("(? ", out);
    dump_opt_node(out, n->a);
    fputc(')', out);
    return;
  case NT_IF_EXPR:
    fputs("(if-expr ", out);
    dump_opt_node(out, n->a);
    fputc(' ', out);
    dump_opt_node(out, n->b);
    fputc(' ', out);
    dump_opt_node(out, n->c);
    fputc(')', out);
    return;
  case NT_MATCH_EXPR:
    fputs("(match-expr ", out);
    dump_opt_node(out, n->a);
    dump_list(out, n->list);
    fputc(')', out);
    return;

  case NT_PLIT:
  case NT_PBIND:
  case NT_PWILD:
  case NT_PVAR:
    dump_pattern(out, n);
    return;

  default:
    fprintf(out, "(?%s)", node_kind_name(n->kind));
  }
}

void dump_module(FILE *out, Module *m) {
  for (size_t i = 0; i < reflist_len(m->decls); i++) {
    dump_node(out, node_get(reflist_at(m->decls, i)));
    fputc('\n', out);
  }
}
