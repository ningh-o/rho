// rho IR — strict SSA over scalar virtual registers, memory (frame slots)
// for everything else. Control flow is structured: reducible CFGs built
// from if/else diamonds, while/loop back edges, and returns, which keeps
// wasm lowering direct. Phis appear exactly where expression values merge.
#ifndef RHO_IR_H
#define RHO_IR_H

#include "rho.h"

typedef enum IRType {
  IT_I8, IT_I16, IT_I32, IT_I64,
  IT_U8, IT_U16, IT_U32, IT_U64, IT_USIZE,
  IT_F32, IT_F64,
  IT_PTR,
} IRType;

typedef struct IRSlot IRSlot;
typedef struct IRVreg IRVreg;
typedef struct IRBlock IRBlock;
typedef struct IRIns IRIns;
typedef struct IRPhi IRPhi;
typedef struct IRFn IRFn;

struct IRSlot {
  int id;
  int64_t size, align;
  const char *name; // debug
};

struct IRVreg {
  int id;
  IRType ty;
  int def_slot; // spill-everything: every vreg gets a frame slot
};

typedef enum IROp {
  IR_CONST, IR_FCONST,
  IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD, IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
  IR_CMP, // integer/pointer compare; cc below
  IR_LOAD,   // dst = [addr]
  IR_STORE,  // [addr] = src
  IR_ADDRC,  // dst = &slot
  IR_ADDI,   // dst = a + imm (address arithmetic)
  IR_CALL,   // direct: symbol | indirect: callee vreg (fn value)
  IR_CAST,   // dst = cast(src)
  IR_COPYMEM,// copy size bytes dst <- src
  IR_ZERO,   // zero size bytes at addr
  IR_LITADDR,// dst = &literal
} IROp;

typedef enum IRCC { CC_EQ, CC_NE, CC_LT, CC_LE, CC_GT, CC_GE } IRCC;

typedef enum IRCastKind {
  CAST_TRUNC, CAST_SEXT, CAST_ZEXT, CAST_I2F, CAST_F2I, CAST_F32_F64,
  CAST_F64_F32, CAST_BITCOPY, // same size+class noop
  CAST_REINTERP,              // f32->u32 / f64->u64 bit pattern move
} IRCastKind;

typedef struct IRArg {
  IRVreg *vreg;
  Type *ty; // for ABI classification
} IRArg;

struct IRIns {
  IROp op;
  IRVreg *dst, *a, *b;
  IRSlot *slot;      // ADDRC
  int64_t imm;       // CONST / ADDI
  double fimm;       // FCONST
  int64_t size;      // LOAD/STORE/COPYMEM/ZERO
  bool is_float;     // arith/cmp/LOAD/STORE operate on floats
  bool signed_ops;   // DIV/MOD/SHR/CMP
  IRCC cc;           // CMP
  IRCastKind cast;   // CAST
  IRType cast_from, cast_to;
  const char *callee;      // CALL direct (rho or extern symbol, target-adjusted)
  IRVreg *callee_vreg;     // CALL indirect (0.0.5 closures)
  Vec args;                // IRArg*
  IRVreg *addr;            // LOAD/STORE/COPYMEM dst/ZERO
  int lit;                 // LITADDR: literal index
  void *join_hint;         // CBR term: the join block, known at lowering
  int line;
};

struct IRPhi {
  IRVreg *dst;
  Vec args; // IRVreg* parallel to preds
  Vec preds; // IRBlock*
};

struct IRBlock {
  int id;
  Vec phis;  // IRPhi*
  Vec ins;   // IRIns*
  IRIns *term;
  bool sealed;
  bool emitted;      // wasm: control-flow re-nesting marks blocks as it goes
  bool loop_header;  // wasm: while/loop headers (set by the lowering)
  void *loop_exit;   // IRBlock* — the break target of that loop
};

typedef struct IRLiteral {
  const char *label;
  Vec bytes; // char
} IRLiteral;

struct IRFn {
  int uid; // emission-unique label prefix
  const char *symbol;
  Type *ret;              // NULL for void-ish (actually TY_VOID)
  bool returns_aggregate; // hidden out param
  Vec params;             // IRSlot* (source order; out slot first when aggregate)
  Vec param_types;        // Type* parallel to params (ABI classification)
  IRSlot *out_slot;
  Vec slots;              // IRSlot* (all frame slots incl. vreg spill slots)
  Vec blocks;             // IRBlock*
  IRBlock *entry, *cur;
  Vec literals;           // IRLiteral
  int next_vreg, next_block, next_slot;
};

// build IR for every checked fn in g_module_order (after check_module)
void lower_program(void);
void lower_set_root(Module *m);

static bool ir_is_float(IRType t) { return t == IT_F32 || t == IT_F64; }
static int64_t ir_size_of(IRType t) {
  switch (t) {
  case IT_I8: case IT_U8: return 1;
  case IT_I16: case IT_U16: return 2;
  case IT_I32: case IT_U32: case IT_F32: return 4;
  default: return 8;
  }
}
static bool ty_is_signed_int(IRType t) {
  switch (t) {
  case IT_I8: case IT_I16: case IT_I32: case IT_I64: return true;
  default: return false;
  }
}

// terminators live outside the IROp enum range to keep switches tight
#define OP_BR 100
#define OP_CBR 101
#define OP_RET 102
extern Vec g_ir_fns; // IRFn* in deterministic order

// emit all fns + globals to assembly text for the target
typedef enum Target {
  TGT_AMD64_LINUX,
  TGT_AMD64_MAC, // Rosetta test vehicle for the SysV backend
  TGT_ARM64_MAC,
  TGT_WASM32_WASI,
  TGT_ESP32C3,   // RV32IM, flat image run by the in-tree simulator
} Target;

void emit_amd64(Target target, SB *out);
void emit_arm64(Target target, SB *out);   // 0.0.3
void emit_wasm(Target target, SB *out);    // 0.0.4
void emit_riscv32(SB *out);                // 0.2.0: esp32c3
int assemble_rv32(const char *text, unsigned char **image_out); // text -> flat image

// statics + string literals collected during lowering
typedef struct IRGlobal {
  const char *symbol;
  int64_t size, align;
  Vec init;        // bytes (char); empty => zero-initialized (.bss)
  char **relocs;   // per-8-byte-word relocation labels (may be NULL)
  bool is_extern;  // nothing to emit
} IRGlobal;
extern Vec g_ir_globals; // IRGlobal*
extern const char *g_main_symbol;

IRFn *ir_fn_for_symbol(const char *symbol);

#endif
