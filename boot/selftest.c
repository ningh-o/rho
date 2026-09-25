// selftest.c — `rho selftest`: unit tests for the skeleton pieces
// (arena, str, intern, vec, diag, node pool, AST dump). Runs in
// milliseconds; a failure prints the failing check and exits 1.
#include "rho.h"

static int g_fail;
static const char *g_suite;

#define SUITE(name) g_suite = name
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,       \
              g_suite ? g_suite : "?", #cond);                          \
      g_fail++;                                                         \
    }                                                                   \
  } while (0)

static void test_arena(void) {
  SUITE("arena");
  Arena *a = arena_new(256);
  char *p1 = arena_alloc(a, 100, 1);
  memcpy(p1, "abcdefghij", 10);
  char *p2 = arena_alloc(a, 100, 8); // forces a chained block
  CHECK(p2 != NULL);
  char *p3 = arena_alloc(a, 4096, 16); // big alloc: own block
  CHECK(p3 != NULL);
  CHECK(memcmp(p1, "abcdefghij", 10) == 0); // p1 survives growth
  char *s = aprintf(a, "x=%d y=%s", 42, "hi");
  CHECK(strcmp(s, "x=42 y=hi") == 0);
  char *d = astrdup(a, "copy");
  CHECK(strcmp(d, "copy") == 0);
}

static void test_str(void) {
  SUITE("str");
  CHECK(str_eq(STR("abc"), str_slice("abc", 3)));
  CHECK(!str_eq(STR("abc"), STR("abd")));
  CHECK(!str_eq(STR("abc"), STR("ab")));
  CHECK(str_eq_c(STR("hello"), "hello"));
  CHECK(!str_eq_c(STR("hello"), "hell"));
  CHECK(str_eq(STR(""), str_slice("", 0)));
}

static void test_intern(void) {
  SUITE("intern");
  const char *a = intern("alpha", 5);
  const char *b = intern_c("alpha");
  CHECK(a == b); // identical spelling, identical pointer
  const char *c = intern("beta", 4);
  CHECK(a != c);
  char long1[300], long2[300];
  memset(long1, 'k', sizeof long1);
  memcpy(long2, long1, sizeof long2);
  CHECK(intern(long1, sizeof long1) == intern(long2, sizeof long2));
}

static void test_vec(void) {
  SUITE("vec");
  VEC(uint64_t, v);
  for (uint64_t i = 0; i < 1000; i++)
    *VPUSH(v, uint64_t) = i * 3;
  CHECK(VLEN(v) == 1000);
  CHECK(*VAT(v, uint64_t, 0) == 0);
  CHECK(*VAT(v, uint64_t, 999) == 999 * 3);
  VEC(Diag, d2);
  Diag *dd = VPUSH(d2, Diag);
  dd->line = 7;
  CHECK(VAT(d2, Diag, 0)->line == 7);
}

static void test_diag(void) {
  SUITE("diag");
  size_t before = diags_count();
  diag_at(DIAG_ERROR, "t.rho", 3, 5, "unknown name %s", "x");
  CHECK(diags_count() == before + 1);
  CHECK(g_had_error);
  char *buf = NULL;
  size_t cap = 0;
  FILE *ms = open_memstream(&buf, &cap);
  diags_print(ms);
  fclose(ms);
  CHECK(strstr(buf, "t.rho:3:5: error: unknown name x") != NULL);
  free(buf);
}

// Build a small AST by hand and dump it: fn main() { printf("hi"); }
static void test_dump(void) {
  SUITE("dump");
  const char *f = "t.rho";
  NodeRef strlit = node_new(NT_STR, f, 2, 12);
  node_get(strlit)->sval = STR("hi\\n");
  NodeRef callee = node_new(NT_PATH, f, 2, 5);
  node_get(callee)->name = intern_c("printf");
  RefList *args = reflist();
  reflist_add(args, strlit);
  NodeRef call = node_new(NT_CALL, f, 2, 5);
  node_get(call)->a = callee;
  node_get(call)->list = args;
  NodeRef stmt = node_new(NT_EXPRSTMT, f, 2, 5);
  node_get(stmt)->a = call;
  RefList *body = reflist();
  reflist_add(body, stmt);
  NodeRef fn = node_new(NT_FN, f, 1, 1);
  node_get(fn)->name = intern_c("main");
  node_get(fn)->list = reflist();          // params
  node_get(fn)->c = NO_REF;                // no ret
  RefList *blk = reflist();
  reflist_add(blk, stmt);
  // body wraps in a block-less list: dump uses fn.d = block node
  NodeRef blknode = node_new(NT_EXPRSTMT, f, 1, 1);
  (void)blknode;
  (void)body;

  char *buf = NULL;
  size_t cap = 0;
  FILE *ms = open_memstream(&buf, &cap);
  dump_node(ms, node_get(fn));
  fclose(ms);
  CHECK(strcmp(buf, "(fn main () - -)") == 0);
  free(buf);

  // dump the call expression instead
  buf = NULL;
  cap = 0;
  ms = open_memstream(&buf, &cap);
  dump_node(ms, node_get(call));
  fclose(ms);
  CHECK(strcmp(buf, "(call printf \"hi\\\\n\")") == 0);
  free(buf);
}

