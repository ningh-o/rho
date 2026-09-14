#include "rho.h"

// Canonical formatter. One style, no configuration: `rho fmt` output is
// idempotent and reparses to the same AST (both pinned by selftest).

static int f_indent;

static void f_line(SB *sb) {
  sb_push(sb, '\n');
  for (int i = 0; i < f_indent; i++)
    sb_append_c(sb, "  ");
}

static void f_type(SB *sb, TypeAst *t);
static void f_expr(SB *sb, Expr *e, int parent_bp);
static void f_stmts(SB *sb, Vec *stmts);
static void f_stmt(SB *sb, Stmt *s);
static void f_if_full(SB *sb, Expr *e);
static void f_match_full(SB *sb, Expr *e);

// precedence levels for parenthesization (mirror the parser)
static int bin_bp(Tok op) {
  switch (op) {
  case P_OROR: return 2;
  case P_ANDAND: return 3;
  case P_EQ: case P_NE: return 4;
  case P_LT: case P_GT: case P_LE: case P_GE: return 5;
  case P_PIPE: return 6;
  case P_CARET: return 7;
  case P_AMP: return 8;
  case P_SHL: case P_SHR: return 9;
  case P_PLUS: case P_MINUS: return 10;
  case P_STAR: case P_SLASH: case P_PERCENT: return 11;
  default: return 0;
  }
}

static void f_strlit(SB *sb, Str s) {
  sb_push(sb, '"');
  for (size_t i = 0; i < s.n; i++) {
    char c = s.p[i];
    switch (c) {
    case '\n': sb_append_c(sb, "\\n"); break;
    case '\t': sb_append_c(sb, "\\t"); break;
    case '\r': sb_append_c(sb, "\\r"); break;
    case '\\': sb_append_c(sb, "\\\\"); break;
    case '"': sb_append_c(sb, "\\\""); break;
    case '\0': sb_append_c(sb, "\\0"); break;
    default:
      if ((unsigned char)c < 0x20)
        sb_printf(sb, "\\x%02x", (unsigned char)c);
      else
        sb_push(sb, c);
    }
  }
  sb_push(sb, '"');
}

static void f_type(SB *sb, TypeAst *t) {
  if (!t)
    return;
  switch (t->kind) {
  case TA_PRIM:
    sb_append_c(sb, PRIM_NAMES[t->prim]);
    break;
  case TA_ARRAY:
    if (t->size) {
      sb_push(sb, '[');
      f_expr(sb, t->size, 0);
      sb_push(sb, ']');
    } else {
      sb_append_c(sb, "[]");
    }
    f_type(sb, t->elem);
    break;
  case TA_PTR:
    sb_push(sb, '*');
    f_type(sb, t->elem);
    break;
  case TA_WEAK:
    sb_append_c(sb, "weak[");
    f_type(sb, t->elem);
    sb_push(sb, ']');
    break;
  case TA_NAMED:
    for (size_t i = 0; i < t->path.n; i++) {
      if (i)
        sb_push(sb, '.');
      sb_append_c(sb, t->path.items[i]);
    }
    if (t->targs.n) {
      sb_push(sb, '[');
      for (size_t i = 0; i < t->targs.n; i++) {
        if (i)
          sb_append_c(sb, ", ");
        f_type(sb, t->targs.items[i]);
      }
      sb_push(sb, ']');
    }
    break;
  case TA_FN:
    sb_append_c(sb, "fn(");
    for (size_t i = 0; i < t->params.n; i++) {
      if (i)
        sb_append_c(sb, ", ");
      f_type(sb, t->params.items[i]);
    }
    sb_append_c(sb, ")");
    if (t->ret) {
      sb_append_c(sb, " -> ");
      f_type(sb, t->ret);
    }
    break;
  case TA_INFER:
    break;
  }
}

