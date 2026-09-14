#include "rho.h"

typedef struct Parser {
  Token **toks;
  size_t n, i;
} Parser;

static Token *peek(Parser *p) { return p->toks[p->i]; }
static Token *peek2(Parser *p) { return p->toks[p->i + 1 < p->n ? p->i + 1 : p->i]; }
static Token *advance(Parser *p) { return p->toks[p->i < p->n - 1 ? p->i++ : p->i]; }

static bool at(Parser *p, Tok k) { return peek(p)->kind == k; }
static bool at_ident(Parser *p, const char *word) {
  return peek(p)->kind == TK_IDENT && str_eq_c(peek(p)->text, word);
}

static bool accept(Parser *p, Tok k) {
  if (at(p, k)) {
    advance(p);
    return true;
  }
  return false;
}

static Token *expect(Parser *p, Tok k, const char *what) {
  if (at(p, k))
    return advance(p);
  Token *t = peek(p);
  err_at(t->file, t->line, t->col, "expected %s (%s), found %s", what, tok_name(k), tok_name(t->kind));
  return t;
}

static Token *expect_ident(Parser *p, const char *what) {
  if (at(p, TK_IDENT))
    return advance(p);
  Token *t = peek(p);
  err_at(t->file, t->line, t->col, "expected %s, found %s", what, tok_name(t->kind));
  return t;
}

#define NODE(f, k)                                 \
  f = arena_alloc_zeroed(sizeof(*f));              \
  f->kind = (k);                                   \
  f->file = start->file;                           \
  f->line = start->line;                           \
  f->col = start->col;

static TypeAst *parse_type(Parser *p);

static Expr *type_as_expr(Parser *p) {
  Token *start = peek(p);
  Expr *e;
  NODE(e, EX_TYPE);
  e->ty = parse_type(p);
  return e;
}

// ---- primitive type names
static int prim_from_name(Str s) {
  for (int i = 0; i < PRIM_COUNT; i++)
    if (str_eq_c(s, PRIM_NAMES[i]))
      return i;
  return -1;
}

static Expr *parse_expr(Parser *p);

static TypeAst *parse_type(Parser *p) {
  Token *start = peek(p);
  TypeAst *t;
  if (accept(p, P_LBRACKET)) {
    // []T slice or [N]T array
    NODE(t, TA_ARRAY);
    if (!at(p, P_RBRACKET)) {
      t->size = parse_expr(p);
    }
    expect(p, P_RBRACKET, "`]`");
    t->elem = parse_type(p);
    return t;
  }
  if (accept(p, P_STAR)) {
    NODE(t, TA_PTR);
    t->elem = parse_type(p);
    return t;
  }
  if (at(p, KW_WEAK)) {
    advance(p);
    NODE(t, TA_WEAK);
    expect(p, P_LBRACKET, "`[`");
    t->elem = parse_type(p);
    expect(p, P_RBRACKET, "`]`");
    return t;
  }
  if (at(p, KW_FN)) {
    advance(p);
    NODE(t, TA_FN);
    expect(p, P_LPAREN, "`(`");
    if (!at(p, P_RPAREN)) {
      do {
        vec_push(&t->params, parse_type(p));
      } while (accept(p, P_COMMA));
    }
    expect(p, P_RPAREN, "`)`");
    if (accept(p, P_ARROW))
      t->ret = parse_type(p);
    return t;
  }
  if (at(p, TK_IDENT)) {
    Token *id = advance(p);
    int prim = prim_from_name(id->text);
    NODE(t, TA_PRIM);
    if (prim >= 0) {
      t->prim = prim;
      return t;
    }
    t->kind = TA_NAMED;
    vec_push(&t->path, str_to_c(id->text));
    while (accept(p, P_DOT)) {
      Token *seg = expect_ident(p, "type name");
      vec_push(&t->path, str_to_c(seg->text));
    }
    if (accept(p, P_LBRACKET)) {
      if (!at(p, P_RBRACKET)) {
        do {
          vec_push(&t->targs, parse_type(p));
        } while (accept(p, P_COMMA));
      }
      expect(p, P_RBRACKET, "`]`");
    }
    return t;
  }
  err_at(start->file, start->line, start->col, "expected a type, found %s", tok_name(start->kind));
  NODE(t, TA_PRIM);
  t->prim = 0;
  return t;
}

