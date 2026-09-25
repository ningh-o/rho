// sem.h — semantic types and symbols for the checker and emitter.
#ifndef RHO_SEM_H
#define RHO_SEM_H

#include "rho.h"
#include "cval.h"

// ---------------------------------------------------------------- types

typedef enum {
  TY_I8, TY_I16, TY_I32, TY_I64,
  TY_U8, TY_U16, TY_U32, TY_U64, TY_USIZE,
  TY_F32, TY_F64, TY_BOOL, TY_STRING,
  TY_UNIT,   // the absent return type
  TY_PTR,    // base = pointee
  TY_SLICE,  // base = element
  TY_DYN,    // tdef = trait
  TY_FN,     // fn_params/fn_ret
  TY_STRUCT, // sdef + args
  TY_ENUM,   // edef + args
  TY_PARAM,  // generic parameter (by name, per generic scope)
} TyKind;

typedef struct Type Type;
typedef struct StructDef StructDef;
typedef struct EnumDef EnumDef;
typedef struct EnumVariant EnumVariant;
typedef struct TraitDef TraitDef;
typedef struct FieldDef FieldDef;
typedef struct FnSig FnSig;

struct Type {
  TyKind kind;
  Type *base;        // ptr/slice element
  Type **args;       // generic instantiation arguments
  size_t nargs;
  StructDef *sdef;
  EnumDef *edef;
  TraitDef *tdef;
  const char *pname; // TY_PARAM name
  FnSig *sig;        // TY_FN
  bool is_opt;       // syntactic ?T sugar provenance (diagnostics only)
};

typedef struct FieldDef {
  const char *name;
  Type *ty;
  NodeRef decl; // NT_FIELD
} FieldDef;

typedef struct ParamDef {
  const char *name;
  Type *ty;
  bool variadic;
  NodeRef decl;
} ParamDef;

struct FnSig {
  ParamDef *params;
  size_t nparams;
  Type *ret;
};

struct StructDef {
  const char *name;
  FieldDef *fields;
  size_t nfields;
  const char **gparams; // generic parameter names
  size_t ngparams;
  NodeRef decl;
  struct Module *mod; // defining module
  Type *self;         // cached TY_PARAM-based self type template
};

struct EnumVariant {
  const char *name;
  int form; // VAR_UNIT/VAR_TUPLE/VAR_STRUCT
  FieldDef *fields; // payload fields (tuple: positional names "0","1",…)
  size_t nfields;
  int tag;
  NodeRef decl;
};

struct EnumDef {
  const char *name;
  EnumVariant *variants;
  size_t nvariants;
  const char **gparams;
  size_t ngparams;
  NodeRef decl;
  struct Module *mod;
  bool is_option; // the prelude Option
  bool is_result; // the prelude Result
};

struct TraitDef {
  const char *name;
  FnSig *sigs;
  size_t nsigs;
  NodeRef decl;
  struct Module *mod;
};

extern Type *ty_i8, *ty_i16, *ty_i32, *ty_i64, *ty_u8, *ty_u16, *ty_u32,
    *ty_u64, *ty_usize, *ty_f32, *ty_f64, *ty_bool, *ty_string, *ty_unit;

Type *type_ptr(Type *elem);
Type *type_slice(Type *elem);
Type *type_dyn(TraitDef *t);
Type *type_fn(FnSig *sig);
Type *type_struct(StructDef *sd, Type **args, size_t nargs);
Type *type_enum(EnumDef *ed, Type **args, size_t nargs);
Type *type_param(const char *name);
Type *type_result_ok(Type *t);     // Result[t, ?]
Type *type_option_of(Type *t);     // Option[t]

bool type_eq(Type *a, Type *b);    // structural identity
const char *type_name(Type *t);    // deterministic spelling (arena)

bool type_is_int(Type *t);
bool type_is_float(Type *t);
bool type_is_num(Type *t);
bool type_is_managed(Type *t);
bool sig_same(FnSig *a, FnSig *b);

// generic-parameter scope (name list with parent chain)
typedef struct GScope {
  const char **names;
  size_t n;
  struct GScope *up;
} GScope;

// set by check_program
extern Module *g_prelude_mod; // the <prelude> module
extern Module *g_entry_mod;   // the entry module

Type *resolve_type_pub(Module *m, NodeRef tr, GScope *g);
Type *tsubst(Type *t, void *b); // TBind is checker-internal
void for_each_live_use(Module *m, bool (*cb)(Module *, NodeRef));
void const_resolve_module_pub(Module *m);
void prune_dead_uses(Module *m);

// ---------------------------------------------------------------- symbols

typedef enum {
  SYM_FN,       // name → overload set (FnDef list)
  SYM_STRUCT,
  SYM_ENUM,
  SYM_TRAIT,
  SYM_CONST,
  SYM_STATIC,
  SYM_EXTERN,
  SYM_MODULE,   // a use binding (module or facade)
} SymKind;

typedef struct FnDef {
  const char *name;
  FnSig *sig;
  NodeRef body; // NO_REF for trait sigs
  struct Module *mod;
  NodeRef decl;
  bool is_pub;
  bool is_method;      // fn T.name with self receiver
  bool is_assoc;       // fn T.name without self
  const char *recv;    // receiver type spelling (method/assoc)
  const char **gparams;
  size_t ngparams;
  struct FnDef *next_overload; // same-name chain
  struct FnDef *instances;     // monomorphized instances (chain)
  struct FnDef *next_instance;
  Type **ibinds;               // instance: gparam → concrete type
} FnDef;

typedef struct ConstDef {
  const char *name;
  Type *ty;
  NodeRef init;
  NodeRef decl;
  struct Module *mod;
  bool is_root;    // root-file const (build parameter, prelude status)
  bool overridden; // --set replaced the value
  void *cval;      // resolved comptime value (check3's CVal), or NULL
} ConstDef;

typedef struct Sym {
  SymKind kind;
  const char *name;
  bool pub;
  union {
    FnDef *fns;        // SYM_FN (overload chain)
    StructDef *sdef;
    EnumDef *edef;
    TraitDef *tdef;
    ConstDef *konst;
    struct Module *module;
  } u;
  struct Sym *next;       // hash chain
  struct Sym *order_next; // creation order
} Sym;

// Module symbol table: open hash over name → Sym, creation order chain.
typedef struct SymTab {
  Sym **slots;
  size_t cap, count;
  Sym *order_head, *order_tail;
} SymTab;

Sym *symtab_get(SymTab *st, const char *name);
Sym *symtab_add(SymTab *st, const char *name); // fails loudly on dup kind clash
size_t symtab_count(SymTab *st);

// ---------------------------------------------------------------- program

typedef struct Program Program;

// ---------------------------------------------------------------- checking

#endif // RHO_SEM_H