static void test_nodes(void) {
  SUITE("nodes");
  NodeRef r = node_new(NT_LET, "t.rho", 4, 2);
  Node *n = node_get(r);
  n->name = intern_c("x");
  n->bval = true; // mut
  CHECK(n->kind == NT_LET && strcmp(n->name, "x") == 0);
  CHECK(n->a == NO_REF && n->list == NULL);
  RefList *l = reflist();
  reflist_add(l, r);
  reflist_add(l, r);
  CHECK(reflist_len(l) == 2 && reflist_at(l, 1) == r);
}

// ---------------------------------------------------------------- lexer

static Token *lex_sample(const char *src, size_t *n) {
  Lexer *lx = lex_file(g_arena, "t.rho", src);
  size_t cnt;
  const Token *ts = lex_tokens(lx, &cnt);
  Token *copy = malloc(cnt * sizeof(Token));
  memcpy(copy, ts, cnt * sizeof(Token));
  if (n)
    *n = cnt;
  return copy;
}

static void test_lexer_basic(void) {
  SUITE("lexer-basic");
  size_t n;
  Token *t = lex_sample("fn main() -> i32 { return 0; }", &n);
  TokKind want[] = {K_FN,   T_IDENT, T_LPAREN, T_RPAREN, T_ARROW,
                    K_I32,  T_LBRACE, K_RETURN, T_INT,   T_SEMI,
                    T_RBRACE, T_EOF};
  CHECK(n == sizeof want / sizeof want[0]);
  for (size_t i = 0; i < n && i < sizeof want / sizeof want[0]; i++)
    CHECK(t[i].kind == want[i]);
  CHECK(t[1].text.n == 4 && memcmp(t[1].text.p, "main", 4) == 0);
  CHECK(t[8].i == 0);
  free(t);
}

static void test_lexer_numbers(void) {
  SUITE("lexer-numbers");
  size_t n;
  Token *t = lex_sample("1_000 0x1F_ff 0b1010 3.25 2e10 1.5e-3 9", &n);
  CHECK(n == 8); // 7 tokens + EOF
  CHECK(t[0].kind == T_INT && t[0].i == 1000);
  CHECK(t[1].kind == T_INT && t[1].i == 0x1FFF);
  CHECK(t[2].kind == T_INT && t[2].i == 10);
  CHECK(t[3].kind == T_FLOAT && t[3].f == 3.25);
  CHECK(t[4].kind == T_FLOAT && t[4].f == 2e10);
  CHECK(t[5].kind == T_FLOAT && t[5].f == 1.5e-3);
  CHECK(t[6].kind == T_INT && t[6].i == 9);
  free(t);

  t = lex_sample("18446744073709551616", &n); // 2^64: too large
  CHECK(g_had_error == true);                 // error recorded
  free(t);
}