static Param *parse_param(Parser *p) {
  Token *start = peek(p);
  Param *pa = arena_alloc_zeroed(sizeof(Param));
  pa->file = start->file;
  pa->line = start->line;
  pa->col = start->col;
  if (at(p, KW_SELF)) {
    advance(p);
    pa->name = str_from("self");
    pa->is_self = true;
  } else {
    Token *id = expect_ident(p, "parameter name");
    pa->name = id->text;
  }
  expect(p, P_COLON, "`:`");
  pa->ty = parse_type(p);
  return pa;
}

// optional `[T, U]` type-parameter list (fn/struct/enum declarations)
static void parse_tparams(Parser *p, Vec *out) {
  if (!accept(p, P_LBRACKET))
    return;
  if (!at(p, P_RBRACKET)) {
    do {
      Token *id = expect_ident(p, "type parameter name");
      vec_push(out, str_to_c(id->text));
    } while (accept(p, P_COMMA));
  }
  expect(p, P_RBRACKET, "`]`");
}

static Stmt *parse_stmt(Parser *p);

static Stmt *parse_block_stmts(Parser *p, Tok end) {
  Token *start = peek(p);
  Stmt *s;
  NODE(s, ST_BLOCK);
  while (!at(p, end) && !at(p, TK_EOF)) {
    size_t before = p->i;
    Stmt *inner = parse_stmt(p);
    if (inner)
      vec_push(&s->stmts, inner);
    if (p->i == before)
      advance(p); // no progress: skip the offending token
  }
  return s;
}

static Vec parse_block(Parser *p) {
  expect(p, P_LBRACE, "`{`");
  Stmt *s = parse_block_stmts(p, P_RBRACE);
  expect(p, P_RBRACE, "`}`");
  return s->stmts;
}

static Expr *parse_expr_bp(Parser *p, int min_bp);

typedef struct BinLevel {
  Tok ops[4];
  int bp;
} BinLevel;

// loosest to tightest, mirrors spec §4.1
static const BinLevel LEVELS[] = {
    {{P_OROR}, 2},    {{P_ANDAND}, 3},  {{P_EQ, P_NE}, 4}, {{P_LT, P_GT, P_LE, P_GE}, 5},
    {{P_PIPE}, 6},    {{P_CARET}, 7},   {{P_AMP}, 8},      {{P_SHL, P_SHR}, 9},
    {{P_PLUS, P_MINUS}, 10}, {{P_STAR, P_SLASH, P_PERCENT}, 11},
};

static int level_of(Tok k, int *bp) {
  for (size_t i = 0; i < sizeof(LEVELS) / sizeof(LEVELS[0]); i++)
    for (int j = 0; j < 4; j++)
      if (LEVELS[i].ops[j] && LEVELS[i].ops[j] == k) {
        *bp = LEVELS[i].bp;
        return (int)i;
      }
  return -1;
}

// Parses `if cond { } (else (if.. | block))?` — the `if` keyword is already
// consumed. The then-block is items[0]; the else-arm (an EX_IF or a Stmt
// block) is items[1].
static Expr *parse_if_body(Parser *p) {
  Token *start = peek(p);
  Expr *e;
  NODE(e, EX_IF);
  e->a = parse_expr_bp(p, 0); // condition; `{` ends it (no struct literals)
  expect(p, P_LBRACE, "`{`");
  Stmt *then = parse_block_stmts(p, P_RBRACE);
  expect(p, P_RBRACE, "`}`");
  vec_push(&e->items, then);
  if (accept(p, KW_ELSE)) {
    if (at(p, KW_IF)) {
      advance(p);
      vec_push(&e->items, parse_if_body(p));
    } else {
      expect(p, P_LBRACE, "`{`");
      Stmt *els = parse_block_stmts(p, P_RBRACE);
      expect(p, P_RBRACE, "`}`");
      vec_push(&e->items, els);
    }
  }
  return e;
}

