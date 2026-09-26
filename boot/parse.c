// parse.c — recursive-descent parser for the full declaration surface:
// fn/methods/associated/generic, struct, enum, trait, impl,
// const/static/extern, use/as/pub use, overloads, patterns, labels.
#include "rho.h"

#include <ctype.h>

typedef struct Parser {
  Module *m;
  size_t pos;
} Parser;

static Token *cur(Parser *p) {
  return (Token *)&p->m->toks[p->pos];
}
static Token *at(Parser *p, size_t off) {
  size_t i = p->pos + off;
  if (i >= p->m->ntoks)
    i = p->m->ntoks - 1; // EOF
  return (Token *)&p->m->toks[i];
}
static TokKind kind(Parser *p) { return cur(p)->kind; }

// assignment operators (statement heads; never expressions)
static bool is_assign_tok(TokKind k) {
  return k == T_EQ || k == T_PLUSEQ || k == T_DASHEQ || k == T_STAREQ ||
         k == T_SLASHEQ || k == T_PCTEQ || k == T_AMPEQ || k == T_PIPEEQ ||
         k == T_CARETEQ || k == T_SHLEQ || k == T_SHREQ;
}
static bool is(Parser *p, TokKind k) { return kind(p) == k; }
static bool is2(Parser *p, TokKind k, size_t off) {
  return at(p, off)->kind == k;
}

static Token *eat(Parser *p) {
  Token *t = cur(p);
  if (p->pos + 1 < p->m->ntoks)
    p->pos++;
  return t;
}
static bool accept(Parser *p, TokKind k) {
  if (is(p, k)) {
    eat(p);
    return true;
  }
  return false;
}

static bool peek_is(Parser *p, size_t ahead, TokKind k) {
  size_t i = p->pos + ahead;
  return i < p->m->ntoks && p->m->toks[i].kind == k;
}

static Token *expect(Parser *p, TokKind k, const char *what) {
  if (is(p, k))
    return eat(p);
  diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
          "expected %s %s, found %s", tok_spell(k), what,
          tok_spell(kind(p)));
  return cur(p);
}

static NodeRef nnew(Parser *p, NodeKind k) {
  return node_new(k, p->m->path, cur(p)->line, cur(p)->col);
}

// sync: skip forward to a likely statement/decl boundary (error recovery)
static void sync_stmt(Parser *p) __attribute__((unused));
static void sync_stmt(Parser *p) {
  while (!is(p, T_SEMI) && !is(p, T_RBRACE) && !is(p, T_EOF))
    eat(p);
  accept(p, T_SEMI);
}

// ================================================================ types

static NodeRef parse_type(Parser *p);

static NodeRef parse_builtin(Parser *p) {
  Token *t = eat(p);
  const char *spell;
  tok_is_builtin_type(t->kind, &spell);
  NodeRef r = nnew(p, NT_BUILTIN);
  // note: position is the token's, consumed above; reuse token info
  Node *n = node_get(r);
  n->file = t->file;
  n->line = t->line;
  n->col = t->col;
  n->name = spell;
  return r;
}

static NodeRef parse_fn_type(Parser *p) {
  NodeRef r = nnew(p, NT_FNTYPE);
  Node *n = node_get(r);
  eat(p); // 'fn'
  expect(p, T_LPAREN, "to open the parameter type list");
  n->list = reflist();
  if (!is(p, T_RPAREN)) {
    for (;;) {
      reflist_add(n->list, parse_type(p));
      if (!accept(p, T_COMMA))
        break;
      if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
        break;
    }
  }
  expect(p, T_RPAREN, "to close the parameter type list");
  if (accept(p, T_ARROW))
    n->a = parse_type(p);
  return r;
}

static NodeRef parse_type(Parser *p) {
  switch (kind(p)) {
  case T_STAR: {
    eat(p);
    NodeRef r = nnew(p, NT_PTR);
    node_get(r)->a = parse_type(p);
    return r;
  }
  case T_QUESTION: {
    eat(p);
    NodeRef r = nnew(p, NT_OPT);
    node_get(r)->a = parse_type(p);
    return r;
  }
  case T_LBRACK: {
    // Go-style slice type: '[]' followed by the element type
    eat(p); // '['
    NodeRef r = nnew(p, NT_SLICE);
    expect(p, T_RBRACK, "to close the slice type prefix");
    node_get(r)->a = parse_type(p);
    return r;
  }
  case K_FN:
    return parse_fn_type(p);
  case K_DYN: {
    eat(p);
    Token *t = expect(p, T_IDENT, "as trait name after dyn");
    NodeRef r = nnew(p, NT_DYN);
    node_get(r)->name = intern(t->text.p, t->text.n);
    return r;
  }
  default:
    if (kind(p) >= K_I8 && kind(p) <= K_STRING)
      return parse_builtin(p);
    if (is(p, T_IDENT)) {
      Token *t = eat(p);
      const char *base = intern(t->text.p, t->text.n);
      if (is(p, T_LBRACK)) {
        eat(p);
        NodeRef r = nnew(p, NT_APP);
        Node *n = node_get(r);
        n->file = t->file;
        n->line = t->line;
        n->col = t->col;
        n->name = base;
        n->list = reflist();
        for (;;) {
          reflist_add(n->list, parse_type(p));
          if (!accept(p, T_COMMA))
            break;
        }
        expect(p, T_RBRACK, "to close generic arguments");
        return r;
      }
      // a named type (struct/enum/generic param) without type args
      NodeRef r = nnew(p, NT_APP);
      Node *n = node_get(r);
      n->file = t->file;
      n->line = t->line;
      n->col = t->col;
      n->name = base;
      n->list = NULL; // no type args yet
      return r;
    }
    diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
            "expected a type, found %s", tok_spell(kind(p)));
    eat(p);
    return NO_REF;
  }
}

// ============================================================ patterns

static NodeRef parse_pattern(Parser *p);

