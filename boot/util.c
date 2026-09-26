// util.c — arena, strings, interning, vectors, diagnostics, node pool.
#include "rho.h"

#include <stdarg.h>

Arena *g_arena;

Arena *arena_new(size_t cap) {
  Arena *a = calloc(1, sizeof(Arena));
  a->base = malloc(cap);
  a->cap = cap;
  return a;
}

void *arena_alloc(Arena *a, size_t n, size_t align) {
  assert(align <= 16);
  size_t pad = (align - (a->used & (align - 1))) & (align - 1);
  if (a->used + pad + n > a->cap) {
    // grow by chained blocks; big allocations get their own block
    size_t bc = a->cap * 2;
    if (bc < n + pad + 64)
      bc = n + pad + 64;
    Arena *na = calloc(1, sizeof(Arena));
    na->base = malloc(bc);
    na->cap = bc;
    // chain at the tail so traversal order is creation order
    Arena *t = a;
    while (t->next)
      t = t->next;
    t->next = na;
    // the new block's base is 16-aligned: alignment restarts at zero.
    // Applying the OLD block's pad here handed every chained
    // allocation a (used & (align-1)) skewed address whenever the
    // head happened to fill at a non-aligned offset
    if (n <= na->cap) {
      na->used = n;
      return na->base;
    }
    assert(!"arena block too small");
  }
  void *p = a->base + a->used + pad;
  a->used += pad + n;
  return p;
}

char *aprintf(Arena *a, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  assert(n >= 0);
  char *buf = arena_alloc(a, (size_t)n + 1, 1);
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return buf;
}

char *astrdup(Arena *a, const char *s) {
  size_t n = strlen(s);
  char *p = arena_alloc(a, n + 1, 1);
  memcpy(p, s, n + 1);
  return p;
}

// ---------------------------------------------------------------- str

bool str_eq(Str a, Str b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}
bool str_eq_c(Str a, const char *b) {
  size_t n = strlen(b);
  return a.n == n && (n == 0 || memcmp(a.p, b, n) == 0);
}
Str str_slice(const char *p, size_t n) { return (Str){p, n}; }

// ---------------------------------------------------------------- intern

// Probe table of pointers to arena nodes; nodes carry the creation-
// order chain, so iteration order is deterministic and survives
// rehashing. Interned pointers are stable for the process lifetime.

typedef struct INode {
  const char *s;
  size_t n;
  struct INode *order;
} INode;

static INode **g_ihash;
static size_t g_icap, g_iused;
static INode *g_iorder_head, *g_iorder_tail;

static uint64_t fnv(const char *p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void intern_grow(void) {
  size_t ncap = g_icap ? g_icap * 2 : 4096;
  INode **nt = calloc(ncap, sizeof(INode *));
  for (size_t j = 0; j < g_icap; j++) {
    if (!g_ihash[j])
      continue;
    size_t i = (size_t)(fnv(g_ihash[j]->s, g_ihash[j]->n) & (ncap - 1));
    while (nt[i])
      i = (i + 1) & (ncap - 1);
    nt[i] = g_ihash[j];
  }
  free(g_ihash);
  g_ihash = nt;
  g_icap = ncap;
}

const char *intern(const char *p, size_t n) {
  if (!g_icap || g_iused * 4 >= g_icap * 3)
    intern_grow();
  uint64_t h = fnv(p, n);
  size_t i = (size_t)(h & (g_icap - 1));
  while (g_ihash[i]) {
    if (g_ihash[i]->n == n && memcmp(g_ihash[i]->s, p, n) == 0)
      return g_ihash[i]->s;
    i = (i + 1) & (g_icap - 1);
  }
  char *copy = arena_alloc(g_arena, n + 1, 1);
  memcpy(copy, p, n);
  copy[n] = 0;
  INode *e = arena_alloc(g_arena, sizeof(INode), 8);
  e->s = copy;
  e->n = n;
  g_ihash[i] = e;
  g_iused++;
  if (g_iorder_tail)
    g_iorder_tail->order = e;
  else
    g_iorder_head = e;
  g_iorder_tail = e;
  return copy;
}

const char *intern_c(const char *s) { return intern(s, strlen(s)); }

// ---------------------------------------------------------------- vec

void vec_init(Vec *v, size_t elem) {
  v->data = NULL;
  v->len = v->cap = 0;
  v->elem = elem;
}

void *vec_push(Vec *v) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 16;
    v->data = realloc(v->data, nc * v->elem);
    v->cap = nc;
  }
  void *p = v->data + v->len * v->elem;
  memset(p, 0, v->elem);
  v->len++;
  return p;
}