static Expr *parse_primary(Parser *p) {
  Token *start = peek(p);
  Expr *e;
  switch (start->kind) {
  case TK_INT: {
    advance(p);
    NODE(e, EX_INT);
    e->iv = start->iv;
    return e;
  }
  case TK_FLOAT: {
    advance(p);
    NODE(e, EX_FLOAT);
    e->fv = start->fv;
    return e;
  }
  case TK_STR: {
    advance(p);
    NODE(e, EX_STR);
    e->sv = start->text;
    return e;
  }
  case KW_TRUE:
  case KW_FALSE: {
    advance(p);
    NODE(e, EX_BOOL);
    e->bv = start->kind == KW_TRUE;
    return e;
  }
  case KW_NULL: {
    advance(p);
    NODE(e, EX_NULL);
    return e;
  }
  case KW_SELF: {
    advance(p);
    NODE(e, EX_NAME);
    e->sv = start->text;
    return e;
  }
  case TK_IDENT: {
    advance(p);
    NODE(e, EX_NAME);
    e->sv = start->text;
    return e;
  }
  case P_LPAREN: {
    advance(p);
    e = parse_expr(p);
    expect(p, P_RPAREN, "`)`");
    return e;
  }
  case KW_IF: {
    advance(p);
    return parse_if_body(p);
  }
  case KW_MATCH: {
    advance(p);
    NODE(e, EX_MATCH);
    e->a = parse_expr_bp(p, 0);
    expect(p, P_LBRACE, "`{`");
    while (!at(p, P_RBRACE) && !at(p, TK_EOF)) {
      MatchArm *arm = arena_alloc_zeroed(sizeof(MatchArm));
      arm->file = peek(p)->file;
      arm->line = peek(p)->line;
      arm->col = peek(p)->col;
      Token *pt = peek(p);
      if (at(p, TK_INT)) {
        advance(p);
        arm->pk = PAT_INT;
        arm->pat_int = pt->iv;
      } else if (at(p, TK_STR)) {
        advance(p);
        arm->pk = PAT_STR;
        arm->pat_str = pt->text;
      } else if (at_ident(p, "_")) {
        advance(p);
        arm->pk = PAT_WILDCARD;
      } else if (at(p, TK_IDENT)) {
        advance(p);
        vec_push(&arm->pat_path, str_to_c(pt->text));
        while (accept(p, P_DOT)) {
          Token *seg = expect_ident(p, "variant name");
          vec_push(&arm->pat_path, str_to_c(seg->text));
        }
        if (accept(p, P_LPAREN)) {
          arm->pk = PAT_TUPLE;
          if (!at(p, P_RPAREN)) {
            do {
              Token *b = peek(p);
              if (at(p, TK_IDENT)) {
                advance(p);
                vec_push(&arm->pat_names, str_to_c(b->text));
              } else {
                err_at(b->file, b->line, b->col, "expected binding name");
                advance(p);
                vec_push(&arm->pat_names, str_to_c(str_from("_")));
              }
            } while (accept(p, P_COMMA));
          }
          expect(p, P_RPAREN, "`)`");
        } else if (accept(p, P_LBRACE)) {
          arm->pk = PAT_STRUCT;
          while (!at(p, P_RBRACE) && !at(p, TK_EOF)) {
            Token *fn = expect_ident(p, "field name");
            FieldAst *fa = arena_alloc_zeroed(sizeof(FieldAst));
            fa->name = fn->text;
            fa->file = fn->file;
            fa->line = fn->line;
            fa->col = fn->col;
            if (accept(p, P_COLON)) {
              Token *b = expect_ident(p, "binding name");
              vec_push(&arm->pat_names, str_to_c(b->text));
            } else {
              vec_push(&arm->pat_names, str_to_c(fn->text));
            }
            vec_push(&arm->pat_fields, fa);
            if (!accept(p, P_COMMA))
              break;
          }
          expect(p, P_RBRACE, "`}`");
        } else {
          arm->pk = PAT_UNIT;
        }
      } else {
        err_at(pt->file, pt->line, pt->col, "expected a pattern, found %s", tok_name(pt->kind));
        advance(p);
        continue;
      }
      expect(p, P_FATARROW, "`=>`");
      if (at(p, P_LBRACE)) {
        // arm block: `{ stmts }` whose value is its trailing expression
        advance(p);
        Expr *blk;
        NODE(blk, EX_BLOCK);
        Stmt *body = parse_block_stmts(p, P_RBRACE);
        expect(p, P_RBRACE, "`}`");
        blk->items = body->stmts;
        arm->body = blk;
      } else {
        arm->body = parse_expr(p);
      }
      vec_push(&e->arms, arm);
      if (!accept(p, P_COMMA))
        break;
    }
    expect(p, P_RBRACE, "`}`");
    return e;
  }
  case KW_NEW: {
    advance(p);
    NODE(e, EX_NEW);
    e->ty = parse_type(p);
    expect(p, P_LBRACE, "`{`");
    while (!at(p, P_RBRACE) && !at(p, TK_EOF)) {
      Token *fn = expect_ident(p, "field name");
      expect(p, P_COLON, "`:`");
      FieldAst *fa = arena_alloc_zeroed(sizeof(FieldAst));
      fa->name = fn->text;
      fa->file = fn->file;
      fa->line = fn->line;
      fa->col = fn->col;
      // Expr stored in an unused slot: we reuse pat_fields trick — no.
      // NEW fields: keep parallel arrays: fa->ty is a TypeAst*, so store the
      // initializer expr in a parallel vec via args.
      vec_push(&e->args, parse_expr(p));
      vec_push(&e->items, fa); // items = field names
      if (!accept(p, P_COMMA))
        break;
    }
    expect(p, P_RBRACE, "`}`");
    return e;
  }
  case KW_FN: // closure literal: fn(a: T) -> R { ... }
  {
    advance(p);
    NODE(e, EX_CLOSURE);
    expect(p, P_LPAREN, "`(`");
    if (!at(p, P_RPAREN)) {
      do {
        vec_push(&e->params, parse_param(p));
      } while (accept(p, P_COMMA));
    }
    expect(p, P_RPAREN, "`)`");
    if (accept(p, P_ARROW))
      e->ret = parse_type(p);
    e->items = parse_block(p); // body stmts
    return e;
  }
  default:
    err_at(start->file, start->line, start->col, "expected an expression, found %s",
           tok_name(start->kind));
    advance(p);
    NODE(e, EX_INT);
    return e;
  }
}