// the ( ... ) tuple-payload or { ... } struct-payload part of a
// variant pattern; n is an NT_PVAR with its name already set
static void parse_variant_payload(Parser *p, Node *n) {
  n->list = reflist();
  if (accept(p, T_LPAREN)) {
    n->op = VAR_TUPLE;
    if (!is(p, T_RPAREN)) {
      for (;;) {
        reflist_add(n->list, parse_pattern(p));
        if (!accept(p, T_COMMA))
          break;
      }
    }
    expect(p, T_RPAREN, "to close variant pattern");
  } else if (accept(p, T_LBRACE)) {
    n->op = VAR_STRUCT;
    if (!is(p, T_RBRACE)) {
      for (;;) {
        NodeRef fld = nnew(p, NT_FIELD);
        Node *fn = node_get(fld);
        Token *fnm = expect(p, T_IDENT, "as field binder");
        fn->name = intern(fnm->text.p, fnm->text.n);
        if (accept(p, T_COLON))
          fn->a = parse_pattern(p);
        else {
          NodeRef bind = nnew(p, NT_PBIND);
          node_get(bind)->name = fn->name;
          fn->a = bind;
        }
        reflist_add(n->list, fld);
        if (!accept(p, T_COMMA))
          break;
      }
    }
    expect(p, T_RBRACE, "to close variant pattern");
  } else {
    n->op = VAR_UNIT;
  }
}

static NodeRef parse_pattern(Parser *p) {
  switch (kind(p)) {
  case T_INT: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_PLIT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->op = 0;
    n->ival = t->i;
    return r;
  }
  case T_FLOAT: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_PLIT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->op = 1;
    n->fval = t->f;
    return r;
  }
  case K_TRUE:
  case K_FALSE: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_PLIT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->op = 2;
    n->bval = t->kind == K_TRUE;
    return r;
  }
  case T_STRING: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_PLIT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->op = 3;
    n->sval = t->text;
    return r;
  }
  case T_IDENT: {
    Token *first = cur(p);
    // wildcard binds nothing
    if (first->text.n == 1 && first->text.p[0] == '_') {
      eat(p);
      NodeRef r = nnew(p, NT_PWILD);
      node_get(r)->file = first->file;
      node_get(r)->line = first->line;
      node_get(r)->col = first->col;
      return r;
    }
    eat(p);
    const char *name = intern(first->text.p, first->text.n);
    // path variant? ident '.' ident
    if (is(p, T_DOT) && is2(p, T_IDENT, 1)) {
      eat(p); // .
      Token *second = eat(p);
      const char *var = intern(second->text.p, second->text.n);
      char *joined = aprintf(g_arena, "%s.%s", name, var);
      NodeRef r = nnew(p, NT_PVAR);
      Node *n = node_get(r);
      n->file = second->file;
      n->line = second->line;
      n->col = second->col;
      n->name = joined;
      parse_variant_payload(p, n);
      return r;
    }
    // bare variant candidate: Some(v) / None — resolves against the
    // match's scrutinee enum at check time (§19); a name that is not
    // a variant stays a binder. Parens/brace demand the variant form.
    if (is(p, T_LPAREN) || is(p, T_LBRACE)) {
      NodeRef r = nnew(p, NT_PVAR);
      Node *n = node_get(r);
      n->file = first->file;
      n->line = first->line;
      n->col = first->col;
      n->name = name;
      n->list = reflist();
      parse_variant_payload(p, n);
      return r;
    }
    // plain binder
    NodeRef r = nnew(p, NT_PBIND);
    Node *n = node_get(r);
    n->file = first->file;
    n->line = first->line;
    n->col = first->col;
    n->name = name;
    return r;
  }
  default:
    diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
            "expected a pattern, found %s", tok_spell(kind(p)));
    eat(p);
    return NO_REF;
  }
}

// patterns: parse_pattern handles all forms including '_'
static NodeRef parse_pattern_top(Parser *p);
static NodeRef parse_pattern_top(Parser *p) {
  NodeRef first = parse_pattern(p);
  if (!is(p, T_PIPE))
    return first;
  NodeRef r = nnew(p, NT_POR);
  Node *n = node_get(r);
  n->list = reflist();
  reflist_add(n->list, first);
  while (accept(p, T_PIPE))
    reflist_add(n->list, parse_pattern(p));
  return r;
}

// ========================================================== expressions

static NodeRef parse_expr(Parser *p);
static NodeRef parse_block(Parser *p);

// (forward declarations only; helpers live below)

enum {
  PREC_OR = 1,    // ||
  PREC_AND,       // &&
  PREC_BITOR,     // |
  PREC_BITXOR,    // ^
  PREC_BITAND,    // &
  PREC_EQ,        // == !=
  PREC_REL,       // < <= > >=
  PREC_SHIFT,     // << >>
  PREC_ADD,       // + -
  PREC_MUL,       // * / %
};

static int bin_op_of(TokKind k) {
  switch (k) {
  case T_OROR:
    return (PREC_OR << 8) | OP_OR;
  case T_ANDAND:
    return (PREC_AND << 8) | OP_AND;
  case T_PIPE:
    return (PREC_BITOR << 8) | OP_BOR;
  case T_CARET:
    return (PREC_BITXOR << 8) | OP_BXOR;
  case T_AMP:
    return (PREC_BITAND << 8) | OP_BAND;
  case T_EQEQ:
    return (PREC_EQ << 8) | OP_EQ;
  case T_NE:
    return (PREC_EQ << 8) | OP_NE;
  case T_LT:
    return (PREC_REL << 8) | OP_LT;
  case T_LE:
    return (PREC_REL << 8) | OP_LE;
  case T_GT:
    return (PREC_REL << 8) | OP_GT;
  case T_GE:
    return (PREC_REL << 8) | OP_GE;
  case T_SHL:
    return (PREC_SHIFT << 8) | OP_SHL;
  case T_SHR:
    return (PREC_SHIFT << 8) | OP_SHR;
  case T_PLUS:
    return (PREC_ADD << 8) | OP_ADD;
  case T_DASH:
    return (PREC_ADD << 8) | OP_SUB;
  case T_STAR:
    return (PREC_MUL << 8) | OP_MUL;
  case T_SLASH:
    return (PREC_MUL << 8) | OP_DIV;
  case T_PERCENT:
    return (PREC_MUL << 8) | OP_MOD;
  default:
    return 0;
  }
}