static void test_lexer_strings(void) {
  SUITE("lexer-strings");
  size_t n;
  Token *t = lex_sample("\"a\\nb\\tc\\\\d\\\"e\\x41\\u{1F600}\"", &n);
  CHECK(n == 2);
  static const unsigned char want[] = {'a',  '\n', 'b',  '\t', 'c',
                                       '\\', 'd',  '"',  'e',  'A',
                                       0xF0, 0x9F, 0x98, 0x80};
  CHECK(t[0].kind == T_STRING);
  CHECK(t[0].text.n == sizeof want);
  CHECK(memcmp(t[0].text.p, want, sizeof want) == 0);
  free(t);

  // triple-quoted: fully verbatim — escapes never decode inside
  t = lex_sample("\"\"\"a\\nb \"\"\"", &n);
  CHECK(n == 2); // TSTRING + EOF
  CHECK(t[0].kind == T_TSTRING);
  CHECK(t[0].text.n == 5);
  CHECK(memcmp(t[0].text.p, "a\\nb ", 5) == 0); // backslash is a byte
  free(t);

  // the first """ closes, mid-line; text may run right up to it
  t = lex_sample("\"\"\"ab\"\"\"cd", &n);
  CHECK(n == 3); // TSTRING, IDENT(cd), EOF
  CHECK(t[0].text.n == 2 && memcmp(t[0].text.p, "ab", 2) == 0);
  CHECK(t[1].kind == T_IDENT && t[1].text.n == 2 &&
        memcmp(t[1].text.p, "cd", 2) == 0);
  free(t);

  // two quotes inside are content, not a closer
  t = lex_sample("\"\"\"x\"\"y\"\"\"", &n);
  CHECK(n == 2);
  CHECK(t[0].text.n == 4 && memcmp(t[0].text.p, "x\"\"y", 4) == 0);
  free(t);

  // unterminated triple = error
  t = lex_sample("\"\"\"xyz", &n);
  CHECK(g_had_error);
  free(t);
  g_had_error = false;
}

static void test_lexer_operators(void) {
  SUITE("lexer-operators");
  size_t n;
  Token *t = lex_sample(">>= >>= <<= << >> && || == != <= >= => "
                        "-> ... += -= *= /= %= &= |= ^= . : ; ,",
                        &n);
  TokKind want[] = {T_SHREQ,  T_SHREQ, T_SHLEQ, T_SHL,    T_SHR,
                    T_ANDAND, T_OROR,  T_EQEQ,  T_NE,     T_LE,
                    T_GE,     T_FATARROW, T_ARROW, T_ELLIPSIS, T_PLUSEQ,
                    T_DASHEQ, T_STAREQ, T_SLASHEQ, T_PCTEQ, T_AMPEQ,
                    T_PIPEEQ, T_CARETEQ, T_DOT,  T_COLON,  T_SEMI,
                    T_COMMA,  T_EOF};
  CHECK(n == sizeof want / sizeof want[0]);
  for (size_t i = 0; i < n && i < sizeof want / sizeof want[0]; i++)
    CHECK(t[i].kind == want[i]);
  free(t);
}

static void test_lexer_comments(void) {
  SUITE("lexer-comments");
  size_t n;
  Token *t = lex_sample("// line comment\n42 // trailing\n", &n);
  CHECK(n == 2);
  CHECK(t[0].kind == T_INT && t[0].i == 42);
  CHECK(t[1].kind == T_EOF);
  free(t);
}

static void test_lexer_errors(void) {
  SUITE("lexer-errors");
  size_t n;
  Token *t = lex_sample("\"abc\\q\"", &n); // bad escape
  CHECK(g_had_error);
  free(t);

  g_had_error = false;
  t = lex_sample("\"unterminated", &n);
  CHECK(g_had_error);
  free(t);

  g_had_error = false;
  t = lex_sample("0x", &n); // malformed hex
  CHECK(g_had_error);
  free(t);

  g_had_error = false;
  t = lex_sample("12ab", &n); // ident glued to number
  CHECK(g_had_error);
  free(t);
  g_had_error = false;
}

// ---------------------------------------------------------------- parser

static Module *parse_src(const char *src) {
  return module_parse_src("t.rho", src);
}

static void test_parse_fn(void) {
  SUITE("parse-fn");
  Module *m = parse_src("fn main() -> i32 { printf(\"hi\\n\"); return 0; }");
  CHECK(!g_had_error);
  CHECK(reflist_len(m->decls) == 1);
  Node *fn = node_get(reflist_at(m->decls, 0));
  CHECK(fn->kind == NT_FN && str_eq_c(str_slice(fn->name, strlen(fn->name)), "main"));
  CHECK(reflist_len(fn->list) == 0); // params
  CHECK(fn->d != NO_REF);
  CHECK(node_get(fn->c)->kind == NT_BUILTIN);
}