static Expr *parse_postfix(Parser *p) {
  Expr *e = parse_primary(p);
  for (;;) {
    Token *start = peek(p);
    if (accept(p, P_LPAREN)) {
      Expr *call;
      NODE(call, EX_CALL);
      call->a = e;
      if (!at(p, P_RPAREN)) {
        // named argument: `field: expr` (struct-variant construction)
        if (at(p, TK_IDENT) && peek2(p)->kind == P_COLON) {
          Token *nm = advance(p);
          advance(p); // ':'
          vec_push(&call->arg_names, str_to_c(nm->text));
          vec_push(&call->args, parse_expr(p));
        } else if (e->kind == EX_NAME && str_eq_c(e->sv, "make") && at(p, P_LBRACKET)) {
          // make([]T, n): the first argument is a type
          vec_push(&call->arg_names, NULL);
          vec_push(&call->args, type_as_expr(p));
        } else {
          vec_push(&call->arg_names, NULL);
          vec_push(&call->args, parse_expr(p));
        }
        while (accept(p, P_COMMA)) {
          if (at(p, TK_IDENT) && peek2(p)->kind == P_COLON) {
            Token *nm = advance(p);
            advance(p);
            vec_push(&call->arg_names, str_to_c(nm->text));
            vec_push(&call->args, parse_expr(p));
          } else {
            vec_push(&call->arg_names, NULL);
            vec_push(&call->args, parse_expr(p));
          }
        }
      }
      expect(p, P_RPAREN, "`)`");
      e = call;
      continue;
    }
    if (accept(p, P_LBRACKET)) {
      if (at(p, P_ELLIPSIS2)) {
        err_at(start->file, start->line, start->col, "slice needs a start");
        advance(p);
        parse_expr(p);
        expect(p, P_RBRACKET, "`]`");
        continue;
      }
      Expr *idx = parse_expr(p);
      if (accept(p, P_ELLIPSIS2)) {
        Expr *sl;
        NODE(sl, EX_SLICE);
        sl->a = e;
        sl->b = idx;
        sl->c = parse_expr(p);
        expect(p, P_RBRACKET, "`]`");
        e = sl;
        continue;
      }
      expect(p, P_RBRACKET, "`]`");
      Expr *ix;
      NODE(ix, EX_INDEX);
      ix->a = e;
      ix->b = idx;
      e = ix;
      continue;
    }
    if (accept(p, P_DOT)) {
      Token *name = expect_ident(p, "field or method name");
      Expr *f;
      NODE(f, EX_FIELD);
      f->a = e;
      f->sv = name->text;
      e = f;
      continue;
    }
    if (accept(p, P_QMARK)) {
      Expr *q;
      NODE(q, EX_QMARK);
      q->a = e;
      e = q;
      continue;
    }
    if (at(p, KW_AS)) {
      advance(p);
      Expr *c;
      NODE(c, EX_CAST);
      c->a = e;
      c->ty = parse_type(p);
      e = c;
      continue;
    }
    break;
  }
  return e;
}