static NodeRef parse_primary(Parser *p) {
  switch (kind(p)) {
  case T_INT: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_INT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->ival = t->i;
    return r;
  }
  case T_FLOAT: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_FLOAT);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->fval = t->f;
    return r;
  }
  case K_TRUE:
  case K_FALSE: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_BOOL);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->bval = t->kind == K_TRUE;
    return r;
  }
  case T_STRING:
  case T_TSTRING: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_STR);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->sval = t->text;
    n->op = t->kind == T_TSTRING ? 1 : 0;
    return r;
  }
  case K_NULL:
    diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
            "'null' does not exist: pointers are non-null, absence is ?T");
    eat(p);
    return NO_REF;
  case T_LPAREN: {
    eat(p);
    NodeRef e = parse_expr(p);
    expect(p, T_RPAREN, "to close parenthesized expression");
    return e;
  }
  case T_LBRACK: {
    Token *open = eat(p);
    NodeRef r = nnew(p, NT_SLICE_LIT);
    Node *n = node_get(r);
    n->file = open->file;
    n->line = open->line;
    n->col = open->col;
    n->list = reflist();
    if (!is(p, T_RBRACK)) {
      for (;;) {
        reflist_add(n->list, parse_expr(p));
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
    }
    expect(p, T_RBRACK, "to close slice literal");
    return r;
  }
  case K_NEW: {
    Token *t = eat(p);
    Token *tn = expect(p, T_IDENT, "as the type to allocate");
    NodeRef r = nnew(p, NT_NEW);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->name = intern(tn->text.p, tn->text.n);
    // a: optional NT_APP wrapper with explicit type args
    if (is(p, T_LBRACK)) {
      NodeRef app = nnew(p, NT_APP);
      Node *an = node_get(app);
      an->name = n->name;
      an->list = reflist();
      eat(p); // '['
      for (;;) {
        reflist_add(an->list, parse_type(p));
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
      expect(p, T_RBRACK, "to close generic arguments");
      n->a = app;
    }
    // b: wrapper whose ->list holds the NT_FIELDINIT list
    NodeRef fw = nnew(p, NT_FIELDINIT);
    node_get(fw)->list = reflist();
    n->b = fw;
    if (accept(p, T_LBRACE)) {
      if (!is(p, T_RBRACE)) {
        for (;;) {
          NodeRef fi = nnew(p, NT_FIELDINIT);
          Token *fnm = expect(p, T_IDENT, "as field name");
          node_get(fi)->name = intern(fnm->text.p, fnm->text.n);
          expect(p, T_COLON, "after field name");
          node_get(fi)->a = parse_expr(p);
          reflist_add(node_get(fw)->list, fi);
          if (!accept(p, T_COMMA))
            break;
          if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
            break;
        }
      }
      expect(p, T_RBRACE, "to close constructor");
    } else {
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "expected '{ field: value, ... }' after new %s", n->name);
    }
    return r;
  }
  case T_IDENT: {
    Token *t = eat(p);
    NodeRef r = nnew(p, NT_PATH);
    Node *n = node_get(r);
    n->file = t->file;
    n->line = t->line;
    n->col = t->col;
    n->name = intern(t->text.p, t->text.n);
    return r;
  }
  case K_IF: {
    eat(p);
    NodeRef r = nnew(p, NT_IF_EXPR);
    node_get(r)->a = parse_expr(p);
    node_get(r)->b = parse_block(p);
    if (accept(p, K_ELSE)) {
      if (is(p, K_IF)) {
        node_get(r)->c = parse_expr(p); // else if chain
      } else {
        node_get(r)->c = parse_block(p);
      }
    }
    return r;
  }
  case K_MATCH: {
    eat(p);
    NodeRef r = nnew(p, NT_MATCH_EXPR);
    node_get(r)->a = parse_expr(p);
    node_get(r)->list = reflist();
    expect(p, T_LBRACE, "to open match arms");
    if (!is(p, T_RBRACE)) {
      for (;;) {
        NodeRef arm = nnew(p, NT_ARM);
        node_get(arm)->a = parse_pattern_top(p);
        if (accept(p, K_IF))
          node_get(arm)->c = parse_expr(p); // §19 guard
        expect(p, T_FATARROW, "in match arm");
        if (is(p, T_LBRACE))
          node_get(arm)->b = parse_block(p);
        else {
          node_get(arm)->b = parse_expr(p);
          // an assignment is a statement: an arm body that is one
          // needs a block — diagnose instead of derailing the parse
          if (is_assign_tok(kind(p))) {
            diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
                    "an assignment arm body needs a block: "
                    "pat => { x = …; }");
            while (!is(p, T_COMMA) && !is(p, T_RBRACE) && !is(p, T_EOF))
              eat(p);
          }
        }
        reflist_add(node_get(r)->list, arm);
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
        if (is(p, T_RBRACE))
          break;
      }
    }
    expect(p, T_RBRACE, "to close match arms");
    return r;
  }
  case K_FN: {
    // closure
    eat(p);
    NodeRef r = nnew(p, NT_CLOSURE);
    Node *n = node_get(r);
    n->list = reflist();
    expect(p, T_LPAREN, "to open closure parameters");
    if (!is(p, T_RPAREN)) {
      for (;;) {
        NodeRef param = nnew(p, NT_PARAM);
        Node *pn = node_get(param);
        Token *nm = expect(p, T_IDENT, "as parameter name");
        pn->name = intern(nm->text.p, nm->text.n);
        expect(p, T_COLON, "after parameter name");
        pn->a = parse_type(p);
        reflist_add(n->list, param);
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
    }
    expect(p, T_RPAREN, "to close closure parameters");
    if (accept(p, T_ARROW))
      n->b = parse_type(p);
    n->c = parse_block(p);
    return r;
  }
  default:
    diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
            "expected an expression, found %s", tok_spell(kind(p)));
    eat(p);
    return NO_REF;
  }
}