void *vec_at(const Vec *v, size_t i) {
  assert(i < v->len);
  return v->data + i * v->elem;
}

// ---------------------------------------------------------------- diag

Vec g_diags;
bool g_had_error;

// one-error mode: a hostile input that trips a structural limit (a
// depth bomb, say) would otherwise surface thousands of secondary
// diagnostics from the same root cause. Per-compile, never per-
// process: compile boundaries call diag_gate_clear.
static bool g_diag_gate;

void diag_gate_set(void) { g_diag_gate = true; }

void diag_gate_clear(void) { g_diag_gate = false; }

void diag_at(DiagKind kind, const char *file, int line, int col,
             const char *fmt, ...) {
  if (g_diag_gate) {
    if (kind == DIAG_ERROR)
      g_had_error = true;
    return;
  }
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  assert(n >= 0);
  Diag *d = vec_push(&g_diags);
  d->kind = kind;
  d->file = file;
  d->line = line;
  d->col = col;
  d->msg = malloc((size_t)n + 1);
  vsnprintf(d->msg, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  if (kind == DIAG_ERROR)
    g_had_error = true;
}

void diags_print(FILE *out) {
  for (size_t i = 0; i < vec_len(&g_diags); i++) {
    Diag *d = VAT(g_diags, Diag, i);
    fprintf(out, "%s:%d:%d: %s: %s\n", d->file, d->line, d->col,
            d->kind == DIAG_ERROR ? "error" : "note", d->msg);
  }
}

size_t diags_count(void) { return vec_len(&g_diags); }

// ---------------------------------------------------------------- nodes

Node *g_nodes;
size_t g_nodes_len, g_nodes_cap;

// Node storage is CHUNKED and never moves: the checker and emitter
// hand out Node* pointers that must stay valid across later node
// creation (generic instantiation clones whole trees while its
// callers still hold pointers into the originals).
#define NODE_CHUNK 8192
static Node **g_chunk_ptrs; // chunk bases (the array may move; the
                            // chunks themselves never do)
static size_t g_nchunks, g_chunks_cap;

static Node *chunk_for(size_t idx) {
  return g_chunk_ptrs[idx / NODE_CHUNK];
}

NodeRef node_new(NodeKind kind, const char *file, int line, int col) {
  if (g_nchunks == 0) {
    g_chunks_cap = 8;
    g_chunk_ptrs = malloc(g_chunks_cap * sizeof(Node *));
    Node *c0 = malloc(NODE_CHUNK * sizeof(Node));
    memset(c0, 0, NODE_CHUNK * sizeof(Node));
    g_chunk_ptrs[0] = c0;
    g_nchunks = 1;
    // index 0 is NO_REF; park an inert node there forever
    c0[0].a = c0[0].b = c0[0].c = c0[0].d = NO_REF;
    g_nodes_len = 1;
    g_nodes = c0; // compatibility for direct-base readers
  }
  if (g_nodes_len >= g_nchunks * NODE_CHUNK) {
    if (g_nchunks == g_chunks_cap) {
      g_chunks_cap *= 2;
      g_chunk_ptrs = realloc(g_chunk_ptrs, g_chunks_cap * sizeof(Node *));
    }
    Node *nc = malloc(NODE_CHUNK * sizeof(Node));
    memset(nc, 0, NODE_CHUNK * sizeof(Node));
    g_chunk_ptrs[g_nchunks++] = nc;
  }
  Node *n = &chunk_for(g_nodes_len)[g_nodes_len % NODE_CHUNK];
  NodeRef idx = (NodeRef)g_nodes_len;
  g_nodes_len++;
  memset(n, 0, sizeof(Node));
  n->kind = kind;
  n->file = file;
  n->line = line;
  n->col = col;
  n->a = n->b = n->c = n->d = NO_REF;
  return idx;
}

Node *node_get(NodeRef r) {
  assert(r != NO_REF && r < g_nodes_len);
  return &chunk_for(r)[r % NODE_CHUNK];
}

RefList *reflist(void) {
  RefList *l = arena_alloc(g_arena, sizeof(RefList), 8);
  l->items = NULL;
  l->n = l->cap = 0;
  return l;
}

void reflist_add(RefList *l, NodeRef r) {
  if (l->n == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 8;
    NodeRef *ni = arena_alloc(g_arena, nc * sizeof(NodeRef), 8);
    if (l->items)
      memcpy(ni, l->items, l->n * sizeof(NodeRef));
    l->items = ni;
    l->cap = nc;
  }
  l->items[l->n++] = r;
}

// ---------------------------------------------------------------- names

static const struct { NodeKind k; const char *s; } k_node_names[] = {
    {NT_BUILTIN, "builtin-type"},   {NT_PTR, "ptr"},
    {NT_OPT, "opt"},                {NT_SLICE, "slice"},
    {NT_FNTYPE, "fn-type"},         {NT_APP, "generic-app"},
    {NT_FN, "fn"},                  {NT_STRUCT, "struct"},
    {NT_ENUM, "enum"},              {NT_TRAIT, "trait"},
    {NT_IMPL, "impl"},              {NT_CONST, "const"},
    {NT_STATIC, "static"},          {NT_EXTERN, "extern"},
    {NT_USE, "use"},                {NT_TEST, "test"},
    {NT_POR, "or-pattern"},                {NT_GPARAM, "generic-param"},
    {NT_FIELD, "field"},            {NT_ENUMVAR, "variant"},
    {NT_ARM, "arm"},                {NT_FIELDINIT, "field-init"},
    {NT_POSARG, "pos-arg"},         {NT_PARAM, "param"},
    {NT_SEG, "segment"},            {NT_LET, "let"},
    {NT_ASSIGN, "assign"},          {NT_IF, "if"},
    {NT_WHILE, "while"},            {NT_LOOP, "loop"},
    {NT_MATCH, "match"},            {NT_RETURN, "return"},
    {NT_DEFER, "defer"},            {NT_BREAK, "break"},
    {NT_CONTINUE, "continue"},      {NT_EXPRSTMT, "expr-stmt"},
    {NT_INT, "int"},                {NT_FLOAT, "float"},
    {NT_BOOL, "bool"},              {NT_STR, "string"},
    {NT_PATH, "path"},              {NT_CALL, "call"},
    {NT_METHOD, "method"},          {NT_FIELD_E, "field-access"},
    {NT_INDEX, "index"},            {NT_UNARY, "unary"},
    {NT_BINARY, "binary"},          {NT_AS, "as"},
    {NT_NEW, "new"},                {NT_SLICE_LIT, "slice-lit"},
    {NT_SLICE_E, "slice-expr"},     {NT_DYN, "dyn"},
    {NT_CLOSURE, "closure"},        {NT_QMARK, "qmark"},
    {NT_IF_EXPR, "if-expr"},        {NT_MATCH_EXPR, "match-expr"},
    {NT_PLIT, "pattern-lit"},       {NT_PBIND, "pattern-bind"},
    {NT_PWILD, "pattern-wild"},     {NT_PVAR, "pattern-variant"},
};

const char *node_kind_name(NodeKind k) {
  for (size_t i = 0; i < sizeof(k_node_names) / sizeof(k_node_names[0]);
       i++)
    if (k_node_names[i].k == k)
      return k_node_names[i].s;
  return "?";
}

static const struct { int op; const char *s; } k_op_names[] = {
    {OP_NEG, "-"},   {OP_NOT, "!"},
    {OP_BITNOT, "~"},
    {OP_DEREF, "*"},
    {OP_ADD, "+"},   {OP_SUB, "-"},
    {OP_MUL, "*"},   {OP_DIV, "/"},
    {OP_MOD, "%"},   {OP_BAND, "&"},
    {OP_BOR, "|"},   {OP_BXOR, "^"},
    {OP_SHL, "<<"},  {OP_SHR, ">>"},
    {OP_EQ, "=="},   {OP_NE, "!="},
    {OP_LT, "<"},    {OP_LE, "<="},
    {OP_GT, ">"},    {OP_GE, ">="},
    {OP_AND, "&&"},  {OP_OR, "||"},
};

const char *op_spell(int op) {
  for (size_t i = 0; i < sizeof(k_op_names) / sizeof(k_op_names[0]); i++)
    if (k_op_names[i].op == op)
      return k_op_names[i].s;
  return "?";
}

static const struct { TokKind k; const char *s; } k_tok_names[] = {
    {T_EOF, "end of file"},
    {T_IDENT, "identifier"},   {T_INT, "integer"},
    {T_FLOAT, "float"},        {T_STRING, "string"},
    {T_TSTRING, "string"},     {T_LPAREN, "'('"},
    {T_RPAREN, "')'"},         {T_LBRACE, "'{'"},
    {T_RBRACE, "'}'"},         {T_LBRACK, "'['"},
    {T_RBRACK, "']'"},         {T_COMMA, "','"},
    {T_SEMI, "';'"},           {T_COLON, "':'"},
    {T_DOT, "'.'"},            {T_ELLIPSIS, "'...'"},
    {T_ARROW, "'->'"},         {T_FATARROW, "'=>'"},
    {T_PLUS, "'+'"},           {T_DASH, "'-'"},
    {T_STAR, "'*'"},           {T_SLASH, "'/'"},
    {T_PERCENT, "'%'"},        {T_AMP, "'&'"},
    {T_PIPE, "'|'"},           {T_CARET, "'^'"},
    {T_BANG, "'!'"},           {T_QUESTION, "'?'"},
    {T_ANDAND, "'&&'"},        {T_OROR, "'||'"},
    {T_SHL, "'<<'"},           {T_SHR, "'>>'"},
    {T_EQ, "'='"},             {T_EQEQ, "'=='"},
    {T_NE, "'!='"},            {T_LT, "'<'"},
    {T_LE, "'<='"},            {T_GT, "'>'"},
    {T_GE, "'>='"},            {T_PLUSEQ, "'+='"},
    {T_DASHEQ, "'-='"},        {T_STAREQ, "'*='"},
    {T_SLASHEQ, "'/='"},       {T_PCTEQ, "'%='"},
    {T_AMPEQ, "'&='"},         {T_PIPEEQ, "'|='"},
    {T_CARETEQ, "'^='"},       {T_SHLEQ, "'<<='"},
    {T_SHREQ, "'>>='"},        {K_AS, "'as'"},
    {K_BREAK, "'break'"},      {K_CONST, "'const'"},
    {K_CONTINUE, "'continue'"}, {K_DEFER, "'defer'"},
    {K_DYN, "'dyn'"},          {K_ELSE, "'else'"},
    {K_ENUM, "'enum'"},        {K_EXTERN, "'extern'"},
    {K_FALSE, "'false'"},      {K_FN, "'fn'"},
    {K_FOR, "'for'"},          {K_IF, "'if'"},
    {K_IMPL, "'impl'"},        {K_LET, "'let'"},
    {K_LOOP, "'loop'"},        {K_MATCH, "'match'"},
    {K_MUT, "'mut'"},          {K_NEW, "'new'"},
    {K_NULL, "'null'"},        {K_PUB, "'pub'"},
    {K_RETURN, "'return'"},    {K_STATIC, "'static'"},
    {K_STRUCT, "'struct'"},    {K_TEST, "'test'"},
    {K_TRAIT, "'trait'"},      {K_TRUE, "'true'"},
    {K_USE, "'use'"},
    {K_WHILE, "'while'"},      {K_I8, "'i8'"},
    {K_I16, "'i16'"},          {K_I32, "'i32'"},
    {K_I64, "'i64'"},          {K_U8, "'u8'"},
    {K_U16, "'u16'"},          {K_U32, "'u32'"},
    {K_U64, "'u64'"},          {K_USIZE, "'usize'"},
    {K_F32, "'f32'"},          {K_F64, "'f64'"},
    {K_BOOL, "'bool'"},        {K_STRING, "'string'"},
};

const char *tok_spell(TokKind k) {
  for (size_t i = 0; i < sizeof(k_tok_names) / sizeof(k_tok_names[0]);
       i++)
    if (k_tok_names[i].k == k)
      return k_tok_names[i].s;
  return "?";
}

bool tok_is_builtin_type(TokKind k, const char **spell) {
  static const char *spells[] = {"i8",  "i16", "i32", "i64",  "u8",
                                 "u16", "u32", "u64", "usize", "f32",
                                 "f64", "bool", "string"};
  if (k >= K_I8 && k <= K_STRING) {
    *spell = spells[k - K_I8];
    return true;
  }
  return false;
}


// deep clone of an AST subtree: fresh nodes, fresh reflists; sem fields
// start clean (per-instance annotation)
static NodeRef clone_ref(NodeRef r, RefList *owned) {
  if (r == NO_REF)
    return NO_REF;
  Node *n = node_get(r);
  NodeRef c = node_new(n->kind, n->file, n->line, n->col);
  Node *m = node_get(c);
  m->name = n->name;
  m->name2 = n->name2;
  m->op = n->op;
  m->ival = n->ival;
  m->fval = n->fval;
  m->bval = n->bval;
  m->sval = n->sval;
  m->a = clone_ref(n->a, owned);
  m->b = clone_ref(n->b, owned);
  m->c = clone_ref(n->c, owned);
  m->d = clone_ref(n->d, owned);
  if (n->list) {
    RefList *nl = reflist();
    for (size_t i = 0; i < reflist_len(n->list); i++)
      reflist_add(nl, clone_ref(reflist_at(n->list, i), owned));
    m->list = nl;
  }
  return c;
}

NodeRef clone_node_tree(NodeRef r) {
  return clone_ref(r, NULL);
}