static Expr *parse_expr_bp(Parser *p, int min_bp) {
  Token *start = peek(p);
  Expr *left;
  int unary_bp = 12;
  if (min_bp <= unary_bp && (at(p, P_BANG) || at(p, P_MINUS) || at(p, P_TILDE) || at(p, P_STAR))) {
    Token *op = advance(p);
    Expr *operand = parse_expr_bp(p, unary_bp);
    Expr *u;
    NODE(u, EX_UN);
    u->unop = op->kind;
    u->a = operand;
    left = u;
  } else {
    left = parse_postfix(p);
  }
  for (;;) {
    int bp;
    if (level_of(peek(p)->kind, &bp) < 0 || bp < min_bp)
      break;
    Token *op = advance(p);
    Expr *right = parse_expr_bp(p, bp + 1);
    Expr *bin;
    NODE(bin, EX_BIN);
    bin->binop = op->kind;
    bin->a = left;
    bin->b = right;
    left = bin;
  }
  return left;
}

static Expr *parse_expr(Parser *p) { return parse_expr_bp(p, 0); }

static bool at_assign_start(Parser *p) {
  switch (peek(p)->kind) {
  case P_ASSIGN: case P_PLUSEQ: case P_MINUSEQ: case P_STAREQ: case P_SLASHEQ:
  case P_PERCENTEQ: case P_AMPEQ: case P_PIPEEQ: case P_CARETEQ: case P_SHLEQ:
  case P_SHREQ:
    return true;
  default:
    return false;
  }
}