static NodeRef parse_postfix(Parser *p) {
  NodeRef e = parse_primary(p);
  for (;;) {
    if (is(p, T_LPAREN)) {
      eat(p);
      NodeRef r = nnew(p, NT_CALL);
      Node *n = node_get(r);
      n->a = e;
      n->list = reflist();
      // builtin make([]T, n): first argument is a type
      if (node_get(e)->kind == NT_PATH &&
          strcmp(node_get(e)->name, "make") == 0 && is(p, T_LBRACK)) {
        NodeRef tw = nnew(p, NT_SLICE);
        eat(p); // '['
        expect(p, T_RBRACK, "to close the make element type prefix");
        node_get(tw)->a = parse_type(p);
        NodeRef aw = nnew(p, NT_POSARG);
        node_get(aw)->op = 1; // type argument marker
        node_get(aw)->a = tw;
        reflist_add(n->list, aw);
        if (accept(p, T_COMMA)) {
          NodeRef cnt = nnew(p, NT_POSARG);
          node_get(cnt)->a = parse_expr(p);
          reflist_add(n->list, cnt);
        }
        expect(p, T_RPAREN, "to close make");
        e = r;
        continue;
      }
      if (!is(p, T_RPAREN)) {
        for (;;) {
          if (is(p, T_IDENT) && is2(p, T_COLON, 1)) {
            // named constructor argument: name: expr
            NodeRef aw = nnew(p, NT_FIELDINIT);
            Token *nm = eat(p);
            eat(p); // ':'
            node_get(aw)->name = intern(nm->text.p, nm->text.n);
            node_get(aw)->a = parse_expr(p);
            reflist_add(n->list, aw);
          } else {
            bool want_mut = accept(p, K_MUT); // §18 argument marker
            NodeRef arg = parse_expr(p);
            bool spread = accept(p, T_ELLIPSIS);
            NodeRef aw = nnew(p, NT_POSARG);
            node_get(aw)->a = arg;
            node_get(aw)->bval = spread;
            node_get(aw)->op = want_mut ? 2 : 0; // §18 marker: op 2 (make type-arg is 1) // marker rides op
            reflist_add(n->list, aw);
          }
          if (!accept(p, T_COMMA))
            break;
          if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
            break;
        }
      }
      expect(p, T_RPAREN, "to close call arguments");
      e = r;
    } else if (is(p, T_DOT)) {
      eat(p);
      Token *t = expect(p, T_IDENT, "as field or method name");
      const char *name = intern(t->text.p, t->text.n);
      if (is(p, T_LPAREN)) {
        eat(p);
        NodeRef r = nnew(p, NT_METHOD);
        Node *n = node_get(r);
        n->a = e;
        n->name = name;
        n->list = reflist();
        if (!is(p, T_RPAREN)) {
          for (;;) {
            if (is(p, T_IDENT) && is2(p, T_COLON, 1)) {
              NodeRef aw = nnew(p, NT_FIELDINIT);
              Token *nm = eat(p);
              eat(p); // ':'
              node_get(aw)->name = intern(nm->text.p, nm->text.n);
              node_get(aw)->a = parse_expr(p);
              reflist_add(n->list, aw);
            } else {
              bool want_mut = accept(p, K_MUT); // §18 argument marker
              NodeRef arg = parse_expr(p);
              bool spread = accept(p, T_ELLIPSIS);
              NodeRef aw = nnew(p, NT_POSARG);
              node_get(aw)->a = arg;
              node_get(aw)->bval = spread;
              node_get(aw)->op = want_mut ? 2 : 0; // §18 marker: op 2 (make type-arg is 1)
              reflist_add(n->list, aw);
            }
            if (!accept(p, T_COMMA))
              break;
            if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
              break;
          }
        }
        expect(p, T_RPAREN, "to close call arguments");
        e = r;
      } else {
        NodeRef r = nnew(p, NT_FIELD_E);
        Node *n = node_get(r);
        n->a = e;
        n->name = name;
        e = r;
      }
    } else if (is(p, T_LBRACK)) {
      eat(p);
      NodeRef r = nnew(p, NT_INDEX);
      Node *n = node_get(r);
      n->a = e;
      if (is(p, T_DOTDOT)) {
        // open low end: s[..hi]
        NodeRef s = nnew(p, NT_SLICE_E);
        Node *sn = node_get(s);
        sn->a = e;
        sn->b = NO_REF;
        eat(p); // ..
        if (!is(p, T_RBRACK))
          sn->c = parse_expr(p);
        expect(p, T_RBRACK, "to close slice");
        e = s;
        continue;
      }
      n->b = parse_expr(p);
      if (is(p, T_DOTDOT)) {
        // slice s[lo..hi], either end open: s[..n], s[n..]
        NodeRef s = nnew(p, NT_SLICE_E);
        Node *sn = node_get(s);
        sn->a = e;
        sn->b = n->b;
        eat(p); // ..
        if (!is(p, T_RBRACK))
          sn->c = parse_expr(p);
        expect(p, T_RBRACK, "to close slice");
        e = s;
      } else {
        expect(p, T_RBRACK, "to close index");
        e = r;
      }
    } else if (is(p, T_QUESTION)) {
      eat(p);
      NodeRef r = nnew(p, NT_QMARK);
      node_get(r)->a = e;
      e = r;
    } else {
      return e;
    }
  }
}

static NodeRef parse_unary(Parser *p) {
  if (is(p, T_DASH)) {
    NodeRef r = nnew(p, NT_UNARY);
    node_get(r)->op = OP_NEG;
    eat(p);
    node_get(r)->a = parse_unary(p);
    return r;
  }
  if (is(p, T_BANG)) {
    NodeRef r = nnew(p, NT_UNARY);
    node_get(r)->op = OP_NOT;
    eat(p);
    node_get(r)->a = parse_unary(p);
    return r;
  }
  if (is(p, T_STAR)) {
    NodeRef r = nnew(p, NT_UNARY);
    node_get(r)->op = OP_DEREF;
    eat(p);
    node_get(r)->a = parse_unary(p);
    return r;
  }
  if (is(p, T_TILDE)) {
    NodeRef r = nnew(p, NT_UNARY);
    node_get(r)->op = OP_BITNOT;
    eat(p);
    node_get(r)->a = parse_unary(p);
    return r;
  }
  return parse_postfix(p);
}

// as-chain sits between unary and the multiplicative level:
// `x as u8 * 2` parses `(x as u8) * 2` (Rust placement, spec §6.1).
static NodeRef parse_as(Parser *p) {
  NodeRef e = parse_unary(p);
  while (is(p, K_AS)) {
    eat(p);
    NodeRef r = nnew(p, NT_AS);
    node_get(r)->a = e;
    node_get(r)->b = parse_type(p);
    e = r;
  }
  return e;
}

static NodeRef parse_binary(Parser *p, int min_prec) {
  NodeRef lhs = min_prec <= PREC_MUL ? parse_as(p) : parse_unary(p);
  for (;;) {
    int bo = bin_op_of(kind(p));
    if (!bo)
      return lhs;
    int prec = bo >> 8;
    if (prec < min_prec)
      return lhs;
    int op = bo & 0xff;
    eat(p);
    NodeRef r = nnew(p, NT_BINARY);
    node_get(r)->op = op;
    node_get(r)->a = lhs;
    node_get(r)->b =
        prec + 1 <= PREC_MUL ? parse_binary(p, prec + 1) : parse_unary(p);
    lhs = r;
  }
}

static NodeRef parse_expr(Parser *p) { return parse_binary(p, PREC_OR); }

