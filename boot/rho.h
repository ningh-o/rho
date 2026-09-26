// rho.h — shared declarations for boot, the seed compiler.
// One shot, arena-allocated, deterministic. C11, no warnings.
#ifndef RHO_H
#define RHO_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- memory

typedef struct Arena Arena;
struct Arena {
  char *base;
  size_t cap, used;
  Arena *next; // chained blocks
};

void *arena_alloc(Arena *a, size_t n, size_t align);
char *aprintf(Arena *a, const char *fmt, ...);
char *astrdup(Arena *a, const char *s);
// A shared program-lifetime arena for pools and interned data.
extern Arena *g_arena;
Arena *arena_new(size_t cap);

// ---------------------------------------------------------------- slices

typedef struct Str { const char *p; size_t n; } Str;
#define STR(lit) ((Str){lit, sizeof(lit) - 1})
bool str_eq(Str a, Str b);
bool str_eq_c(Str a, const char *b);
Str str_slice(const char *p, size_t n);

// Interned strings: unique pointer per spelling.
const char *intern(const char *p, size_t n);
const char *intern_c(const char *s);

// ---------------------------------------------------------------- vec

typedef struct Vec {
  char *data;
  size_t len, cap, elem;
} Vec;

void vec_init(Vec *v, size_t elem);
void *vec_push(Vec *v);
void *vec_at(const Vec *v, size_t i);
static inline size_t vec_len(const Vec *v) { return v->len; }
static inline void vec_clear(Vec *v) { v->len = 0; }

#define VEC(T, name) \
  Vec name;          \
  vec_init(&(name), sizeof(T))
#define VPUSH(name, T) ((T *)vec_push(&(name)))
#define VAT(name, T, i) ((T *)vec_at(&(name), (i)))
#define VLEN(name) vec_len(&(name))

// ---------------------------------------------------------------- diag

typedef enum { DIAG_ERROR, DIAG_NOTE } DiagKind;

typedef struct Diag {
  DiagKind kind;
  const char *file;
  int line, col;
  char *msg;
} Diag;

extern Vec g_diags;
extern bool g_had_error;

void diag_at(DiagKind kind, const char *file, int line, int col,
             const char *fmt, ...);
void diag_gate_set(void); // one-error mode after a structural limit trips
void diag_gate_clear(void); // compile boundary: the gate is not a latch
void diags_print(FILE *out);
size_t diags_count(void);

// ---------------------------------------------------------------- tokens

typedef enum {
  T_EOF = 0,
  T_IDENT, T_INT, T_FLOAT, T_STRING, T_TSTRING,
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
  T_COMMA, T_SEMI, T_COLON, T_DOT, T_ELLIPSIS,
  T_ARROW, T_FATARROW, T_PLUS, T_DASH, T_STAR, T_SLASH, T_PERCENT,
  T_AMP, T_PIPE, T_CARET, T_BANG, T_TILDE, T_QUESTION, T_ANDAND, T_OROR,
  T_DOTDOT,
  T_SHL, T_SHR,
  T_EQ, T_EQEQ, T_NE, T_LT, T_LE, T_GT, T_GE,
  T_PLUSEQ, T_DASHEQ, T_STAREQ, T_SLASHEQ, T_PCTEQ,
  T_AMPEQ, T_PIPEEQ, T_CARETEQ, T_SHLEQ, T_SHREQ,
  K_AS, K_BREAK, K_CONST, K_CONTINUE, K_DEFER, K_DYN, K_ELSE, K_ENUM,
  K_EXTERN, K_FALSE, K_FN, K_FOR, K_IF, K_IMPL, K_LET, K_LOOP, K_MATCH,
  K_MUT, K_NEW, K_NULL, K_PUB, K_RETURN, K_STATIC, K_STRUCT, K_TEST,
  K_TRAIT, K_TRUE, K_USE, K_WHILE,
  K_I8, K_I16, K_I32, K_I64, K_U8, K_U16, K_U32, K_U64, K_USIZE,
  K_F32, K_F64, K_BOOL, K_STRING, K_SELF,
} TokKind;

const char *tok_spell(TokKind k);
bool tok_is_builtin_type(TokKind k, const char **spell);

typedef struct Token {
  TokKind kind;
  const char *file;
  int line, col;
  Str text;    // ident spelling / decoded literal bytes
  uint64_t i;  // integer literal value (non-negative)
  double f;    // float literal value
  char *serr;  // string decode error text, else NULL
} Token;

// comments never become tokens; the lexer records them verbatim so the
// formatter can replay them (fmt may not drop any comment)
typedef struct LexComment {
  const char *text; // from "//" to end of line, no newline, NUL-terminated
  int line, col;
} LexComment;

typedef struct Lexer Lexer;
Lexer *lex_file(Arena *a, const char *path, const char *src);
const Token *lex_tokens(const Lexer *lx, size_t *n);
const LexComment *lex_comments(const Lexer *lx, size_t *n);

// ---------------------------------------------------------------- AST
//
// Positional children (a,b,c,d) + one list per node; leaf payloads in
// ival/fval/bval/sval/op/name/name2. The fmt/dump walker is generic
// over children, so adding a kind never touches the walker.

typedef struct Node Node;
typedef size_t NodeRef; // index into g_nodes; 0 = none
#define NO_REF ((NodeRef)0)

