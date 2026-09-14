// rho boot compiler — common definitions.
//
// Everything lives in one arena that is never freed: the compiler is a
// one-shot process, and the self-hosted port keeps the same shape.
#ifndef RHO_H
#define RHO_H

#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- arena ---

typedef struct Arena {
  char *at;
  size_t left;
  struct Arena *next;
} Arena;

void *arena_alloc(size_t n);
void *arena_alloc_zeroed(size_t n);
char *arena_strndup(const char *s, size_t n);
char *arena_strdup(const char *s);
char *arena_printf(const char *fmt, ...);
char *arena_printf_(const char *fmt, va_list ap);

// --------------------------------------------------------------- strings ---

typedef struct Str {
  const char *p;
  size_t n;
} Str;

Str str_from(const char *c);
Str str_from_len(const char *c, size_t n);
bool str_eq(Str a, Str b);
bool str_eq_c(Str a, const char *b);
Str str_slice(Str s, size_t lo, size_t hi);
char *str_to_c(Str s); // arena copy, NUL-terminated

// String builder.
typedef struct SB {
  char *buf;
  size_t n, cap;
} SB;

void sb_grow(SB *sb, size_t extra);
void sb_push(SB *sb, char c);
void sb_append(SB *sb, Str s);
void sb_append_c(SB *sb, const char *s);
void sb_printf(SB *sb, const char *fmt, ...);
Str sb_finish(SB *sb);

// ------------------------------------------------------------------ vec ---

// A growable array of pointers. Every AST node list and symbol table order
// uses this, so iteration order is insertion order — deterministic by
// construction.
typedef struct Vec {
  void **items;
  size_t n, cap;
} Vec;

void vec_push(Vec *v, void *p);
void *vec_pop(Vec *v);
void *vec_last(Vec *v);

// ------------------------------------------------------------------ map ---

// Open-addressing string map with insertion-order iteration. No deletion;
// compile-time maps never shrink.
typedef struct Map {
  struct MapEnt *slots; // power of two
  size_t cap, n;
  Vec keys; // Str* in insertion order
} Map;

size_t map_hash(Str s);
void *map_get(const Map *m, Str key);
void map_put(Map *m, Str key, void *val); // replaces existing
bool map_has(const Map *m, Str key);

// ------------------------------------------------------------------ diag ---

typedef struct Diag {
  Str file;
  int line, col;
  char *msg;
} Diag;

extern Vec diags;      // Diag*
extern bool any_error;

void err_at(Str file, int line, int col, const char *fmt, ...);
void err_range(Str file, int line, int col, int len, const char *fmt, ...);
void render_diags(SB *sb);
void flush_diags(void);
void clear_diags(void);
int diag_count(void);

// --------------------------------------------------------------- tokens ---

typedef enum Tok {
  TK_EOF = 0,
  TK_IDENT,
  TK_INT,
  TK_FLOAT,
  TK_STR,
  // keywords
  KW_FN, KW_LET, KW_MUT, KW_IF, KW_ELSE, KW_WHILE, KW_LOOP, KW_BREAK,
  KW_CONTINUE, KW_RETURN, KW_DEFER, KW_STRUCT, KW_ENUM, KW_USE, KW_PUB,
  KW_STATIC, KW_CONST, KW_MATCH, KW_AS, KW_NEW, KW_NULL, KW_TRUE, KW_FALSE,
  KW_WEAK, KW_SELF, KW_EXTERN,
  // punctuation & operators
  P_LPAREN, P_RPAREN, P_LBRACE, P_RBRACE, P_LBRACKET, P_RBRACKET,
  P_COMMA, P_COLON, P_SEMI, P_DOT, P_ARROW, P_FATARROW, P_ELLIPSIS2,
  P_PLUS, P_MINUS, P_STAR, P_SLASH, P_PERCENT, P_BANG, P_TILDE, P_AMP,
  P_PIPE, P_CARET, P_SHL, P_SHR, P_ANDAND, P_OROR, P_EQ, P_NE, P_LT, P_GT,
  P_LE, P_GE, P_ASSIGN, P_PLUSEQ, P_MINUSEQ, P_STAREQ, P_SLASHEQ,
  P_PERCENTEQ, P_AMPEQ, P_PIPEEQ, P_CARETEQ, P_SHLEQ, P_SHREQ, P_QMARK,
} Tok;