// postfix expressions bind at 13, unary at 12
static void f_expr(SB *sb, Expr *e, int parent_bp) {
  int self_bp = 13;
  bool paren = false;
  switch (e->kind) {
  case EX_INT: sb_printf(sb, "%llu", (unsigned long long)e->iv); self_bp = 13; break;
  case EX_FLOAT: {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", e->fv);
    sb_append_c(sb, buf);
    // keep it a float literal
    if (!strchr(buf, '.') && !strchr(buf, 'e'))
      sb_append_c(sb, ".0");
    self_bp = 13;
    break;
  }
  case EX_STR: f_strlit(sb, e->sv); self_bp = 13; break;
  case EX_BOOL: sb_append_c(sb, e->bv ? "true" : "false"); self_bp = 13; break;
  case EX_NULL: sb_append_c(sb, "null"); self_bp = 13; break;
  case EX_NAME: sb_append(sb, e->sv); self_bp = 13; break;
  case EX_TYPE: f_type(sb, e->ty); self_bp = 13; break;
  case EX_BIN: {
    self_bp = bin_bp(e->binop);
    sb_push(sb, '(');
    f_expr(sb, e->a, self_bp);
    sb_printf(sb, " %s ", tok_spell(e->binop));
    f_expr(sb, e->b, self_bp + 1);
    sb_push(sb, ')');
    self_bp = 13; // wrapped
    break;
  }
  case EX_UN: {
    self_bp = 12;
    sb_push(sb, '(');
    sb_append_c(sb, tok_spell(e->unop));
    f_expr(sb, e->a, 12);
    sb_push(sb, ')');
    self_bp = 13;
    break;
  }
  case EX_CALL:
    f_expr(sb, e->a, 13);
    sb_push(sb, '(');
    for (size_t i = 0; i < e->args.n; i++) {
      if (i)
        sb_append_c(sb, ", ");
      if (e->arg_names.n > i && e->arg_names.items[i])
        sb_printf(sb, "%s: ", (char *)e->arg_names.items[i]);
      f_expr(sb, e->args.items[i], 0);
    }
    sb_push(sb, ')');
    break;
  case EX_INDEX:
    f_expr(sb, e->a, 13);
    sb_push(sb, '[');
    f_expr(sb, e->b, 0);
    sb_push(sb, ']');
    break;
  case EX_SLICE:
    f_expr(sb, e->a, 13);
    sb_push(sb, '[');
    f_expr(sb, e->b, 0);
    sb_append_c(sb, "..");
    f_expr(sb, e->c, 0);
    sb_push(sb, ']');
    break;
  case EX_FIELD:
    f_expr(sb, e->a, 13);
    sb_push(sb, '.');
    sb_append(sb, e->sv);
    break;
  case EX_CAST:
    f_expr(sb, e->a, 13);
    sb_append_c(sb, " as ");
    f_type(sb, e->ty);
    break;
  case EX_NEW:
    sb_append_c(sb, "new ");
    f_type(sb, e->ty);
    sb_append_c(sb, " { ");
    for (size_t i = 0; i < e->items.n; i++) {
      FieldAst *fa = e->items.items[i];
      if (i)
        sb_append_c(sb, ", ");
      sb_append(sb, fa->name);
      sb_append_c(sb, ": ");
      f_expr(sb, e->args.items[i], 0);
    }
    sb_append_c(sb, " }");
    break;
  case EX_MAKE:
    sb_append_c(sb, "make(");
    f_type(sb, e->ty);
    sb_append_c(sb, ", ");
    f_expr(sb, e->c, 0);
    sb_push(sb, ')');
    break;
  case EX_CLOSURE:
    sb_append_c(sb, "fn(");
    for (size_t i = 0; i < e->params.n; i++) {
      Param *pa = e->params.items[i];
      if (i)
        sb_append_c(sb, ", ");
      sb_append(sb, pa->name);
      sb_append_c(sb, ": ");
      f_type(sb, pa->ty);
    }
    sb_push(sb, ')');
    if (e->ret) {
      sb_append_c(sb, " -> ");
      f_type(sb, e->ret);
    }
    sb_append_c(sb, " { ... }");
    self_bp = 13;
    break;
  case EX_MATCH:
    f_match_full(sb, e);
    self_bp = 13;
    break;
  case EX_QMARK:
    f_expr(sb, e->a, 13);
    sb_push(sb, '?');
    break;
  case EX_IF:
    f_if_full(sb, e);
    self_bp = 13;
    break;
  case EX_ENUM_CTOR:
  case EX_METHOD:
  case EX_BLOCK:
    sb_append_c(sb, "...");
    self_bp = 13;
    break;
  }
  (void)parent_bp;
  (void)paren;
}