// ============================================================ statements

static RefList *parse_params(Parser *p, bool *is_variadic) {
  RefList *params = reflist();
  *is_variadic = false;
  expect(p, T_LPAREN, "to open parameters");
  if (!is(p, T_RPAREN)) {
    for (;;) {
      if (is(p, K_MUT) || is(p, T_IDENT)) {
        NodeRef param = nnew(p, NT_PARAM);
        Node *pn = node_get(param);
        pn->bval = accept(p, K_MUT);
        if (is(p, T_IDENT) && is2(p, T_COLON, 1)) {
          // named parameter
          Token *nm = eat(p);
          eat(p); // ':'
          pn->name = intern(nm->text.p, nm->text.n);
          pn->a = parse_type(p);
        } else if (is(p, T_IDENT) && str_eq_c(cur(p)->text, "self") &&
                   !is2(p, T_COLON, 1) && params->n == 0) {
          // bare 'self' (trait signature form)
          Token *nm = eat(p);
          pn->name = intern(nm->text.p, nm->text.n);
          pn->op = 1; // marks bare self, no type
        } else {
          diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
                  "expected 'name: type' in parameter list");
          eat(p);
          reflist_add(params, param);
          if (!accept(p, T_COMMA))
            break;
          continue;
        }
        if (accept(p, T_ELLIPSIS)) {
          pn->op = 2; // variadic (op: 0 normal, 1 bare self, 2 variadic)
          *is_variadic = true;
        }
        reflist_add(params, param);
      } else if (is(p, K_MUT)) {
        eat(p);
        continue;
      } else {
        diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
                "expected a parameter, found %s", tok_spell(kind(p)));
        eat(p);
      }
      if (!accept(p, T_COMMA))
        break;
      if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
        break;
    }
  }
  expect(p, T_RPAREN, "to close parameters");
  return params;
}

static int assign_op_of(TokKind k) {
  switch (k) {
  case T_EQ:
    return OP_NONE;
  case T_PLUSEQ:
    return OP_ADD;
  case T_DASHEQ:
    return OP_SUB;
  case T_STAREQ:
    return OP_MUL;
  case T_SLASHEQ:
    return OP_DIV;
  case T_PCTEQ:
    return OP_MOD;
  case T_AMPEQ:
    return OP_BAND;
  case T_PIPEEQ:
    return OP_BOR;
  case T_CARETEQ:
    return OP_BXOR;
  case T_SHLEQ:
    return OP_SHL;
  case T_SHREQ:
    return OP_SHR;
  default:
    return -1;
  }
}

static NodeRef parse_stmt(Parser *p);

static NodeRef parse_block(Parser *p) {
  NodeRef r = nnew(p, NT_EXPRSTMT); // block = expr-stmt list wrapper
  Node *n = node_get(r);
  n->list = reflist();
  n->op = 3; // marks this wrapper as a BLOCK
  expect(p, T_LBRACE, "to open a block");
  while (!is(p, T_RBRACE) && !is(p, T_EOF)) {
    NodeRef s = parse_stmt(p);
    reflist_add(n->list, s);
    if (node_get(s)->kind == NT_EXPRSTMT && node_get(s)->bval) {
      // tail expression: the block's value; '}' must follow
      if (!is(p, T_RBRACE))
        diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
                "expected ';' or '}' after the tail expression");
      break;
    }
  }
  expect(p, T_RBRACE, "to close the block");
  return r;
}