typedef struct Token {
  Tok kind;
  Str text;   // identifier text / literal raw
  uint64_t iv;
  double fv;
  Str file;
  int line, col;
} Token;

void lex_file(Str file, Str src, Vec *out_tokens);

const char *tok_name(Tok t);
const char *tok_spell(Tok t); // plain operator spelling for dumps/fmt

// ------------------------------------------------------------------ AST ---

// Primitive type indices — the order is the canonical spelling table too.
enum PrimKind {
  PRIM_BOOL, PRIM_I8, PRIM_I16, PRIM_I32, PRIM_I64, PRIM_U8, PRIM_U16,
  PRIM_U32, PRIM_U64, PRIM_F32, PRIM_F64, PRIM_STRING, PRIM_USIZE,
  PRIM_ISIZE, PRIM_VOID,
};
#define PRIM_COUNT 15
extern const char *PRIM_NAMES[PRIM_COUNT];

typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct TypeAst TypeAst;
typedef struct Decl Decl;
typedef struct Param Param;
typedef struct FieldAst FieldAst;
typedef struct VariantAst VariantAst;
typedef struct MatchArm MatchArm;

// ---- type syntax
typedef enum TypeAstKind {
  TA_PRIM,   // prim
  TA_ARRAY,  // [N]elem  (size may be NULL for slice: [])
  TA_PTR,    // *elem
  TA_WEAK,   // weak[elem]
  TA_NAMED,  // path.Name[targs...]
  TA_FN,     // fn(params) -> ret
  TA_INFER,  // omitted annotation
} TypeAstKind;

typedef struct TypeAst {
  TypeAstKind kind;
  int prim;          // TA_PRIM: PrimKind
  TypeAst *elem;     // ARRAY/PTR/WEAK
  Expr *size;        // ARRAY (NULL => slice)
  Vec path;          // TA_NAMED: char* segments
  Vec targs;         // TA_NAMED: TypeAst*
  Vec params;        // TA_FN: TypeAst*
  TypeAst *ret;      // TA_FN
  Str file;
  int line, col;
} TypeAst;

// ---- expressions
typedef enum ExprKind {
  EX_INT, EX_FLOAT, EX_STR, EX_BOOL, EX_NULL, EX_NAME, EX_TYPE,
  EX_BIN, EX_UN, EX_CALL, EX_INDEX, EX_SLICE, EX_FIELD, EX_METHOD,
  EX_CAST, EX_NEW, EX_MAKE, EX_CLOSURE, EX_MATCH, EX_QMARK, EX_ENUM_CTOR,
  EX_IF, EX_BLOCK,
} ExprKind;

typedef struct MatchArm {
  // pattern
  enum { PAT_WILDCARD, PAT_INT, PAT_STR, PAT_UNIT, PAT_TUPLE, PAT_STRUCT } pk;
  Vec pat_path;   // TUPLE/STRUCT: char* segments (usually 1)
  uint64_t pat_int;
  int variant_index; // UNIT on enum: filled by checker
  uint64_t disc;     // UNIT on enum: variant discriminant, filled by checker
  Str pat_str;
  Vec pat_names;  // TUPLE: char* bindings ("_" = ignore)
  Vec pat_fields; // STRUCT: FieldAst* (name + optional binding name)
  Vec bind_syms;  // checker: Sym* payload bindings, parallel to pat_names /
                  // pat_fields (NULL entry = `_`)
  Vec bind_fidx;  // checker: payload field index per binding (as long)
  Expr *body;     // expression or block
  Str file;
  int line, col;
} MatchArm;