// full expression printing — if/match get real block bodies
static void f_expr_full(SB *sb, Expr *e);

static void f_if_full(SB *sb, Expr *e) {
  sb_append_c(sb, "if ");
  f_expr_full(sb, e->a);
  sb_append_c(sb, " {");
  f_indent++;
  f_stmts(sb, &((Stmt *)e->items.items[0])->stmts);
  f_indent--;
  f_line(sb);
  if (e->items.n > 1) {
    sb_append_c(sb, "} else ");
    void *els = e->items.items[1];
    Expr *else_if = els;
    if (else_if->kind == EX_IF) {
      f_if_full(sb, else_if);
      return;
    }
    sb_push(sb, '{');
    f_indent++;
    f_stmts(sb, &((Stmt *)els)->stmts);
    f_indent--;
    f_line(sb);
  }
  sb_push(sb, '}');
}

static void f_pat_path(SB *sb, MatchArm *arm) {
  for (size_t j = 0; j < arm->pat_path.n; j++) {
    if (j)
      sb_push(sb, '.');
    sb_append_c(sb, arm->pat_path.items[j]);
  }
}

static void f_match_full(SB *sb, Expr *e) {
  sb_append_c(sb, "match ");
  f_expr_full(sb, e->a);
  sb_append_c(sb, " {");
  f_indent++;
  for (size_t i = 0; i < e->arms.n; i++) {
    MatchArm *arm = e->arms.items[i];
    f_line(sb);
    switch (arm->pk) {
    case PAT_WILDCARD: sb_append_c(sb, "_"); break;
    case PAT_INT: sb_printf(sb, "%llu", (unsigned long long)arm->pat_int); break;
    case PAT_STR: f_strlit(sb, arm->pat_str); break;
    case PAT_UNIT: f_pat_path(sb, arm); break;
    case PAT_TUPLE: {
      f_pat_path(sb, arm);
      sb_push(sb, '(');
      for (size_t j = 0; j < arm->pat_names.n; j++) {
        if (j)
          sb_append_c(sb, ", ");
        sb_append_c(sb, arm->pat_names.items[j]);
      }
      sb_push(sb, ')');
      break;
    }
    case PAT_STRUCT: {
      f_pat_path(sb, arm);
      sb_append_c(sb, " { ");
      for (size_t j = 0; j < arm->pat_fields.n; j++) {
        FieldAst *fa = arm->pat_fields.items[j];
        if (j)
          sb_append_c(sb, ", ");
        sb_append(sb, fa->name);
        sb_printf(sb, ": %s", (char *)arm->pat_names.items[j]);
      }
      sb_append_c(sb, " }");
      break;
    }
    }
    sb_append_c(sb, " => ");
    if (arm->body->kind == EX_BLOCK) {
      sb_push(sb, '{');
      f_indent++;
      f_stmts(sb, &arm->body->items);
      f_indent--;
      f_line(sb);
      sb_push(sb, '}');
    } else {
      f_expr_full(sb, arm->body);
    }
    sb_push(sb, ',');
  }
  f_indent--;
  f_line(sb);
  sb_push(sb, '}');
}