static void test_parse_method(void) {
  SUITE("parse-method");
  Module *m = parse_src(
      "struct Rect { w: i32, h: i32 }\n"
      "fn Rect.area(self: *Rect) -> i32 { return self.w; }\n"
      "fn Rect.square(n: i32) -> *Rect { return new Rect { w: n, h: n }; }");
  CHECK(!g_had_error);
  CHECK(reflist_len(m->decls) == 3);
  Node *meth = node_get(reflist_at(m->decls, 1));
  CHECK(meth->kind == NT_FN && meth->name2 != NULL);
  CHECK(strcmp(meth->name2, "Rect") == 0 && strcmp(meth->name, "area") == 0);
  Node *p0 = node_get(reflist_at(meth->list, 0));
  CHECK(strcmp(p0->name, "self") == 0);
  CHECK(node_get(p0->a)->kind == NT_PTR);
}

static void test_parse_precedence(void) {
  SUITE("parse-precedence");
  Module *m = parse_src("fn f(a: i32, b: i32, c: i32) -> i32 { return a + b * c; }");
  Node *fn = node_get(reflist_at(m->decls, 0));
  Node *body = node_get(fn->d);
  Node *ret = node_get(reflist_at(body->list, 0));
  Node *e = node_get(ret->a);
  CHECK(e->kind == NT_BINARY && e->op == OP_ADD);
  Node *rhs = node_get(e->b);
  CHECK(rhs->kind == NT_BINARY && rhs->op == OP_MUL);

  m = parse_src("fn f(a: i32, b: i32, c: i32) -> i32 { return a & b | c; }");
  fn = node_get(reflist_at(m->decls, 0));
  body = node_get(fn->d);
  ret = node_get(reflist_at(body->list, 0));
  e = node_get(ret->a);
  CHECK(e->kind == NT_BINARY && e->op == OP_BOR); // (a&b)|c per C ladder
  CHECK(node_get(e->a)->op == OP_BAND);

  m = parse_src("fn f(x: i32) -> i64 { return x as i64 + 1; }");
  fn = node_get(reflist_at(m->decls, 0));
  body = node_get(fn->d);
  ret = node_get(reflist_at(body->list, 0));
  e = node_get(ret->a);
  CHECK(e->kind == NT_BINARY && e->op == OP_ADD);
  CHECK(node_get(e->a)->kind == NT_AS); // (x as i64) + 1
}

static void test_parse_labels(void) {
  SUITE("parse-labels");
  Module *m = parse_src(
      "fn f() -> i32 {\n"
      "  let mut i: i32 = 0;\n"
      "  outer: while i < 10 {\n"
      "    i += 1;\n"
      "    inner: loop {\n"
      "      break outer;\n"
      "    }\n"
      "    continue outer;\n"
      "  }\n"
      "  return i;\n"
      "}");
  CHECK(!g_had_error);
  Node *fn = node_get(reflist_at(m->decls, 0));
  Node *body = node_get(fn->d);
  Node *wh = node_get(reflist_at(body->list, 1));
  CHECK(wh->kind == NT_WHILE && wh->name && strcmp(wh->name, "outer") == 0);
  Node *lp = node_get(reflist_at(node_get(wh->b)->list, 1));
  CHECK(lp->kind == NT_LOOP && lp->name && strcmp(lp->name, "inner") == 0);
  Node *br = node_get(reflist_at(node_get(lp->b)->list, 0));
  CHECK(br->kind == NT_BREAK && br->name && strcmp(br->name, "outer") == 0);
}

static void test_parse_enum_match(void) {
  SUITE("parse-enum-match");
  Module *m = parse_src(
      "enum Shape { Circle(f64), Rect { w: i64, h: i64 }, Point }\n"
      "fn area(s: Shape) -> i64 {\n"
      "  return match s {\n"
      "    Shape.Circle(r) => r as i64,\n"
      "    Shape.Rect { w, h } => w * h,\n"
      "    Shape.Point => 0,\n"
      "  };\n"
      "}");
  CHECK(!g_had_error);
  Node *en = node_get(reflist_at(m->decls, 0));
  CHECK(en->kind == NT_ENUM && reflist_len(en->list) == 3);
  Node *v0 = node_get(reflist_at(en->list, 0));
  CHECK(v0->op == VAR_TUPLE && strcmp(v0->name, "Circle") == 0);
  Node *v1 = node_get(reflist_at(en->list, 1));
  CHECK(v1->op == VAR_STRUCT);
  Node *v2 = node_get(reflist_at(en->list, 2));
  CHECK(v2->op == VAR_UNIT);
  Node *fn = node_get(reflist_at(m->decls, 1));
  Node *body = node_get(fn->d);
  Node *ret = node_get(reflist_at(body->list, 0));
  Node *match = node_get(ret->a);
  CHECK(match->kind == NT_MATCH_EXPR && reflist_len(match->list) == 3);
  Node *arm = node_get(reflist_at(match->list, 1));
  Node *pat = node_get(arm->a);
  CHECK(pat->kind == NT_PVAR && pat->op == VAR_STRUCT);
  CHECK(strcmp(pat->name, "Shape.Rect") == 0);
}