typedef struct Expr {
  ExprKind kind;
  Str file;
  int line, col;

  uint64_t iv;     // INT
  double fv;       // FLOAT
  Str sv;          // STR, NAME
  bool bv;         // BOOL

  Expr *a, *b;     // BIN: l, r; UN: operand/a; CALL: callee; INDEX/SLICE/
                   // FIELD/METHOD/QMARK/CAST: base; NEW: NULL
  Expr *c;         // BIN op uses binop; UN uses unop; SLICE: hi;
                   // MAKE: len; IF: cond; CAST: unused
  int binop, unop; // Tok
  Vec args;        // CALL/METHOD/NEW(fields as FieldAst*)/ENUM_CTOR
  Vec arg_names;   // CALL: char* parallel to args; NULL entry = positional
  Vec items;       // BLOCK: Stmt*; IF: then/else; NEW: FieldAst* names
  TypeAst *ty;     // CAST target / MAKE elem / NEW type
  Vec arms;        // MATCH: MatchArm*
  Vec params;      // CLOSURE: Param*
  TypeAst *ret;    // CLOSURE

  // filled by checker
  void *typed;     // Type* — computed type of this expression
  void *sym;       // NAME: Sym* resolution
} Expr;

// ---- statements
typedef enum StmtKind {
  ST_LET, ST_ASSIGN, ST_EXPR, ST_RETURN, ST_BREAK, ST_CONTINUE, ST_DEFER,
  ST_WHILE, ST_LOOP, ST_BLOCK,
} StmtKind;

typedef struct Stmt {
  StmtKind kind;
  Str file;
  int line, col;

  bool mut;          // LET
  bool tail;         // EXPR: trailing expression (the block's value)
  Str name;          // LET
  TypeAst *ty;       // LET annotation (may be NULL)
  Expr *a, *b;       // LET: init=a; ASSIGN: target=a, value=b; EXPR/RETURN: a
  int assign_op;     // ASSIGN: Tok
  Vec stmts;         // DEFER: wrapped stmts; BLOCK
  Expr *cond;        // WHILE
  Vec body;          // WHILE/LOOP: Stmt*
} Stmt;

// ---- declarations
typedef struct Param {
  Str name;
  TypeAst *ty;
  bool is_self;
  Str file;
  int line, col;
} Param;

typedef struct FieldAst {
  Str name;
  TypeAst *ty;
  Str file;
  int line, col;
} FieldAst;

typedef struct VariantAst {
  Str name;
  enum { VAR_UNIT, VAR_TUPLE, VAR_STRUCT } vkind;
  Vec types;   // TUPLE: TypeAst*
  Vec fields;  // STRUCT: FieldAst*
  bool has_disc;
  uint64_t disc;
  Str file;
  int line, col;
} VariantAst;

typedef struct Decl {
  enum { DK_FN, DK_STRUCT, DK_ENUM, DK_STATIC, DK_CONST, DK_USE, DK_EXTERN } kind;
  Str file;
  int line, col;

  bool pub_, is_mut;
  Str name;          // FN/STRUCT/ENUM/STATIC/CONST
  Vec path;          // USE: char* segments
  Vec params;        // FN/EXTERN: Param*
  TypeAst *ret;      // FN/EXTERN (NULL => void)
  Vec fields;        // STRUCT: FieldAst*
  Vec variants;      // ENUM: VariantAst*
  Expr *init;        // STATIC/CONST
  Vec body;          // FN: Stmt*
  Vec tparams;       // FN/STRUCT/ENUM: char* type parameter names
  bool is_method;    // FN: first param named self
  Str recv;          // FN method: receiver type name (`fn Point.sum`)
  Vec decls;         // module root only: Decl*
  Map *symbols;      // checker: module-level symbols
  void *ceval_cache; // const-eval memo (checker-owned struct)
  bool ceval_cache_ok;
  void *tenv;        // checker: Map* name->Type* env this decl resolves under
  bool templated;    // checker: signature mentions a type parameter (never lowered)
  void *templ;       // checker: RecType* of the generic definition (struct/enum)
} Decl;

Decl *parse_file(Str path, Str src); // full module: parse + check happens later
Str dump_module(Decl *module);       // canonical s-expression dump

// ---------------------------------------------------------------- check ---