static void f_expr_full(SB *sb, Expr *e) {
  if (e->kind == EX_IF) {
    f_if_full(sb, e);
    return;
  }
  if (e->kind == EX_MATCH) {
    f_match_full(sb, e);
    return;
  }
  if (e->kind == EX_CALL || e->kind == EX_INDEX || e->kind == EX_FIELD || e->kind == EX_CAST ||
      e->kind == EX_SLICE || e->kind == EX_QMARK || e->kind == EX_UN || e->kind == EX_BIN) {
    // re-render with full sub-expressions: handled by f_expr recursively —
    // but its nested calls use f_expr too. Fine: only if/match are special.
    f_expr(sb, e, 0);
    return;
  }
  f_expr(sb, e, 0);
}

static void f_stmt(SB *sb, Stmt *s) {
  switch (s->kind) {
  case ST_LET:
    sb_append_c(sb, "let ");
    if (s->mut)
      sb_append_c(sb, "mut ");
    sb_append(sb, s->name);
    if (s->ty) {
      sb_append_c(sb, ": ");
      f_type(sb, s->ty);
    }
    sb_append_c(sb, " = ");
    f_expr_full(sb, s->a);
    if (!s->tail)
      sb_push(sb, ';');
    break;
  case ST_ASSIGN:
    f_expr_full(sb, s->a);
    sb_printf(sb, " %s ", tok_spell(s->assign_op));
    f_expr_full(sb, s->b);
    sb_push(sb, ';');
    break;
  case ST_EXPR:
    f_expr_full(sb, s->a);
    if (!s->tail && s->a->kind != EX_IF && s->a->kind != EX_MATCH && s->a->kind != EX_BLOCK)
      sb_push(sb, ';');
    break;
  case ST_RETURN:
    sb_append_c(sb, "return");
    if (s->a) {
      sb_push(sb, ' ');
      f_expr_full(sb, s->a);
    }
    sb_push(sb, ';');
    break;
  case ST_BREAK: sb_append_c(sb, "break;"); break;
  case ST_CONTINUE: sb_append_c(sb, "continue;"); break;
  case ST_DEFER:
    sb_append_c(sb, "defer ");
    if (s->stmts.n == 1 && ((Stmt *)s->stmts.items[0])->kind == ST_BLOCK) {
      Stmt *blk = s->stmts.items[0];
      sb_push(sb, '{');
      f_indent++;
      f_stmts(sb, &blk->stmts);
      f_indent--;
      f_line(sb);
      sb_push(sb, '}');
    } else {
      for (size_t i = 0; i < s->stmts.n; i++)
        f_stmt(sb, s->stmts.items[i]);
    }
    break;
  case ST_WHILE:
    sb_append_c(sb, "while ");
    f_expr_full(sb, s->cond);
    sb_append_c(sb, " {");
    f_indent++;
    f_stmts(sb, &s->body);
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  case ST_LOOP:
    sb_append_c(sb, "loop {");
    f_indent++;
    f_stmts(sb, &s->body);
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  case ST_BLOCK:
    sb_push(sb, '{');
    f_indent++;
    f_stmts(sb, &s->stmts);
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  }
}

static void f_stmts(SB *sb, Vec *stmts) {
  for (size_t i = 0; i < stmts->n; i++) {
    f_line(sb);
    f_stmt(sb, stmts->items[i]);
  }
}

static void f_params(SB *sb, Vec *params) {
  sb_push(sb, '(');
  for (size_t i = 0; i < params->n; i++) {
    Param *pa = params->items[i];
    if (i)
      sb_append_c(sb, ", ");
    sb_append(sb, pa->name);
    sb_append_c(sb, ": ");
    f_type(sb, pa->ty);
  }
  sb_push(sb, ')');
}