static NodeRef parse_stmt(Parser *p) {
  switch (kind(p)) {
  case K_LET: {
    eat(p);
    NodeRef r = nnew(p, NT_LET);
    Node *n = node_get(r);
    n->bval = accept(p, K_MUT);
    Token *nm = expect(p, T_IDENT, "as the let name");
    n->name = intern(nm->text.p, nm->text.n);
    if (accept(p, T_COLON))
      n->a = parse_type(p);
    expect(p, T_EQ, "after the let name (initializer required)");
    n->b = parse_expr(p);
    expect(p, T_SEMI, "after the let initializer");
    return r;
  }
  case K_RETURN: {
    eat(p);
    NodeRef r = nnew(p, NT_RETURN);
    if (!is(p, T_SEMI))
      node_get(r)->a = parse_expr(p);
    expect(p, T_SEMI, "after return");
    return r;
  }
  case K_DEFER: {
    eat(p);
    NodeRef r = nnew(p, NT_DEFER);
    if (is(p, T_IDENT)) {
      int aop = assign_op_of(at(p, 1)->kind);
      if (aop >= 0) {
        NodeRef asg = nnew(p, NT_ASSIGN);
        node_get(asg)->op = aop;
        NodeRef lhs = nnew(p, NT_PATH);
        Token *id = cur(p);
        node_get(lhs)->name = intern(id->text.p, id->text.n);
        node_get(lhs)->file = id->file;
        node_get(lhs)->line = id->line;
        node_get(lhs)->col = id->col;
        node_get(asg)->a = lhs;
        eat(p); // ident
        eat(p); // op
        node_get(asg)->b = parse_expr(p);
        expect(p, T_SEMI, "after defer");
        node_get(r)->a = asg;
        return r;
      }
    }
    node_get(r)->a = parse_expr(p);
    expect(p, T_SEMI, "after defer");
    return r;
  }
  case K_BREAK: {
    eat(p);
    NodeRef r = nnew(p, NT_BREAK);
    if (is(p, T_IDENT)) {
      Token *t = eat(p);
      node_get(r)->name = intern(t->text.p, t->text.n);
    }
    expect(p, T_SEMI, "after break");
    return r;
  }
  case K_CONTINUE: {
    eat(p);
    NodeRef r = nnew(p, NT_CONTINUE);
    if (is(p, T_IDENT)) {
      Token *t = eat(p);
      node_get(r)->name = intern(t->text.p, t->text.n);
    }
    expect(p, T_SEMI, "after continue");
    return r;
  }
  case K_IF: {
    eat(p);
    NodeRef r = nnew(p, NT_IF);
    node_get(r)->a = parse_expr(p);
    node_get(r)->b = parse_block(p);
    if (accept(p, K_ELSE)) {
      if (is(p, K_IF))
        node_get(r)->c = parse_stmt(p); // else-if chain
      else
        node_get(r)->c = parse_block(p);
    }
    return r;
  }
  case K_WHILE:
  case K_LOOP: {
    bool is_while = kind(p) == K_WHILE;
    eat(p);
    NodeRef r = nnew(p, is_while ? NT_WHILE : NT_LOOP);
    if (is_while)
      node_get(r)->a = parse_expr(p);
    node_get(r)->b = parse_block(p);
    return r;
  }
  case K_MATCH: {
    NodeRef r = parse_primary(p); // NT_MATCH_EXPR
    node_get(r)->kind = NT_MATCH;
    return r;
  }
  case T_IDENT: {
    // label? IDENT ':' (while|loop)
    if (is2(p, T_COLON, 1) &&
        (is2(p, K_WHILE, 2) || is2(p, K_LOOP, 2))) {
      Token *lbl = eat(p);
      eat(p); // ':'
      const char *label = intern(lbl->text.p, lbl->text.n);
      bool is_while = kind(p) == K_WHILE;
      eat(p);
      NodeRef r = nnew(p, is_while ? NT_WHILE : NT_LOOP);
      node_get(r)->name = label;
      if (is_while)
        node_get(r)->a = parse_expr(p);
      node_get(r)->b = parse_block(p);
      return r;
    }
    // assignment?
    if (is2(p, T_EQ, 1) || is2(p, T_PLUSEQ, 1) || is2(p, T_DASHEQ, 1) ||
        is2(p, T_STAREQ, 1) || is2(p, T_SLASHEQ, 1) || is2(p, T_PCTEQ, 1) ||
        is2(p, T_AMPEQ, 1) || is2(p, T_PIPEEQ, 1) || is2(p, T_CARETEQ, 1) ||
        is2(p, T_SHLEQ, 1) || is2(p, T_SHREQ, 1)) {
      NodeRef r = nnew(p, NT_ASSIGN);
      node_get(r)->op = assign_op_of(at(p, 1)->kind);
      NodeRef lhs = nnew(p, NT_PATH);
      Token *id = cur(p);
      node_get(lhs)->name = intern(id->text.p, id->text.n);
      node_get(lhs)->file = id->file;
      node_get(lhs)->line = id->line;
      node_get(lhs)->col = id->col;
      node_get(r)->a = lhs;
      eat(p); // ident
      eat(p); // op
      node_get(r)->b = parse_expr(p);
      expect(p, T_SEMI, "after assignment");
      return r;
    }
    // fall through: expression statement
    break;
  }
  case T_LBRACE: {
    // bare block statement (its own scope; defers/releases at its end)
    return parse_block(p);
  }
  case T_EOF:
    return nnew(p, NT_EXPRSTMT);
  default:
    break;
  }

  // compound-target assignment: field/index lvalue then assign op
  NodeRef e = parse_expr(p);
  int aop = assign_op_of(kind(p));
  if (aop >= 0) {
    if (e == NO_REF || (node_get(e)->kind != NT_FIELD_E &&
                        node_get(e)->kind != NT_INDEX)) {
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "invalid assignment target");
    }
    eat(p);
    NodeRef r = nnew(p, NT_ASSIGN);
    node_get(r)->op = aop;
    node_get(r)->a = e;
    node_get(r)->b = parse_expr(p);
    expect(p, T_SEMI, "after assignment");
    return r;
  }
  NodeRef r = nnew(p, NT_EXPRSTMT);
  node_get(r)->a = e;
  if (accept(p, T_SEMI))
    return r;
  // no semicolon: only legal as the block's tail expression (checked
  // by the caller, which requires '}' next)
  node_get(r)->bval = true;
  return r;
}

// ========================================================== declarations

static RefList *parse_generics(Parser *p) {
  // '[' gparam (',' gparam)* ']' — bounds absorb trailing comma-idents
  RefList *out = reflist();
  if (!accept(p, T_LBRACK))
    return out;
  if (!is(p, T_RBRACK)) {
    for (;;) {
      NodeRef g = nnew(p, NT_GPARAM);
      Node *gn = node_get(g);
      Token *nm = expect(p, T_IDENT, "as generic parameter name");
      gn->name = intern(nm->text.p, nm->text.n);
      gn->list = reflist();
      if (accept(p, T_COLON)) {
        for (;;) {
          Token *b = expect(p, T_IDENT, "as trait bound");
          NodeRef seg = nnew(p, NT_SEG);
          node_get(seg)->name = intern(b->text.p, b->text.n);
          reflist_add(gn->list, seg);
          if (!accept(p, T_COMMA))
            break;
          if (is(p, T_RBRACK))
            break;
        }
      }
      reflist_add(out, g);
      if (!accept(p, T_COMMA))
        break;
      if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
        break;
    }
  }
  expect(p, T_RBRACK, "to close generic parameters");
  return out;
}

static RefList *parse_use_segs(Parser *p) {
  RefList *segs = reflist();
  for (;;) {
    if (is(p, T_STAR))
      break; // caller (pub use m.*) handles the star
    Token *t = expect(p, T_IDENT, "as a use path segment");
    NodeRef s = nnew(p, NT_SEG);
    node_get(s)->name = intern(t->text.p, t->text.n);
    reflist_add(segs, s);
    if (!accept(p, T_DOT))
      break;
    if (is(p, T_LBRACE))
      break; // the brace form leaves the items to the caller (§4.4)
  }
  return segs;
}