void check_reset(void);
// Runs the resolver/typechecker over a parsed module tree (module itself
// first, then its `use` imports, transitively). Fills typed/sym annotations.
// Returns the number of errors reported (also appended to `diags`).
int check_module(Decl *module);

// --------------------------------------------------------- type model -----
// Shared with the lowering and codegen phases.

typedef enum TypeKind {
  TY_VOID, TY_BOOL, TY_I8, TY_I16, TY_I32, TY_I64, TY_U8, TY_U16, TY_U32,
  TY_U64, TY_F32, TY_F64, TY_STRING, TY_USIZE, TY_ISIZE,
  TY_INT_LIT, TY_FLOAT_LIT, TY_NULL,
  TY_ARRAY, TY_SLICE, TY_PTR, TY_WEAK, TY_FN, TY_STRUCT, TY_ENUM, TY_ERR,
  TY_MODULE, TY_PARAM,
} TypeKind;

typedef struct RecType RecType;
typedef struct Type Type;

struct RecType {
  Decl *decl;
  void *owner;      // Module*
  Vec targs;        // generic instantiation: Type* per tparam (TY_PARAM on the template)
  Str mangled;
  void *env;        // Map* tparam name -> Type* (generic instantiations/templates)
  Vec field_types;  // Type*, parallel to decl->fields
  bool fields_done;
  bool resolving;
  bool is_template; // the unsubstituted generic definition
  Vec methods;      // Sym*
  Vec offsets;      // int64_t field offsets (structs), stored as long
  Vec var_poff;     // ENUM: int64_t payload base offset per variant (as long)
  Vec var_offsets;  // ENUM: Vec* of int64_t payload field offsets per variant
  Vec var_types;    // ENUM: Vec* of Type* payload field types per variant
  int64_t size;     // layout, computed after check
  int64_t align;
};

struct Type {
  TypeKind kind;
  Type *elem;   // ARRAY/SLICE/PTR/WEAK
  uint64_t len; // ARRAY
  RecType *rec; // STRUCT/ENUM
  Vec params;   // FN: Type*
  Type *ret;    // FN
  const char *mangled;
};

typedef enum SymKind {
  SY_LOCAL, SY_PARAM, SY_FN, SY_STRUCT, SY_ENUM, SY_STATIC, SY_CONST,
  SY_MODULE, SY_EXTERN, SY_VARIANT,
} SymKind;

typedef struct Sym {
  SymKind kind;
  Str name;
  Type *type;
  Decl *decl;
  void *owner;       // Module*
  bool mutable;
  void *module;      // SY_MODULE: Module*
  int variant_index; // SY_VARIANT
  int local_id;      // slot index within function (params first)
  const char *symbol; // codegen symbol (fns/externs), set after check
} Sym;

typedef struct Module {
  Str path;
  Str ns;
  Decl *root;
  Map syms;
  bool is_prelude;
  bool checked;
} Module;

typedef struct CV {
  bool ok, is_int;
  uint64_t i;
  double f;
  Str s;
  bool b;
} CV;

bool ty_is_int(Type *t);
bool ty_is_aggregate_t(Type *t);
bool ty_is_signed(Type *t);
bool ty_is_managed(Type *t);

// layout (valid after check_module)
int64_t type_size(Type *t);
int64_t type_align(Type *t);
// enum variant payload offset (after the tag, per-variant aligned)
int64_t variant_payload_offset(Type *enum_t, int variant);
// payload field offset within a variant (relative to the enum value start)
int64_t variant_field_offset(Type *enum_t, int variant, int field);
// field offsets; parallel to rec->decl->fields
int64_t struct_field_offset(RecType *rec, size_t i);

// module registry (filled by check_module)
extern Vec g_module_order; // Module*
Module *g_prelude_module(void);
// symbol name used in assembly for a fn/extern
const char *sym_symbol(Sym *s);
const char *prelude_symbol(const char *name);

// ------------------------------------------------------------------ fmt ---

Str fmt_module(Decl *module);

// ----------------------------------------------------------------- main ---

extern const char *g_root_dir; // directory of the root source file
void prelude_init(void);       // parse + register prelude modules

#endif
