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
  if (g_fail) {
    fprintf(stderr, "selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  printf("selftest: ok\n");
  return 0;
}