static NodeRef parse_decl(Parser *p) {
  bool pub = accept(p, K_PUB);
  if (pub && is(p, K_USE)) {
    // pub use forms (plain 'use' handled in the switch below)
    eat(p);
    NodeRef r = nnew(p, NT_USE);
    Node *n = node_get(r);
    n->list = parse_use_segs(p);
    if (accept(p, K_AS)) {
      n->op = USE_PUB_AS;
      Token *alias = expect(p, T_IDENT, "as the alias");
      n->name2 = intern(alias->text.p, alias->text.n);
    } else if (accept(p, T_STAR)) {
      n->op = USE_PUB_STAR;
    } else {
      n->op = reflist_len(n->list) > 1 ? USE_PUB_ITEM : USE_PUB_MOD;
    }
    expect(p, T_SEMI, "after the pub use");
    return r;
  }
  switch (kind(p)) {
  case K_FN: {
    eat(p);
    NodeRef r = nnew(p, NT_FN);
    Node *n = node_get(r);
    n->bval = pub;
    // builtin receiver first: `fn i32.to_str` — the keyword can never
    // be a plain function name, so there is no ambiguity
    if (kind(p) >= K_I8 && kind(p) <= K_STRING && peek_is(p, 1, T_DOT)) {
      const char *spell = NULL;
      tok_is_builtin_type(kind(p), &spell);
      eat(p);
      eat(p); // the dot
      Token *mn = expect(p, T_IDENT, "as the method name");
      n->name2 = intern_c(spell);
      n->name = intern(mn->text.p, mn->text.n);
      n->op = 1; // method marker
      n->a = NO_REF;
      if (is(p, T_LBRACK)) {
        NodeRef gw = nnew(p, NT_APP);
        node_get(gw)->name = "$generics";
        node_get(gw)->list = parse_generics(p);
        n->a = gw;
      }
      bool variadic = false;
      n->list = parse_params(p, &variadic);
      if (accept(p, T_ARROW))
        n->c = parse_type(p);
      n->d = parse_block(p);
      return r;
    }
    Token *nm = expect(p, T_IDENT, "as the function name");
    n->name = intern(nm->text.p, nm->text.n);
    // method or associated function: fn Type.name
    if (accept(p, T_DOT)) {
      Token *mn = expect(p, T_IDENT, "as the method name");
      n->name2 = n->name;
      n->name = intern(mn->text.p, mn->text.n);
      n->op = 1; // method/associated marker
    }
    n->a = NO_REF;
    if (is(p, T_LBRACK)) {
      NodeRef gw = nnew(p, NT_APP);
      node_get(gw)->name = "$generics";
      node_get(gw)->list = parse_generics(p);
      n->a = gw;
    }
    bool variadic = false;
    n->list = parse_params(p, &variadic);
    if (accept(p, T_ARROW))
      n->c = parse_type(p);
    n->d = parse_block(p);
    return r;
  }
  case K_TEST: {
    if (pub) {
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "a test block cannot be 'pub'");
      pub = false;
    }
    eat(p);
    NodeRef r = nnew(p, NT_TEST);
    Node *n = node_get(r);
    if (!is(p, T_STRING)) {
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "expected a quoted test name after 'test', found %s",
              tok_spell(kind(p)));
      n->name = intern_c("?");
    } else {
      n->name = intern(cur(p)->text.p, cur(p)->text.n);
      eat(p);
    }
    n->d = parse_block(p);
    return r;
  }
  case K_STRUCT: {
    eat(p);
    NodeRef r = nnew(p, NT_STRUCT);
    Node *n = node_get(r);
    n->bval = pub;
    Token *nm = expect(p, T_IDENT, "as the struct name");
    n->name = intern(nm->text.p, nm->text.n);
    n->a = NO_REF;
    if (is(p, T_LBRACK)) {
      NodeRef gw = nnew(p, NT_APP);
      node_get(gw)->name = "$generics";
      node_get(gw)->list = parse_generics(p);
      n->a = gw;
    }
    n->list = reflist();
    expect(p, T_LBRACE, "to open the struct body");
    if (!is(p, T_RBRACE)) {
      for (;;) {
        NodeRef f = nnew(p, NT_FIELD);
        Token *fnm = expect(p, T_IDENT, "as a field name");
        node_get(f)->name = intern(fnm->text.p, fnm->text.n);
        expect(p, T_COLON, "after the field name");
        node_get(f)->a = parse_type(p);
        reflist_add(n->list, f);
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
    }
    expect(p, T_RBRACE, "to close the struct body");
    return r;
  }
  case K_ENUM: {
    eat(p);
    NodeRef r = nnew(p, NT_ENUM);
    Node *n = node_get(r);
    n->bval = pub;
    Token *nm = expect(p, T_IDENT, "as the enum name");
    n->name = intern(nm->text.p, nm->text.n);
    n->a = NO_REF;
    if (is(p, T_LBRACK)) {
      NodeRef gw = nnew(p, NT_APP);
      node_get(gw)->name = "$generics";
      node_get(gw)->list = parse_generics(p);
      n->a = gw;
    }
    n->list = reflist();
    expect(p, T_LBRACE, "to open the enum body");
    if (!is(p, T_RBRACE)) {
      for (;;) {
        NodeRef v = nnew(p, NT_ENUMVAR);
        Node *vn = node_get(v);
        Token *vnm = expect(p, T_IDENT, "as a variant name");
        vn->name = intern(vnm->text.p, vnm->text.n);
        vn->list = reflist();
        if (accept(p, T_LPAREN)) {
          vn->op = VAR_TUPLE;
          if (!is(p, T_RPAREN)) {
            for (;;) {
              reflist_add(vn->list, parse_type(p));
              if (!accept(p, T_COMMA))
                break;
            }
          }
          expect(p, T_RPAREN, "to close the variant payload");
        } else if (accept(p, T_LBRACE)) {
          vn->op = VAR_STRUCT;
          if (!is(p, T_RBRACE)) {
            for (;;) {
              NodeRef f = nnew(p, NT_FIELD);
              Token *fnm = expect(p, T_IDENT, "as a payload field name");
              node_get(f)->name = intern(fnm->text.p, fnm->text.n);
              expect(p, T_COLON, "after the payload field name");
              node_get(f)->a = parse_type(p);
              reflist_add(vn->list, f);
              if (!accept(p, T_COMMA))
                break;
            }
          }
          expect(p, T_RBRACE, "to close the variant payload");
        } else {
          vn->op = VAR_UNIT;
        }
        reflist_add(n->list, v);
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
    }
    expect(p, T_RBRACE, "to close the enum body");
    return r;
  }
  case K_TRAIT: {
    eat(p);
    NodeRef r = nnew(p, NT_TRAIT);
    Node *n = node_get(r);
    n->bval = pub;
    Token *nm = expect(p, T_IDENT, "as the trait name");
    n->name = intern(nm->text.p, nm->text.n);
    n->list = reflist();
    expect(p, T_LBRACE, "to open the trait body");
    if (!is(p, T_RBRACE)) {
      for (;;) {
        NodeRef sig = nnew(p, NT_FN);
        Node *sn = node_get(sig);
        expect(p, K_FN, "to open a trait method signature");
        Token *mnm = expect(p, T_IDENT, "as the method name");
        sn->name = intern(mnm->text.p, mnm->text.n);
        bool variadic = false;
        sn->list = parse_params(p, &variadic);
        if (accept(p, T_ARROW))
          sn->c = parse_type(p);
        sn->d = NO_REF; // signature: no body
        reflist_add(n->list, sig);
        if (!accept(p, T_COMMA))
          break;
        if (is(p, T_RBRACE) || is(p, T_RBRACK) || is(p, T_RPAREN))
          break;
      }
    }
    expect(p, T_RBRACE, "to close the trait body");
    return r;
  }
  case K_IMPL: {
    eat(p);
    NodeRef r = nnew(p, NT_IMPL);
    Node *n = node_get(r);
    Token *tnm = expect(p, T_IDENT, "as the trait name");
    n->name = intern(tnm->text.p, tnm->text.n);
    expect(p, K_FOR, "in an impl block");
    n->b = parse_type(p);
    n->list = reflist();
    expect(p, T_LBRACE, "to open the impl body");
    while (!is(p, T_RBRACE) && !is(p, T_EOF)) {
      expect(p, K_FN, "to open an impl method");
      NodeRef f = nnew(p, NT_FN);
      Node *fn = node_get(f);
      Token *mnm = expect(p, T_IDENT, "as the method name");
      fn->name = intern(mnm->text.p, mnm->text.n);
      fn->op = 2; // impl member (method of the impl's target type)
      bool variadic = false;
      fn->list = parse_params(p, &variadic);
      if (accept(p, T_ARROW))
        fn->c = parse_type(p);
      fn->d = parse_block(p);
      reflist_add(n->list, f);
    }
    expect(p, T_RBRACE, "to close the impl body");
    return r;
  }
  case K_CONST: {
    eat(p);
    NodeRef r = nnew(p, NT_CONST);
    Node *n = node_get(r);
    n->bval = pub;
    Token *nm = expect(p, T_IDENT, "as the const name");
    n->name = intern(nm->text.p, nm->text.n);
    if (accept(p, T_COLON))
      n->a = parse_type(p);
    expect(p, T_EQ, "to initialize the const");
    n->b = parse_expr(p);
    expect(p, T_SEMI, "after the const");
    return r;
  }
  case K_STATIC: {
    eat(p);
    NodeRef r = nnew(p, NT_STATIC);
    Node *n = node_get(r);
    if (pub)
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "statics are module-private; 'pub static' does not exist");
    expect(p, K_MUT, "after 'static' (all statics are mut)");
    Token *nm = expect(p, T_IDENT, "as the static name");
    n->name = intern(nm->text.p, nm->text.n);
    expect(p, T_COLON, "after the static name");
    n->a = parse_type(p);
    expect(p, T_EQ, "to initialize the static");
    n->b = parse_expr(p);
    expect(p, T_SEMI, "after the static");
    return r;
  }
  case K_EXTERN: {
    eat(p);
    NodeRef r = nnew(p, NT_EXTERN);
    Node *n = node_get(r);
    n->bval = pub;
    Token *nm = expect(p, T_IDENT, "as the extern name");
    n->name = intern(nm->text.p, nm->text.n);
    expect(p, T_COLON, "after the extern name");
    n->a = parse_type(p); // fn type
    expect(p, T_SEMI, "after the extern");
    return r;
  }
  case K_USE: {
    eat(p);
    NodeRef r = nnew(p, NT_USE);
    Node *n = node_get(r);
    n->op = USE_PLAIN;
    n->list = parse_use_segs(p);
    // the brace form: use segs.{a, b as c,} — pure sugar for one
    // plain use per item (§4.4); expansion happens at load time.
    // parse_use_segs left the cursor on '{' (the dot is consumed)
    if (accept(p, T_LBRACE)) {
      NodeRef items_ref = nnew(p, NT_SEG);
      Node *items = node_get(items_ref);
      items->list = reflist();
      if (is(p, T_RBRACE)) {
        diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
                "the brace form needs at least one item");
      } else {
        for (;;) {
          NodeRef it = nnew(p, NT_SEG);
          Token *inm = expect(p, T_IDENT, "as an item name");
          node_get(it)->name = intern(inm->text.p, inm->text.n);
          if (accept(p, K_AS)) {
            Token *al = expect(p, T_IDENT, "as the item alias");
            node_get(it)->name2 = intern(al->text.p, al->text.n);
          }
          reflist_add(items->list, it);
          if (!accept(p, T_COMMA))
            break;
          if (is(p, T_RBRACE))
            break; // the grammar's trailing comma (§4.4)
        }
      }
      expect(p, T_RBRACE, "to close the item list");
      n->op = USE_BRACE;
      // the items ride a synthetic NT_SEG whose list carries them
      n->d = items_ref;
      expect(p, T_SEMI, "after the use items");
      return r;
    }
    if (accept(p, K_AS)) {
      Token *alias = expect(p, T_IDENT, "as the use alias");
      n->name2 = intern(alias->text.p, alias->text.n);
    }
    expect(p, T_SEMI, "after the use path");
    return r;
  }
  default:
    if (pub)
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "expected a declaration after 'pub', found %s",
              tok_spell(kind(p)));
    else
      diag_at(DIAG_ERROR, p->m->path, cur(p)->line, cur(p)->col,
              "expected a declaration, found %s", tok_spell(kind(p)));
    eat(p);
    return nnew(p, NT_EXPRSTMT);
  }
}