static Stmt *parse_stmt(Parser *p) {
  Token *start = peek(p);
  switch (start->kind) {
  case KW_LET: {
    advance(p);
    Stmt *s;
    NODE(s, ST_LET);
    s->mut = accept(p, KW_MUT);
    Token *id = expect_ident(p, "binding name");
    s->name = id->text;
    if (accept(p, P_COLON))
      s->ty = parse_type(p);
    expect(p, P_ASSIGN, "`=` (bindings need an initializer)");
    s->a = parse_expr(p);
    expect(p, P_SEMI, "`;`");
    return s;
  }
  case KW_RETURN: {
    advance(p);
    Stmt *s;
    NODE(s, ST_RETURN);
    if (!at(p, P_SEMI))
      s->a = parse_expr(p);
    expect(p, P_SEMI, "`;`");
    return s;
  }
  case KW_BREAK: {
    advance(p);
    Stmt *s;
    NODE(s, ST_BREAK);
    expect(p, P_SEMI, "`;`");
    return s;
  }
  case KW_CONTINUE: {
    advance(p);
    Stmt *s;
    NODE(s, ST_CONTINUE);
    expect(p, P_SEMI, "`;`");
    return s;
  }
  case KW_DEFER: {
    advance(p);
    Stmt *s;
    NODE(s, ST_DEFER);
    if (at(p, P_LBRACE)) {
      Stmt *blk;
      NODE(blk, ST_BLOCK);
      blk->stmts = parse_block(p);
      vec_push(&s->stmts, blk);
    } else {
      Stmt *one = parse_stmt(p); // e.g. `defer cleanup(x);`
      vec_push(&s->stmts, one);
    }
    return s;
  }
  case KW_WHILE: {
    advance(p);
    Stmt *s;
    NODE(s, ST_WHILE);
    s->cond = parse_expr(p);
    s->body = parse_block(p);
    return s;
  }
  case KW_LOOP: {
    advance(p);
    Stmt *s;
    NODE(s, ST_LOOP);
    s->body = parse_block(p);
    return s;
  }
  case P_LBRACE: {
    advance(p);
    Stmt *s;
    NODE(s, ST_BLOCK);
    s->stmts = parse_block_stmts(p, P_RBRACE)->stmts;
    expect(p, P_RBRACE, "`}`");
    return s;
  }
  default: {
    Expr *e = parse_expr(p);
    if (at_assign_start(p)) {
      Token *op = advance(p);
      Stmt *s;
      NODE(s, ST_ASSIGN);
      s->assign_op = op->kind;
      s->a = e;
      s->b = parse_expr(p);
      expect(p, P_SEMI, "`;`");
      return s;
    }
    Stmt *s;
    NODE(s, ST_EXPR);
    s->a = e;
    bool blocky = e->kind == EX_IF || e->kind == EX_MATCH || e->kind == EX_BLOCK;
    if (accept(p, P_SEMI)) {
      s->tail = false;
    } else if (at(p, P_RBRACE)) {
      s->tail = true; // trailing expression: the enclosing block's value
    } else if (blocky) {
      s->tail = false; // `if`/`match` statements need no semicolon
    } else {
      s->tail = false;
      Token *t = peek(p);
      err_at(t->file, t->line, t->col, "expected `;`, found %s", tok_name(t->kind));
    }
    return s;
  }
  }
}