typedef enum {
  // types
  NT_BUILTIN, NT_PTR, NT_OPT, NT_SLICE, NT_FNTYPE, NT_APP, NT_DYN,
  // declarations
  NT_FN, NT_STRUCT, NT_ENUM, NT_TRAIT, NT_IMPL, NT_CONST, NT_STATIC,
  NT_EXTERN, NT_USE, NT_TEST,
  // generic-parameter / field / variant / arm helpers
  NT_GPARAM, NT_FIELD, NT_ENUMVAR, NT_ARM, NT_FIELDINIT, NT_POSARG,
  NT_PARAM, NT_SEG,
  // statements
  NT_LET, NT_ASSIGN, NT_IF, NT_WHILE, NT_LOOP, NT_MATCH, NT_RETURN,
  NT_DEFER, NT_BREAK, NT_CONTINUE, NT_EXPRSTMT,
  // expressions
  NT_INT, NT_FLOAT, NT_BOOL, NT_STR, NT_PATH, NT_CALL, NT_METHOD,
  NT_FIELD_E, NT_INDEX, NT_UNARY, NT_BINARY, NT_AS, NT_NEW,
  NT_SLICE_LIT, NT_SLICE_E, NT_CLOSURE, NT_QMARK, NT_IF_EXPR,
  NT_MATCH_EXPR,
  // patterns
  NT_PLIT, NT_PBIND, NT_PWILD, NT_PVAR, NT_POR,
} NodeKind;

const char *node_kind_name(NodeKind k);

struct Node {
  NodeKind kind;
  const char *file;
  int line, col;
  int end_line; // fmt-only: line of the construct's last token (0 = unstamped)
  void *sem;  // checker-annotated Type* (emit reads it)
  void *sem2; // checker-annotated FnDef* (chosen overload)
  const char *name;  // identifier payload (many kinds)
  const char *name2; // second identifier (method receiver type, alias)
  int op;            // small enum payload (operators, forms, flags)
  uint64_t ival;
  double fval;
  bool bval;
  Str sval;
  NodeRef a, b, c, d;
  struct RefList *list;
};

typedef struct RefList {
  NodeRef *items;
  size_t n, cap;
} RefList;

extern Node *g_nodes;
extern size_t g_nodes_len, g_nodes_cap;

NodeRef node_new(NodeKind kind, const char *file, int line, int col);
Node *node_get(NodeRef r);
RefList *reflist(void);
void reflist_add(RefList *l, NodeRef r);
static inline size_t reflist_len(const RefList *l) { return l->n; }
static inline NodeRef reflist_at(const RefList *l, size_t i) {
  return l->items[i];
}

// op payloads shared across kinds
enum {
  OP_NONE = 0,
  // forms for NT_USE: plain use / pub use segs / pub use item / rename /
  // star
  USE_PLAIN = 0, USE_PUB_MOD = 1, USE_PUB_ITEM = 2, USE_PUB_AS = 3,
  USE_PUB_STAR = 4, USE_BRACE = 5, // use a.{b, c as d} — §4.4 sugar
  USE_DEAD = 8, // or-ed in when a comptime-folded branch kills the use
  // enum variant forms
  VAR_UNIT = 0, VAR_TUPLE = 1, VAR_STRUCT = 2,
  // new forms
  NEW_STRUCT = 0, NEW_VARIANT = 1,
};

// operator payloads for NT_UNARY/NT_BINARY/NT_ASSIGN (op field)
enum {
  OP_NEG = 1, OP_NOT, OP_BITNOT, OP_DEREF,
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
  OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
  OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
  OP_AND, OP_OR,
};
const char *op_spell(int op);

// ---------------------------------------------------------------- modules

typedef struct SymTab SymTab; // sem.h completes it

typedef struct UseBind {
  const char *alias; // binding name in the importing module
  struct Module *target;
  NodeRef decl;      // NT_USE
} UseBind;

typedef struct Module {
  const char *name; // file stem
  const char *path; // as opened
  char *src;
  const Token *toks;
  size_t ntoks;
  const LexComment *cmts; // the file's comments, in source order
  size_t ncmts;
  RefList *decls;
  struct Module *next;
  bool is_package; // loaded via a lib.rho facade
  struct Module *importer; // first module that pulled this in
  SymTab *syms;    // collected symbols (sem.h Sym)
  Vec uses;        // of UseBind — the module's use closure, use order
  bool prepared; // collected + consts resolved at load time
} Module;

Module *module_load(Arena *a, const char *path);       // read + lex + parse
Module *module_parse_src(const char *path, const char *src); // lex + parse

// ---------------------------------------------------------------- program

typedef struct SetOverride {
  const char *name;
  const char *value; // text form, parsed against the const's type
} SetOverride;

typedef struct Program {
  Module *entry;   // the root file
  Module *modules; // every loaded module, load order, linked by ->next
  size_t nmodules;
  Vec sets;        // of SetOverride
} Program;

Program *program_new(void);
bool check_program(Program *p); // full front half: collect + bodies
// Loads the entry module and (transitively) every use-reachable module.
// Reports and returns false on resolution errors.
bool program_load_graph(Program *p, const char *entry_path);

// ---------------------------------------------------------------- dump

void dump_node(FILE *out, Node *n);
void dump_pattern(FILE *out, Node *n);
void dump_module(FILE *out, Module *m);

// ---------------------------------------------------------------- driver

int cmd_build(int argc, char **argv);
int cmd_run(int argc, char **argv);
int cmd_test(int argc, char **argv);
int cmd_fmt(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_selftest(void);
int cmd_dump_ast(const char *path);

#define EXIT_COMPILE 1
#define EXIT_USAGE 2

#endif // RHO_H