// ================================================================ module

static char *read_whole_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = arena_alloc(g_arena, (size_t)sz + 1, 1);
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = 0;
  if (n)
    *n = rd;
  return buf;
}

Module *module_parse_src(const char *path, const char *src) {
  Module *m = arena_alloc(g_arena, sizeof(Module), 8);
  memset(m, 0, sizeof(Module));
  m->path = intern_c(path);
  // name = file stem
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t stem = dot ? (size_t)(dot - base) : strlen(base);
  m->name = intern(base, stem);
  m->src = (char *)src;
  Lexer *lx = lex_file(g_arena, path, src);
  m->toks = lex_tokens(lx, &m->ntoks);
  m->decls = reflist();
  vec_init(&m->uses, sizeof(UseBind));

  Parser p = {m, 0};
  while (!is(&p, T_EOF)) {
    if (accept(&p, T_SEMI))
      continue; // stray semicolons between decls
    NodeRef d = parse_decl(&p);
    if (node_get(d)->kind != NT_EXPRSTMT || node_get(d)->list)
      reflist_add(m->decls, d);
  }
  return m;
}

Module *module_load(Arena *a, const char *path) {
  (void)a;
  size_t nsrc;
  char *src = read_whole_file(path, &nsrc);
  if (!src) {
    diag_at(DIAG_ERROR, intern_c(path), 0, 0, "cannot open file");
    diags_print(stderr);
    exit(EXIT_COMPILE);
  }
  return module_parse_src(path, src);
}