Decl *parse_file(Str path, Str src) {
  Vec toks = {0};
  lex_file(path, src, &toks);
  Parser p = {NULL, 0, 0};
  p.toks = (Token **)toks.items;
  p.n = toks.n;

  Decl *module = arena_alloc_zeroed(sizeof(Decl));
  module->kind = DK_FN; // module root reuses the node
  module->name = str_from("");
  module->file = path;

  while (!at(&p, TK_EOF)) {
    Token *start = peek(&p);
    bool is_pub = accept(&p, KW_PUB);
    Decl *d = arena_alloc_zeroed(sizeof(Decl));
    d->file = start->file;
    d->line = start->line;
    d->col = start->col;
    d->pub_ = is_pub;
    if (accept(&p, KW_USE)) {
      d->kind = DK_USE;
      do {
        Token *seg = expect_ident(&p, "module path segment");
        vec_push(&d->path, str_to_c(seg->text));
      } while (accept(&p, P_SLASH));
      expect(&p, P_SEMI, "`;`");
    } else if (accept(&p, KW_EXTERN)) {
      d->kind = DK_EXTERN;
      if (at(&p, TK_STR))
        advance(&p); // optional ABI tag, only "c" today
      expect(&p, KW_FN, "`fn`");
      Token *id = expect_ident(&p, "function name");
      d->name = id->text;
      expect(&p, P_LPAREN, "`(`");
      if (!at(&p, P_RPAREN)) {
        do {
          vec_push(&d->params, parse_param(&p));
        } while (accept(&p, P_COMMA));
      }
      expect(&p, P_RPAREN, "`)`");
      if (accept(&p, P_ARROW))
        d->ret = parse_type(&p);
      expect(&p, P_SEMI, "`;`");
    } else if (accept(&p, KW_FN)) {
      d->kind = DK_FN;
      Token *id = expect_ident(&p, "function name");
      d->name = id->text;
      if (accept(&p, P_DOT)) {
        // `fn Type.method` — the `self` parameter's type anchors the method
        // to its struct; the segment is kept for the symbol table key
        d->recv = id->text;
        Token *m = expect_ident(&p, "method name");
        d->name = m->text;
      }
      parse_tparams(&p, &d->tparams);
      expect(&p, P_LPAREN, "`(`");
      if (!at(&p, P_RPAREN)) {
        do {
          vec_push(&d->params, parse_param(&p));
        } while (accept(&p, P_COMMA));
      }
      expect(&p, P_RPAREN, "`)`");
      if (accept(&p, P_ARROW))
        d->ret = parse_type(&p);
      if (d->params.n && ((Param *)d->params.items[0])->is_self)
        d->is_method = true;
      d->body = parse_block(&p);
    } else if (accept(&p, KW_STRUCT)) {
      d->kind = DK_STRUCT;
      Token *id = expect_ident(&p, "struct name");
      d->name = id->text;
      parse_tparams(&p, &d->tparams);
      expect(&p, P_LBRACE, "`{`");
      while (!at(&p, P_RBRACE) && !at(&p, TK_EOF)) {
        Token *fn = expect_ident(&p, "field name");
        expect(&p, P_COLON, "`:`");
        FieldAst *fa = arena_alloc_zeroed(sizeof(FieldAst));
        fa->name = fn->text;
        fa->ty = parse_type(&p);
        fa->file = fn->file;
        fa->line = fn->line;
        fa->col = fn->col;
        vec_push(&d->fields, fa);
        if (!accept(&p, P_COMMA))
          break;
      }
      expect(&p, P_RBRACE, "`}`");
    } else if (accept(&p, KW_ENUM)) {
      d->kind = DK_ENUM;
      Token *id = expect_ident(&p, "enum name");
      d->name = id->text;
      parse_tparams(&p, &d->tparams);
      expect(&p, P_LBRACE, "`{`");
      uint64_t next_disc = 0;
      while (!at(&p, P_RBRACE) && !at(&p, TK_EOF)) {
        Token *vn = expect_ident(&p, "variant name");
        VariantAst *v = arena_alloc_zeroed(sizeof(VariantAst));
        v->name = vn->text;
        v->file = vn->file;
        v->line = vn->line;
        v->col = vn->col;
        if (accept(&p, P_LPAREN)) {
          v->vkind = VAR_TUPLE;
          if (!at(&p, P_RPAREN)) {
            do {
              vec_push(&v->types, parse_type(&p));
            } while (accept(&p, P_COMMA));
          }
          expect(&p, P_RPAREN, "`)`");
        } else if (accept(&p, P_LBRACE)) {
          v->vkind = VAR_STRUCT;
          while (!at(&p, P_RBRACE) && !at(&p, TK_EOF)) {
            Token *fn = expect_ident(&p, "field name");
            expect(&p, P_COLON, "`:`");
            FieldAst *fa = arena_alloc_zeroed(sizeof(FieldAst));
            fa->name = fn->text;
            fa->ty = parse_type(&p);
            fa->file = fn->file;
            fa->line = fn->line;
            fa->col = fn->col;
            vec_push(&v->fields, fa);
            if (!accept(&p, P_COMMA))
              break;
          }
          expect(&p, P_RBRACE, "`}`");
        } else {
          v->vkind = VAR_UNIT;
        }
        if (accept(&p, P_ASSIGN)) {
          if (v->vkind != VAR_UNIT) {
            err_at(vn->file, vn->line, vn->col, "only unit variants take explicit discriminants");
          }
          Token *iv = expect(&p, TK_INT, "discriminant");
          v->has_disc = true;
          v->disc = iv->iv;
          next_disc = iv->iv + 1;
        } else {
          v->disc = next_disc++;
        }
        vec_push(&d->variants, v);
        if (!accept(&p, P_COMMA))
          break;
      }
      expect(&p, P_RBRACE, "`}`");
    } else if (accept(&p, KW_STATIC) || accept(&p, KW_CONST)) {
      d->kind = start->kind == KW_STATIC ? DK_STATIC : DK_CONST;
      d->is_mut = start->kind == KW_STATIC && accept(&p, KW_MUT);
      Token *id = expect_ident(&p, "name");
      d->name = id->text;
      expect(&p, P_COLON, "`:`");
      d->ret = parse_type(&p);
      expect(&p, P_ASSIGN, "`=`");
      d->init = parse_expr(&p);
      expect(&p, P_SEMI, "`;`");
    } else {
      err_at(start->file, start->line, start->col,
             "expected a declaration (fn/struct/enum/static/const/use), found %s",
             tok_name(start->kind));
      advance(&p);
      continue;
    }
    vec_push(&module->decls, d);
  }
  return module;
}