static void test_parse_generics_use(void) {
  SUITE("parse-generics-use");
  Module *m = parse_src(
      "use geom.units;\n"
      "pub use web.strs;\n"
      "pub use web.strs.trim as tr;\n"
      "pub use web.*;\n"
      "fn twice[T: Show, Eq](x: T) -> T { return x; }");
  CHECK(!g_had_error);
  Node *u0 = node_get(reflist_at(m->decls, 0));
  CHECK(u0->kind == NT_USE && u0->op == USE_PLAIN);
  CHECK(reflist_len(u0->list) == 2);
  Node *u1 = node_get(reflist_at(m->decls, 1));
  CHECK(u1->op == USE_PUB_ITEM);
  Node *u2 = node_get(reflist_at(m->decls, 2));
  CHECK(u2->op == USE_PUB_AS && strcmp(u2->name2, "tr") == 0);
  Node *u3 = node_get(reflist_at(m->decls, 3));
  CHECK(u3->op == USE_PUB_STAR);
  Node *fn = node_get(reflist_at(m->decls, 4));
  Node *g = node_get(fn->a);
  CHECK(g && g->kind == NT_APP && reflist_len(g->list) == 1);
  Node *gp = node_get(reflist_at(g->list, 0));
  CHECK(gp->kind == NT_GPARAM && strcmp(gp->name, "T") == 0);
  CHECK(reflist_len(gp->list) == 2); // bounds Show, Eq absorb
}

static void test_parse_misc_decls(void) {
  SUITE("parse-misc-decls");
  Module *m = parse_src(
      "trait Show { fn to_str(self) -> string, fn size(self) -> i32 }\n"
      "impl Show for i32 { fn to_str(self) -> string { return \"i\"; } }\n"
      "const N: i64 = 1_000;\n"
      "const M = 42;\n"
      "static mut ACC: f64 = 0.5;\n"
      "extern puts: fn(string) -> i32;\n"
      "fn q(o: Option[i32]) -> ?i32 { return o?; }\n"
      "fn v(ns: i64...) -> i64 { return len(ns); }\n"
      "fn cl() -> fn(i32) -> i32 { return fn(x: i32) -> i32 { return x; }; }\n"
      "fn sl(s: []u8, i: usize) -> u8 { return s[i]; }");
  CHECK(!g_had_error);
  CHECK(reflist_len(m->decls) == 10);
  Node *tr = node_get(reflist_at(m->decls, 0));
  CHECK(tr->kind == NT_TRAIT && reflist_len(tr->list) == 2);
  Node *im = node_get(reflist_at(m->decls, 1));
  CHECK(im->kind == NT_IMPL && strcmp(im->name, "Show") == 0);
  Node *c0 = node_get(reflist_at(m->decls, 2));
  CHECK(c0->kind == NT_CONST && c0->a != NO_REF);
  Node *c1 = node_get(reflist_at(m->decls, 3));
  CHECK(c1->kind == NT_CONST && c1->a == NO_REF);
  Node *st = node_get(reflist_at(m->decls, 4));
  CHECK(st->kind == NT_STATIC);
  Node *ex = node_get(reflist_at(m->decls, 5));
  CHECK(ex->kind == NT_EXTERN && node_get(ex->a)->kind == NT_FNTYPE);
  Node *q = node_get(reflist_at(m->decls, 6));
  Node *qp = node_get(reflist_at(q->list, 0));
  CHECK(node_get(qp->a)->kind == NT_APP); // Option[i32]
  Node *qr = node_get(q->c);
  CHECK(qr->kind == NT_OPT); // ?i32
  Node *vf = node_get(reflist_at(m->decls, 7));
  Node *vp = node_get(reflist_at(vf->list, 0));
  CHECK(vp->op == 2); // variadic
  Node *cf = node_get(reflist_at(m->decls, 8));
  CHECK(node_get(cf->c)->kind == NT_FNTYPE);
}