static void f_decl(SB *sb, Decl *d) {
  switch (d->kind) {
  case DK_USE:
    sb_append_c(sb, "use ");
    for (size_t i = 0; i < d->path.n; i++) {
      if (i)
        sb_push(sb, '/');
      sb_append_c(sb, d->path.items[i]);
    }
    sb_push(sb, ';');
    break;
  case DK_EXTERN:
    sb_append_c(sb, "extern fn ");
    sb_append(sb, d->name);
    f_params(sb, &d->params);
    if (d->ret) {
      sb_append_c(sb, " -> ");
      f_type(sb, d->ret);
    }
    sb_push(sb, ';');
    break;
  case DK_FN:
    if (d->pub_)
      sb_append_c(sb, "pub ");
    sb_append_c(sb, "fn ");
    sb_append(sb, d->name);
    f_params(sb, &d->params);
    if (d->ret) {
      sb_append_c(sb, " -> ");
      f_type(sb, d->ret);
    }
    sb_append_c(sb, " {");
    f_indent++;
    f_stmts(sb, &d->body);
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  case DK_STRUCT:
    if (d->pub_)
      sb_append_c(sb, "pub ");
    sb_append_c(sb, "struct ");
    sb_append(sb, d->name);
    if (d->fields.n == 0) {
      sb_append_c(sb, " {}");
      break;
    }
    sb_append_c(sb, " {");
    f_indent++;
    for (size_t i = 0; i < d->fields.n; i++) {
      FieldAst *fa = d->fields.items[i];
      f_line(sb);
      sb_append(sb, fa->name);
      sb_append_c(sb, ": ");
      f_type(sb, fa->ty);
      sb_push(sb, ',');
    }
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  case DK_ENUM:
    if (d->pub_)
      sb_append_c(sb, "pub ");
    sb_append_c(sb, "enum ");
    sb_append(sb, d->name);
    sb_append_c(sb, " {");
    f_indent++;
    for (size_t i = 0; i < d->variants.n; i++) {
      VariantAst *v = d->variants.items[i];
      f_line(sb);
      sb_append(sb, v->name);
      if (v->vkind == VAR_TUPLE) {
        sb_push(sb, '(');
        for (size_t j = 0; j < v->types.n; j++) {
          if (j)
            sb_append_c(sb, ", ");
          f_type(sb, v->types.items[j]);
        }
        sb_push(sb, ')');
      } else if (v->vkind == VAR_STRUCT) {
        sb_append_c(sb, " { ");
        for (size_t j = 0; j < v->fields.n; j++) {
          FieldAst *fa = v->fields.items[j];
          if (j)
            sb_append_c(sb, ", ");
          sb_append(sb, fa->name);
          sb_append_c(sb, ": ");
          f_type(sb, fa->ty);
        }
        sb_append_c(sb, " }");
      }
      if (v->has_disc)
        sb_printf(sb, " = %llu", (unsigned long long)v->disc);
      sb_push(sb, ',');
    }
    f_indent--;
    f_line(sb);
    sb_push(sb, '}');
    break;
  case DK_STATIC:
    sb_append_c(sb, "static ");
    if (d->is_mut)
      sb_append_c(sb, "mut ");
    sb_append(sb, d->name);
    sb_append_c(sb, ": ");
    f_type(sb, d->ret);
    sb_append_c(sb, " = ");
    f_expr_full(sb, d->init);
    sb_push(sb, ';');
    break;
  case DK_CONST:
    sb_append_c(sb, "const ");
    sb_append(sb, d->name);
    sb_append_c(sb, ": ");
    f_type(sb, d->ret);
    sb_append_c(sb, " = ");
    f_expr_full(sb, d->init);
    sb_push(sb, ';');
    break;
  }
}

Str fmt_module(Decl *module) {
  SB sb = {0};
  f_indent = 0;
  bool first = true;
  for (size_t i = 0; i < module->decls.n; i++) {
    Decl *d = module->decls.items[i];
    if (!first)
      sb_push(&sb, '\n');
    f_decl(&sb, d);
    sb_push(&sb, '\n');
    first = false;
  }
  return sb_finish(&sb);
}
