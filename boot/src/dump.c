#include "rho.h"

// Canonical s-expression dump of a parsed module. This is the AST's
// fingerprint: golden-pinned in tests, and later compared byte-for-byte
// between the boot compiler and the self-hosted one.

static void d_type(SB *sb, TypeAst *t);
static void d_expr(SB *sb, Expr *e);
static void d_stmt(SB *sb, Stmt *s);

static void d_str_lit(SB *sb, Str s) {
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

static void d_ident(SB *sb, const char *s) { sb_append_c(sb, s); }

static void d_type(SB *sb, TypeAst *t) {
  if (!t) {
    sb_append_c(sb, "void");
    return;
  }
  switch (t->kind) {
  case TA_PRIM:
    sb_append_c(sb, PRIM_NAMES[t->prim]);
    break;
  case TA_ARRAY:
    if (t->size) {
      sb_push(sb, '[');
      d_expr(sb, t->size);
      sb_push(sb, ']');
    } else {
      sb_append_c(sb, "[]");
    }
    d_type(sb, t->elem);
    break;
  case TA_PTR:
    sb_push(sb, '*');
    d_type(sb, t->elem);
    break;
  case TA_WEAK:
    sb_append_c(sb, "weak[");
    d_type(sb, t->elem);
    sb_push(sb, ']');
    break;
  case TA_NAMED:
    for (size_t i = 0; i < t->path.n; i++) {
      if (i)
        sb_push(sb, '.');
      d_ident(sb, t->path.items[i]);
    }
    if (t->targs.n) {
      sb_push(sb, '[');
      for (size_t i = 0; i < t->targs.n; i++) {
        if (i)
          sb_append_c(sb, ", ");
        d_type(sb, t->targs.items[i]);
      }
      sb_push(sb, ']');
    }
    break;
  case TA_FN: {
    sb_append_c(sb, "fn(");
    for (size_t i = 0; i < t->params.n; i++) {
      if (i)
        sb_append_c(sb, ", ");
      d_type(sb, t->params.items[i]);
    }
    sb_append_c(sb, ") -> ");
    d_type(sb, t->ret);
    break;
  }
  case TA_INFER:
    sb_append_c(sb, "_");
    break;
  }
}

static void d_expr(SB *sb, Expr *e) {
  if (!e) {
    sb_append_c(sb, "(void)");
    return;
  }
  switch (e->kind) {
  case EX_INT: sb_printf(sb, "(int %llu)", (unsigned long long)e->iv); break;
  case EX_FLOAT: {
    // canonical float printing: %g-ish but exact for the golden file
    char buf[64];
    snprintf(buf, sizeof(buf), "(float %.17g)", e->fv);
    sb_append_c(sb, buf);
    break;
  }
  case EX_STR:
    sb_append_c(sb, "(str ");
    d_str_lit(sb, e->sv);
    sb_push(sb, ')');
    break;
  case EX_BOOL: sb_printf(sb, "(bool %s)", e->bv ? "true" : "false"); break;
  case EX_NULL: sb_append_c(sb, "(null)"); break;
  case EX_NAME:
    sb_append_c(sb, "(name ");
    sb_append(sb, e->sv);
    sb_push(sb, ')');
    break;
  case EX_TYPE:
    sb_append_c(sb, "(ty ");
    d_type(sb, e->ty);
    sb_push(sb, ')');
    break;
  case EX_BIN:
    sb_printf(sb, "(bin %s ", tok_spell(e->binop));
    d_expr(sb, e->a);
    sb_push(sb, ' ');
    d_expr(sb, e->b);
    sb_push(sb, ')');
    break;
  case EX_UN:
    sb_printf(sb, "(un %s ", tok_spell(e->unop));
    d_expr(sb, e->a);
    sb_push(sb, ')');
    break;
  case EX_CALL:
    sb_append_c(sb, "(call ");
    d_expr(sb, e->a);
    for (size_t i = 0; i < e->args.n; i++) {
      sb_push(sb, ' ');
      if (e->arg_names.n && e->arg_names.items[i]) {
        sb_printf(sb, "%s: ", (char *)e->arg_names.items[i]);
      }
      if (((Expr *)e->args.items[i])->spread)
        sb_append_c(sb, "spread ");
      d_expr(sb, e->args.items[i]);
    }
    sb_push(sb, ')');
    break;
  case EX_INDEX:
    sb_append_c(sb, "(index ");
    d_expr(sb, e->a);
    sb_push(sb, ' ');
    d_expr(sb, e->b);
    sb_push(sb, ')');
    break;
  case EX_SLICE:
    sb_append_c(sb, "(slice ");
    d_expr(sb, e->a);
    sb_push(sb, ' ');
    d_expr(sb, e->b);
    sb_push(sb, ' ');
    d_expr(sb, e->c);
    sb_push(sb, ')');
    break;
  case EX_FIELD:
    sb_append_c(sb, "(field ");
    d_expr(sb, e->a);
    sb_printf(sb, " %.*s)", (int)e->sv.n, e->sv.p);
    break;
  case EX_CAST:
    sb_append_c(sb, "(cast ");
    d_type(sb, e->ty);
    sb_push(sb, ' ');
    d_expr(sb, e->a);
    sb_push(sb, ')');
    break;
  case EX_NEW:
    sb_append_c(sb, "(new ");
    d_type(sb, e->ty);
    for (size_t i = 0; i < e->items.n; i++) {
      FieldAst *fa = e->items.items[i];
      sb_printf(sb, " (%.*s ", (int)fa->name.n, fa->name.p);
      d_expr(sb, e->args.items[i]);
      sb_push(sb, ')');
    }
    sb_push(sb, ')');
    break;
  case EX_MAKE:
    sb_append_c(sb, "(make ");
    d_type(sb, e->ty);
    sb_push(sb, ' ');
    d_expr(sb, e->c);
    sb_push(sb, ')');
    break;
  case EX_CLOSURE: {
    sb_append_c(sb, "(closure (params");
    for (size_t i = 0; i < e->params.n; i++) {
      Param *pa = e->params.items[i];
      sb_printf(sb, " (%.*s ", (int)pa->name.n, pa->name.p);
      d_type(sb, pa->ty);
      sb_push(sb, ')');
    }
    sb_append_c(sb, ") -> ");
    d_type(sb, e->ret);
    for (size_t i = 0; i < e->items.n; i++)
      d_stmt(sb, e->items.items[i]);
    sb_push(sb, ')');
    break;
  }
  case EX_MATCH: {
    sb_append_c(sb, "(match ");
    d_expr(sb, e->a);
    for (size_t i = 0; i < e->arms.n; i++) {
      MatchArm *arm = e->arms.items[i];
      sb_append_c(sb, " (arm ");
      switch (arm->pk) {
      case PAT_WILDCARD: sb_append_c(sb, "_"); break;
      case PAT_INT: sb_printf(sb, "%llu", (unsigned long long)arm->pat_int); break;
      case PAT_STR: d_str_lit(sb, arm->pat_str); break;
      case PAT_UNIT:
        for (size_t j = 0; j < arm->pat_path.n; j++) {
          if (j)
            sb_push(sb, '.');
          d_ident(sb, arm->pat_path.items[j]);
        }
        break;
      case PAT_TUPLE:
        for (size_t j = 0; j < arm->pat_path.n; j++) {
          if (j)
            sb_push(sb, '.');
          d_ident(sb, arm->pat_path.items[j]);
        }
        sb_append_c(sb, "(");
        for (size_t j = 0; j < arm->pat_names.n; j++)
          sb_printf(sb, "%s%s", j ? " " : "", (char *)arm->pat_names.items[j]);
        sb_push(sb, ')');
        break;
      case PAT_STRUCT:
        for (size_t j = 0; j < arm->pat_path.n; j++) {
          if (j)
            sb_push(sb, '.');
          d_ident(sb, arm->pat_path.items[j]);
        }
        sb_append_c(sb, "{");
        for (size_t j = 0; j < arm->pat_fields.n; j++) {
          FieldAst *fa = arm->pat_fields.items[j];
          sb_printf(sb, "%s", j ? " " : "");
          sb_append(sb, fa->name); // Str: not NUL-terminated in place
          sb_printf(sb, " %s", (char *)arm->pat_names.items[j]);
        }
        sb_push(sb, '}');
        break;
      }
      sb_push(sb, ' ');
      d_expr(sb, arm->body);
      sb_push(sb, ')');
    }
    sb_push(sb, ')');
    break;
  }
  case EX_QMARK:
    sb_append_c(sb, "(qmark ");
    d_expr(sb, e->a);
    sb_push(sb, ')');
    break;
  case EX_ENUM_CTOR: sb_append_c(sb, "(enum-ctor)"); break;
  case EX_METHOD: sb_append_c(sb, "(method)"); break;
  case EX_IF: {
    sb_append_c(sb, "(if ");
    d_expr(sb, e->a);
    sb_append_c(sb, " (block");
    Stmt *then = e->items.n > 0 ? e->items.items[0] : NULL;
    if (then)
      for (size_t i = 0; i < then->stmts.n; i++)
        d_stmt(sb, then->stmts.items[i]);
    sb_push(sb, ')');
    if (e->items.n > 1) {
      void *els = e->items.items[1];
      Expr *else_if = els;
      if (else_if->kind == EX_IF) {
        sb_append_c(sb, " ");
        d_expr(sb, else_if);
      } else {
        sb_append_c(sb, " (block");
        Stmt *else_blk = els;
        for (size_t i = 0; i < else_blk->stmts.n; i++)
          d_stmt(sb, else_blk->stmts.items[i]);
        sb_push(sb, ')');
      }
    }
    sb_push(sb, ')');
    break;
  }
  case EX_BLOCK:
    sb_append_c(sb, "(block");
    for (size_t i = 0; i < e->items.n; i++)
      d_stmt(sb, e->items.items[i]);
    sb_push(sb, ')');
    break;
  }
}

static void d_stmt(SB *sb, Stmt *s) {
  switch (s->kind) {
  case ST_LET:
    sb_printf(sb, " (let%s %.*s ", s->mut ? " mut" : "", (int)s->name.n, s->name.p);
    d_type(sb, s->ty);
    sb_push(sb, ' ');
    d_expr(sb, s->a);
    sb_push(sb, ')');
    break;
  case ST_ASSIGN:
    sb_printf(sb, " (assign %s ", tok_spell(s->assign_op));
    d_expr(sb, s->a);
    sb_push(sb, ' ');
    d_expr(sb, s->b);
    sb_push(sb, ')');
    break;
  case ST_EXPR:
    sb_append_c(sb, s->tail ? " (tail " : " (expr ");
    d_expr(sb, s->a);
    sb_push(sb, ')');
    break;
  case ST_RETURN:
    sb_append_c(sb, " (return");
    if (s->a) {
      sb_push(sb, ' ');
      d_expr(sb, s->a);
    }
    sb_push(sb, ')');
    break;
  case ST_BREAK: sb_append_c(sb, " (break)"); break;
  case ST_CONTINUE: sb_append_c(sb, " (continue)"); break;
  case ST_DEFER:
    sb_append_c(sb, " (defer");
    for (size_t i = 0; i < s->stmts.n; i++)
      d_stmt(sb, s->stmts.items[i]);
    sb_push(sb, ')');
    break;
  case ST_WHILE:
    sb_append_c(sb, " (while ");
    d_expr(sb, s->cond);
    sb_append_c(sb, " (block");
    for (size_t i = 0; i < s->body.n; i++)
      d_stmt(sb, s->body.items[i]);
    sb_append_c(sb, "))");
    break;
  case ST_LOOP:
    sb_append_c(sb, " (loop (block");
    for (size_t i = 0; i < s->body.n; i++)
      d_stmt(sb, s->body.items[i]);
    sb_append_c(sb, "))");
    break;
  case ST_BLOCK:
    sb_append_c(sb, " (block");
    for (size_t i = 0; i < s->stmts.n; i++)
      d_stmt(sb, s->stmts.items[i]);
    sb_push(sb, ')');
    break;
  }
}

static void d_decl(SB *sb, Decl *d) {
  switch (d->kind) {
  case DK_FN:
    sb_printf(sb, " (fn%s %.*s (params", d->pub_ ? " pub" : "", (int)d->name.n, d->name.p);
    for (size_t i = 0; i < d->params.n; i++) {
      Param *pa = d->params.items[i];
      sb_printf(sb, " (%.*s%s ", (int)pa->name.n, pa->name.p,
                pa->is_variadic ? "..." : "");
      d_type(sb, pa->ty);
      sb_push(sb, ')');
    }
    sb_append_c(sb, ")");
    if (d->ret) {
      sb_append_c(sb, " (ret ");
      d_type(sb, d->ret);
      sb_push(sb, ')');
    }
    for (size_t i = 0; i < d->body.n; i++)
      d_stmt(sb, d->body.items[i]);
    sb_push(sb, ')');
    break;
  case DK_EXTERN:
    sb_printf(sb, " (extern %.*s (params", (int)d->name.n, d->name.p);
    for (size_t i = 0; i < d->params.n; i++) {
      Param *pa = d->params.items[i];
      sb_printf(sb, " (%.*s ", (int)pa->name.n, pa->name.p);
      d_type(sb, pa->ty);
      sb_push(sb, ')');
    }
    sb_append_c(sb, ")");
    if (d->ret) {
      sb_append_c(sb, " (ret ");
      d_type(sb, d->ret);
      sb_push(sb, ')');
    }
    sb_push(sb, ')');
    break;
  case DK_STRUCT:
    sb_printf(sb, " (struct%s %.*s", d->pub_ ? " pub" : "", (int)d->name.n, d->name.p);
    for (size_t i = 0; i < d->fields.n; i++) {
      FieldAst *fa = d->fields.items[i];
      sb_printf(sb, " (%.*s ", (int)fa->name.n, fa->name.p);
      d_type(sb, fa->ty);
      sb_push(sb, ')');
    }
    sb_push(sb, ')');
    break;
  case DK_ENUM:
    sb_printf(sb, " (enum%s %.*s", d->pub_ ? " pub" : "", (int)d->name.n, d->name.p);
    for (size_t i = 0; i < d->variants.n; i++) {
      VariantAst *v = d->variants.items[i];
      sb_printf(sb, " (%.*s %llu", (int)v->name.n, v->name.p, (unsigned long long)v->disc);
      if (v->vkind == VAR_TUPLE) {
        for (size_t j = 0; j < v->types.n; j++) {
          sb_push(sb, ' ');
          d_type(sb, v->types.items[j]);
        }
      } else if (v->vkind == VAR_STRUCT) {
        for (size_t j = 0; j < v->fields.n; j++) {
          FieldAst *fa = v->fields.items[j];
          sb_printf(sb, " (%.*s ", (int)fa->name.n, fa->name.p);
          d_type(sb, fa->ty);
          sb_push(sb, ')');
        }
      }
      sb_push(sb, ')');
    }
    sb_push(sb, ')');
    break;
  case DK_STATIC:
  case DK_CONST:
    sb_printf(sb, " (%s%s %.*s ", d->kind == DK_CONST ? "const" : "static",
              d->is_mut ? " mut" : "", (int)d->name.n, d->name.p);
    d_type(sb, d->ret);
    sb_push(sb, ' ');
    d_expr(sb, d->init);
    sb_push(sb, ')');
    break;
  case DK_USE:
    sb_append_c(sb, " (use");
    for (size_t i = 0; i < d->path.n; i++)
      sb_printf(sb, " %s", (char *)d->path.items[i]);
    sb_push(sb, ')');
    break;
  }
}

Str dump_module(Decl *module) {
  SB sb = {0};
  sb_append_c(&sb, "(module");
  for (size_t i = 0; i < module->decls.n; i++)
    d_decl(&sb, module->decls.items[i]);
  sb_append_c(&sb, ")\n");
  return sb_finish(&sb);
}