static void test_parse_errors(void) {
  SUITE("parse-errors");
  g_had_error = false;
  parse_src("fn f() { let x = null; }");
  CHECK(g_had_error); // null rejected with the non-null message
  g_had_error = false;
  parse_src("fn f( { }");
  CHECK(g_had_error);
  g_had_error = false;
  parse_src("fn f() -> i32 { return 0 }"); // missing semi
  CHECK(g_had_error);
  g_had_error = false;
  parse_src("fn 5() {}");
  CHECK(g_had_error);
  g_had_error = false;
}

static void test_parse_corpus_forms(void) {
  SUITE("parse-corpus-forms");
  // the full-grammar forms the archive corpus pinned during the sweep
  Module *m = parse_src(
      "fn f(o: Option[i32]) -> i32 {\n"
      "  let xs: []i32 = make([]i32, 5);\n"
      "  let ys: []i64 = make([]i64, 3);\n"
      "  let p: i32 = *new Pair { a: 1, b: 2 }.a;\n"
      "  let v: Shape = Shape.Rect(w: 7, h: 3);\n"
      "  let h: string = s[1..3];\n"
      "  let t: string = s[..2];\n"
      "  let u: string = s[2..];\n"
      "  let b: i32 = ~0 & 0xFF;\n"
      "  defer COUNT += 1;\n"
      "  return match o {\n"
      "    Option.Some(v) => v,\n"
      "    Option.None => {\n"
      "      let d: i32 = 4;\n"
      "      d\n"
      "    },\n"
      "  };\n"
      "}\n");
  CHECK(!g_had_error);
  CHECK(reflist_len(m->decls) == 1);
  Node *fn = node_get(reflist_at(m->decls, 0));
  Node *body = node_get(fn->d);
  CHECK(reflist_len(body->list) == 10);
  Node *mk = node_get(reflist_at(body->list, 0));
  Node *mkcall = node_get(mk->b);
  CHECK(mkcall->kind == NT_CALL);
  Node *mkarg0 = node_get(reflist_at(mkcall->list, 0));
  CHECK(mkarg0->op == 1 && node_get(mkarg0->a)->kind == NT_SLICE);
  Node *deref = node_get(reflist_at(body->list, 2));
  CHECK(node_get(deref->b)->kind == NT_UNARY);
  CHECK(node_get(deref->b)->op == OP_DEREF);
  Node *vc = node_get(reflist_at(body->list, 3));
  Node *mcall = node_get(vc->b);
  CHECK(mcall->kind == NT_METHOD); // Shape.Rect(...)
  CHECK(node_get(reflist_at(mcall->list, 0))->kind == NT_FIELDINIT);
  Node *sl = node_get(reflist_at(body->list, 4));
  CHECK(node_get(sl->b)->kind == NT_SLICE_E);
  Node *df = node_get(reflist_at(body->list, 8));
  CHECK(df->kind == NT_DEFER && node_get(df->a)->kind == NT_ASSIGN);
  Node *mt = node_get(reflist_at(body->list, 9));
  CHECK(node_get(mt->a)->kind == NT_MATCH_EXPR);
  Node *arm1 = node_get(reflist_at(node_get(mt->a)->list, 1));
  Node *blk = node_get(arm1->b);
  Node *tail = node_get(reflist_at(blk->list, 1));
  CHECK(tail->kind == NT_EXPRSTMT && tail->bval); // tail expression
}

int cmd_selftest(void) {
  test_arena();
  test_str();
  test_intern();
  test_vec();
  test_diag();
  test_nodes();
  test_dump();
  test_lexer_basic();
  test_lexer_numbers();
  test_lexer_strings();
  test_lexer_operators();
  test_lexer_comments();
  test_lexer_errors();
  test_parse_fn();
  test_parse_method();
  test_parse_precedence();
  test_parse_labels();
  test_parse_enum_match();
  test_parse_generics_use();
  test_parse_misc_decls();
  test_parse_corpus_forms();
  test_parse_errors();
  if (g_fail) {
    fprintf(stderr, "selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("selftest: ok\n");
  return 0;
}
