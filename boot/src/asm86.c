// The in-tree amd64 assembler for rho's own assembly format — the AT&T
// dialect emit_amd64.c prints (and the amd64-linux runtime blob). A
// straight-line parser (directives, labels, the instruction forms the
// backend emits) plus a fixup pass (rel32 branches/calls, RIP-relative
// lea/mov, absolute .quad relocations against the section symbol table)
// produces raw section images (text with rodata folded in / data) that
// elf64.c wraps into a static executable. Nothing here shells out to an
// external assembler or linker.
//
// Layout contract shared with asm64.c and elf64.c: text | page | data |
// heap, one layout, one truth. The runtime blob is prepended to the text
// image, so the entry point is the first text byte.
//
// Special undefined symbols, bound by the image builder:
//   _rho_rt_heap — the first byte of the zero-fill heap after __data

#include "ir.h"

static int hexv(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

enum { SEC_TEXT, SEC_STR, SEC_DATA, SEC_N };

// fixup kinds
enum { F_REL32, F_RIPREL, F_ABS64 };

typedef struct ASym {
  const char *name;
  int sec;
  uint64_t off;
  bool defined;
} ASym;

typedef struct Fix {
  int sec;
  uint64_t at; // byte offset of the patched field inside its section
  int kind;
  const char *sym;
} Fix;

typedef struct Asm {
  SB out[SEC_N];
  Vec syms;
  Map by_name;
  Vec fixes;
  int sec;
  uint64_t base; // vaddr of the first text byte
  int errors;
} Asm;

static const char *SEC_NAMES[SEC_N] = {"__text", "__rodata", "__data"};

static void asm_err(Asm *a, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "rho: asm86: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  a->errors++;
}

static void emitb(Asm *a, int byte) {
  sb_printf(&a->out[a->sec], "%c", byte & 0xFF);
}

static void emit4(Asm *a, uint32_t w) {
  sb_printf(&a->out[a->sec], "%c%c%c%c", (int)(w & 0xFF), (int)((w >> 8) & 0xFF),
            (int)((w >> 16) & 0xFF), (int)((w >> 24) & 0xFF));
}

static ASym *sym_get(Asm *a, const char *name, bool define) {
  ASym *s = map_get(&a->by_name, str_from(name));
  if (s)
    return s;
  if (!define)
    return NULL;
  char *copy = arena_alloc(strlen(name) + 1);
  strcpy(copy, name);
  s = arena_alloc_zeroed(sizeof(Sym));
  s->name = copy;
  s->sec = a->sec;
  s->off = a->out[a->sec].n;
  vec_push(&a->syms, s);
  map_put(&a->by_name, str_from(copy), s);
  return s;
}

static void fixup(Asm *a, int kind, const char *sym) {
  Fix *f = arena_alloc(sizeof(Fix));
  f->sec = a->sec;
  f->at = a->out[a->sec].n;
  f->kind = kind;
  f->sym = sym;
  vec_push(&a->fixes, f);
}

// ---------------------------------------------------------- registers ----

enum { R_8, R_16, R_32, R_64, R_XMM, R_NONE };

typedef struct Reg {
  int cls;
  int n;
} Reg;

static Reg parse_reg(const char *s) {
  Reg r = {R_NONE, 0};
  if (!s || !*s)
    return r;
  if (*s == '%') // AT&T register marker
    s++;
  if (!*s)
    return r;
  if (s[0] == 'r' && s[1] >= '0' && s[1] <= '9') {
    char *end;
    long n = strtol(s + 1, &end, 10);
    if (n >= 8 && n <= 15) {
      if (!*end)
        return (Reg){R_64, (int)n};
      if (!strcmp(end, "d"))
        return (Reg){R_32, (int)n};
      if (!strcmp(end, "w"))
        return (Reg){R_16, (int)n};
      if (!strcmp(end, "b"))
        return (Reg){R_8, (int)n};
    }
    return r;
  }
  static const char *N64[8] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};
  static const char *N32[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
  static const char *N16[8] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"};
  static const char *N8[8] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"};
  for (int i = 0; i < 8; i++) {
    if (!strcmp(s, N64[i]))
      return (Reg){R_64, i};
    if (!strcmp(s, N32[i]))
      return (Reg){R_32, i};
    if (!strcmp(s, N16[i]))
      return (Reg){R_16, i};
    if (!strcmp(s, N8[i]))
      return (Reg){R_8, i};
  }
  if (!strncmp(s, "xmm", 3) && s[3] >= '0' && s[3] <= '7' && !s[4])
    return (Reg){R_XMM, s[3] - '0'};
  return r;
}

static int parse_imm(const char *s, int64_t *out) {
  if (*s == '$')
    s++;
  bool neg = false;
  if (*s == '-') {
    neg = true;
    s++;
  }
  if (!*s)
    return 0;
  int64_t v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    while (hexv(*s) >= 0)
      v = v * 16 + hexv(*s++);
  } else {
    while (*s >= '0' && *s <= '9')
      v = v * 10 + (*s++ - '0');
  }
  *out = neg ? -v : v;
  return *s == 0;
}

// memory operand: `disp(%reg)' / `(%reg)' / `sym(%rip)'
typedef struct Mem {
  bool has;
  bool rip;
  const char *ripsym;
  int64_t disp;
  Reg base;
} Mem;

static Mem parse_mem(char *s) {
  Mem m = {0};
  char *lp = strchr(s, '(');
  if (!lp)
    return m;
  m.has = true;
  if (!strncmp(lp, "(%rip)", 6)) {
    m.rip = true;
    *lp = 0;
    m.ripsym = s;
    return m;
  }
  char *close = strchr(lp + 1, ')');
  if (!close)
    return (Mem){0};
  *lp = 0;
  *close = 0;
  m.disp = 0;
  if (*s && !parse_imm(s, &m.disp))
    return (Mem){0};
  m.base = parse_reg(lp + 1);
  if (m.base.cls != R_64)
    return (Mem){0};
  return m;
}

// ------------------------------------------------------------ encoding ----

// legacy prefix then REX; REX appears when the operand is 64-bit, any
// register number needs bit 4, or a byte register needs the spl/bpl/sil/dil
// escape
static void prefix_rex(Asm *a, int prefix, int w, Reg reg, Reg rm) {
  if (prefix)
    emitb(a, prefix);
  bool reghi = reg.cls != R_NONE && reg.n >= 8;
  bool rmhi = (rm.cls == R_64 || rm.cls == R_32 || rm.cls == R_16 || rm.cls == R_8) &&
              rm.n >= 8;
  bool b8 = (reg.cls == R_8 && reg.n >= 4) || (rm.cls == R_8 && rm.n >= 4);
  if (w || reghi || rmhi || b8)
    emitb(a, 0x40 | (w ? 8 : 0) | (reghi ? 4 : 0) | (rmhi ? 1 : 0));
}

// ModRM + displacement for a memory rm operand; regfield is the /digit or
// source register. Returns the fixup position for rip-relative operands,
// -1 otherwise.
static long modrm_mem(Asm *a, int regfield, Mem m) {
  if (m.rip) {
    emitb(a, 0x05 | (regfield & 7) << 3); // mod=00 rm=101
    long at = a->out[a->sec].n;
    emit4(a, 0);
    return at;
  }
  int bn = m.base.n & 7;
  if (m.disp == 0 && bn != 5) { // mod=00 with base; rbp needs a disp (mod00+5 = rip)
    emitb(a, (regfield & 7) << 3 | bn);
    return -1;
  }
  emitb(a, 0x80 | (regfield & 7) << 3 | bn); // mod=10, disp32
  emit4(a, (uint32_t)(int32_t)m.disp);
  return -1;
}

// reg-reg ModRM (mod=11), AT&T order: `op %src, %dst`
static void modrm_rr(Asm *a, Reg src, Reg dst) {
  emitb(a, 0xC0 | (src.n & 7) << 3 | (dst.n & 7));
}

// ALU/mov reg<-reg: opcode with reg field = src, rm = dst
static void op_rr(Asm *a, int prefix, int opc, bool w, Reg src, Reg dst) {
  prefix_rex(a, prefix, w, src, dst);
  emitb(a, opc);
  modrm_rr(a, src, dst);
}

// ALU/mov reg<-mem or mem<-reg; `to_mem`: reg operand is the source
static void op_rm(Asm *a, int prefix, const int *opc, int nopc, bool w, Reg reg,
                  Mem m, bool riprel) {
  prefix_rex(a, prefix, w, reg, (Reg){R_NONE, 0});
  for (int i = 0; i < nopc; i++)
    emitb(a, opc[i]);
  long at = modrm_mem(a, reg.n, m);
  if (riprel && at >= 0) {
    Fix *f = arena_alloc(sizeof(Fix));
    f->sec = a->sec;
    f->at = (uint64_t)at;
    f->kind = F_RIPREL;
    f->sym = m.ripsym;
    vec_push(&a->fixes, f);
  }
}

// immediate group ops (add/or/and/sub/xor/cmp): 83 imm8 / 81 imm32, /digit
static void op_imm(Asm *a, int digit, bool w, int64_t imm, Reg rm) {
  prefix_rex(a, 0, w, (Reg){R_NONE, 0}, rm);
  if (imm >= -128 && imm <= 127) {
    emitb(a, w ? 0x83 : 0x80);
    emitb(a, 0xC0 | digit << 3 | (rm.n & 7));
    emitb(a, (int)(imm & 0xFF));
  } else {
    emitb(a, w ? 0x81 : 0x81);
    emitb(a, 0xC0 | digit << 3 | (rm.n & 7));
    emit4(a, (uint32_t)(int32_t)imm);
  }
}

static const int ALU_OPC[6] = {0x01, 0x29, 0x21, 0x09, 0x31}; // add sub and or xor
static const int ALU_DIG[6] = {0, 5, 4, 1, 6};                // in cmp=7 order

// jcc/setcc condition suffix -> low opcode nibble
static int cc_op(const char *s) {
  static const char *CCS[] = {"o",  "no", "b",  "ae", "e",  "ne", "be", "a",
                              "s",  "ns", "p",  "np", "l",  "ge", "le", "g"};
  for (int i = 0; i < 16; i++)
    if (!strcmp(s, CCS[i]))
      return i;
  return -1;
}

// --------------------------------------------------------------- parse ----

// splits the mnemonic off the first operand, then the rest on top-level
// commas (same rules as asm64.c)
static int split_ops(char *line, char **ops, int max) {
  int n = 0;
  char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!*p || *p == '\n')
    return 0;
  ops[n++] = p;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n')
    p++;
  if (!*p || *p == '\n') {
    *p = 0;
    return n;
  }
  *p++ = 0;
  while (*p == ' ' || *p == '\t')
    p++;
  while (*p && *p != '\n' && n < max) {
    ops[n++] = p;
    bool in_str = false;
    while (*p) {
      if (*p == '"')
        in_str = !in_str;
      else if (!in_str && *p == ',') {
        *p++ = 0;
        break;
      }
      p++;
    }
    while (*p == ' ' || *p == '\t')
      p++;
  }
  return n;
}

static void asm_line(Asm *a, char *mn, char **ops, int nops) {
  // ---- ALU (add/sub/and/or/xor/cmp), AT&T order `op src, dst`;
  // unsuffixed mnemonics only appear for 64-bit pairs ----
  static const struct {
    const char *mn;
    int opc_rm; // r/m <- r opcode row (0x01 family)
    int opc_mr; // r <- r/m opcode row (0x03 family)
    int digit;  // immediate-group /digit
  } ALUS[] = {
      {"add", 0x01, 0x03, 0}, {"sub", 0x29, 0x2B, 5}, {"and", 0x21, 0x23, 4},
      {"or", 0x09, 0x0B, 1},  {"xor", 0x31, 0x33, 6}, {"cmp", 0x39, 0x3B, 7},
  };
  for (size_t i = 0; i < sizeof(ALUS) / sizeof(ALUS[0]); i++) {
    if (strcmp(mn, ALUS[i].mn) && strncmp(mn, ALUS[i].mn, strlen(ALUS[i].mn)))
      continue;
    size_t ml = strlen(ALUS[i].mn);
    char sfx = mn[ml]; // 0 (unsuffixed) / b w l q
    if (sfx && !strchr("bwlq", sfx))
      continue;
    bool w = sfx == 0 || sfx == 'q';
    int size = sfx == 'b' ? 1 : sfx == 'w' ? 2 : sfx == 'l' ? 4 : 8;
    int p66 = sfx == 'w' ? 0x66 : 0;
    if (nops != 2)
      break;
    if (ops[0][0] == '$') { // `op $imm, reg/mem` — the immediate is the source
      int64_t imm;
      if (!parse_imm(ops[0], &imm)) {
        asm_err(a, "bad %s immediate", mn);
        return;
      }
      Reg rm = parse_reg(ops[1]);
      if (rm.cls != R_NONE) {
        op_imm(a, ALUS[i].digit, w, imm, rm);
        return;
      }
      Mem m = parse_mem(ops[1]);
      if (!m.has) {
        asm_err(a, "bad %s operand", mn);
        return;
      }
      // imm -> mem: 80/81/83 /digit by width
      bool imm8 = imm >= -128 && imm <= 127;
      prefix_rex(a, 0, w, (Reg){R_NONE, 0}, (Reg){R_NONE, 0});
      if (size != 1 && imm8) {
        emitb(a, 0x83);
        modrm_mem(a, ALUS[i].digit, m);
        emitb(a, (int)(imm & 0xFF));
      } else {
        emitb(a, size == 1 ? 0x80 : 0x81);
        modrm_mem(a, ALUS[i].digit, m);
        if (size == 1)
          emitb(a, (int)(imm & 0xFF));
        else
          emit4(a, (uint32_t)(int32_t)imm);
      }
      return;
    }
    // register source first, memory source second — both AT&T `op src, dst`
    Reg src = parse_reg(ops[0]);
    if (src.cls != R_NONE) {
      Reg dst = parse_reg(ops[1]);
      if (dst.cls != R_NONE) {
        if (dst.cls != src.cls) {
          asm_err(a, "%s size mismatch", mn);
          return;
        }
        op_rr(a, p66, size == 1 ? ALUS[i].opc_rm - 1 : ALUS[i].opc_rm, size == 8,
              src, dst);
        return;
      }
      Mem m = parse_mem(ops[1]);
      if (!m.has) {
        asm_err(a, "bad %s operand", mn);
        return;
      }
      op_rm(a, p66, (int[]){size == 1 ? ALUS[i].opc_rm - 1 : ALUS[i].opc_rm}, 1,
            size == 8, src, m, false);
      return;
    }
    Mem m = parse_mem(ops[0]);
    if (m.has) {
      Reg dst = parse_reg(ops[1]);
      if (dst.cls != R_NONE) {
        op_rm(a, p66, (int[]){size == 1 ? ALUS[i].opc_mr - 1 : ALUS[i].opc_mr}, 1,
              size == 8, dst, m, false);
        return;
      }
    }
    break;
  }

  // ---- movss / movsd: xmm <-> mem (the rip-relative literal pool rides
  // the load form) ----
  if ((!strcmp(mn, "movss") || !strcmp(mn, "movsd")) && nops == 2) {
    int prefix = mn[4] == 's' ? 0xF3 : 0xF2; // movss F3, movsd F2
    Reg xm;
    Mem m;
    int opc;
    if (parse_reg(ops[0]).cls == R_XMM) { // `movsd %xmm0, mem` — store
      xm = parse_reg(ops[0]);
      m = parse_mem(ops[1]);
      opc = 0x11;
    } else { // `movsd mem, %xmm0` — load
      xm = parse_reg(ops[1]);
      m = parse_mem(ops[0]);
      opc = 0x10;
    }
    if (xm.cls != R_XMM || !m.has) {
      asm_err(a, "bad %s operand", mn);
      return;
    }
    prefix_rex(a, prefix, false, xm, (Reg){R_NONE, 0});
    emitb(a, 0x0F);
    emitb(a, opc);
    long at = modrm_mem(a, xm.n, m);
    if (at >= 0) {
      Fix *f = arena_alloc(sizeof(Fix));
      f->sec = a->sec;
      f->at = (uint64_t)at;
      f->kind = F_RIPREL;
      f->sym = m.ripsym;
      vec_push(&a->fixes, f);
    }
    return;
  }

  // ---- movd / movq with an xmm source: exact bit move xmm -> gpr ----
  if ((!strcmp(mn, "movd") || !strcmp(mn, "movq")) && nops == 2 &&
      parse_reg(ops[0]).cls == R_XMM) {
    Reg xm = parse_reg(ops[0]), g = parse_reg(ops[1]);
    if (g.cls != R_32 && g.cls != R_64) {
      asm_err(a, "bad %s", mn);
      return;
    }
    emitb(a, 0x66);
    prefix_rex(a, 0, g.cls == R_64, g, xm);
    emitb(a, 0x0F);
    emitb(a, 0x7E);
    modrm_rr(a, xm, g); // MOVQ r/m64, xmm: reg = xmm source, rm = gpr
    return;
  }

  // ---- mov family ----
  if (!strncmp(mn, "mov", 3)) {
    const char *sfxs[] = {"b", "w", "l", "q", "absq", "sbq", "swq", "slq", "zbq", "zwq"};
    int sfx = -1;
    for (int i = 0; i < 10; i++)
      if (!strcmp(mn + 3, sfxs[i]))
        sfx = i;
    if (sfx < 0 || nops != 2)
      goto unknown;

    if (sfx == 4) { // movabsq $imm64, reg
      int64_t imm;
      Reg rd = parse_reg(ops[1]);
      if (ops[0][0] != '$' || rd.cls != R_64 || !parse_imm(ops[0], &imm)) {
        asm_err(a, "bad movabsq");
        return;
      }
      prefix_rex(a, 0, true, rd, (Reg){R_NONE, 0});
      emitb(a, 0xB8 | (rd.n & 7));
      emit4(a, (uint32_t)(uint64_t)imm);
      emit4(a, (uint32_t)((uint64_t)imm >> 32));
      return;
    }
    if (sfx >= 5) { // movsbq/movswq/movslq/movzbq/movzwq r64 <- r/m
      // slq = movsxd (63); sbq/swq = movsx (0F BE/BF); zbq/zwq = movzx (0F B6/B7)
      // indexed by suffix: sbq(0F BE) swq(0F BF) slq(63/movsxd) zbq(0F B6) zwq(0F B7)
      static const int XO[5] = {0x0FBE, 0x0FBF, 0x063, 0x0FB6, 0x0FB7};
      int code = XO[sfx - 5];
      Reg rd = parse_reg(ops[1]);
      if (rd.cls != R_64) {
        asm_err(a, "bad movsx/movzx");
        return;
      }
      int opc0 = (code >> 8) & 0xFF, opc1 = code & 0xFF;
      bool two = code > 0xFF;
      Reg rs = parse_reg(ops[0]);
      if (rs.cls != R_NONE) { // reg,reg: reg = dst, rm = src
        prefix_rex(a, 0, true, rd, rs);
        if (two) {
          emitb(a, opc0);
        } else {
          emitb(a, opc1);
        }
        if (two)
          emitb(a, opc1);
        modrm_rr(a, rd, rs);
        return;
      }
      Mem m = parse_mem(ops[0]);
      if (!m.has) {
        asm_err(a, "bad movsx/movzx operand");
        return;
      }
      prefix_rex(a, 0, true, rd, (Reg){R_NONE, 0});
      if (two)
        emitb(a, opc0);
      emitb(a, opc1);
      modrm_mem(a, rd.n, m);
      return;
    }
    // plain moves: b/w/l/q; direction and width from the operand classes
    int size = sfx == 0 ? 1 : sfx == 1 ? 2 : sfx == 2 ? 4 : 8;
    Reg a0 = parse_reg(ops[0]);
    Reg a1 = parse_reg(ops[1]);
    if (ops[0][0] == '$') { // immediate source
      int64_t imm;
      parse_imm(ops[0], &imm);
      if (a1.cls != R_NONE) { // $imm, reg
        if (size == 8) {
          prefix_rex(a, 0, true, (Reg){R_NONE, 0}, a1);
          emitb(a, 0xC7);
          emitb(a, 0xC0 | (a1.n & 7));
          emit4(a, (uint32_t)(int32_t)imm);
        } else if (size == 4 && a1.cls == R_32) {
          emitb(a, 0xB8 | (a1.n & 7));
          emit4(a, (uint32_t)(int32_t)imm);
        } else {
          asm_err(a, "bad mov immediate width");
          return;
        }
        return;
      }
      Mem m = parse_mem(ops[1]); // $imm, mem — the C7 form is 64-bit only
      if (size != 8 || !m.has) {
        asm_err(a, "bad mov immediate operand");
        return;
      }
      prefix_rex(a, 0, true, (Reg){R_NONE, 0}, (Reg){R_NONE, 0});
      emitb(a, 0xC7);
      modrm_mem(a, 0, m);
      emit4(a, (uint32_t)(int32_t)imm);
      return;
    }
    if (a1.cls != R_NONE && a0.cls != R_NONE) { // reg,reg
      if (size == 1)
        op_rr(a, 0, 0x88, false, a0, a1);
      else
        op_rr(a, size == 2 ? 0x66 : 0, 0x89, size == 8, a0, a1);
      return;
    }
    // reg <-> mem, AT&T: a register in the SOURCE position means a store
    Mem m = {0};
    bool to_mem = false;
    Reg r = {R_NONE, 0};
    if (a0.cls != R_NONE) { // `movq %reg, mem`
      m = parse_mem(ops[1]);
      to_mem = true;
      r = a0;
    } else if (a1.cls != R_NONE) { // `movq mem, %reg`
      m = parse_mem(ops[0]);
      r = a1;
    }
    if (!m.has || r.cls == R_NONE) {
      asm_err(a, "bad mov operand");
      return;
    }
    if (to_mem) {
      if (size == 1)
        op_rm(a, 0, (int[]){0x88}, 1, false, r, m, false);
      else
        op_rm(a, size == 2 ? 0x66 : 0, (int[]){0x89}, 1, size == 8, r, m, false);
    } else {
      if (size == 1) { // byte loads ride movsbq/movzbq, never plain movb
        asm_err(a, "bad movb load");
        return;
      }
      op_rm(a, size == 2 ? 0x66 : 0, (int[]){0x8B}, 1, size == 8, r, m, false);
    }
    return;
  }

  if (!strcmp(mn, "leaq") && nops == 2) {
    Reg rd = parse_reg(ops[1]);
    Mem m = parse_mem(ops[0]);
    if (rd.cls != R_64 || !m.has) {
      asm_err(a, "bad leaq");
      return;
    }
    op_rm(a, 0, (int[]){0x8D}, 1, true, rd, m, true);
    return;
  }

  // ---- multiply / divide ----
  if (!strcmp(mn, "imul") && nops == 2) {
    Reg dst = parse_reg(ops[1]), src = parse_reg(ops[0]);
    if (dst.cls != R_64 || src.cls != R_64) {
      asm_err(a, "bad imul");
      return;
    }
    prefix_rex(a, 0, true, dst, src);
    emitb(a, 0x0F);
    emitb(a, 0xAF);
    modrm_rr(a, dst, src);
    return;
  }
  if ((!strcmp(mn, "idivq") || !strcmp(mn, "divq")) && nops == 1) {
    Reg rm = parse_reg(ops[0]);
    if (rm.cls != R_64) {
      asm_err(a, "bad div");
      return;
    }
    prefix_rex(a, 0, true, (Reg){R_NONE, 0}, rm);
    emitb(a, 0xF7);
    emitb(a, 0xC0 | (!strcmp(mn, "idivq") ? 7 : 6) << 3 | (rm.n & 7));
    return;
  }
  if (!strcmp(mn, "cqo") && nops == 0) {
    emitb(a, 0x48);
    emitb(a, 0x99);
    return;
  }
  if (!strcmp(mn, "negq") && nops == 1) {
    Reg rm = parse_reg(ops[0]);
    if (rm.cls != R_64) {
      asm_err(a, "bad negq");
      return;
    }
    prefix_rex(a, 0, true, (Reg){R_NONE, 0}, rm);
    emitb(a, 0xF7);
    emitb(a, 0xC0 | 3 << 3 | (rm.n & 7));
    return;
  }
  if ((!strcmp(mn, "incq") || !strcmp(mn, "decq")) && nops == 1) {
    Reg rm = parse_reg(ops[0]);
    if (rm.cls != R_64) {
      asm_err(a, "bad inc/dec");
      return;
    }
    prefix_rex(a, 0, true, (Reg){R_NONE, 0}, rm);
    emitb(a, 0xFF);
    emitb(a, 0xC0 | (!strcmp(mn, "incq") ? 0 : 1) << 3 | (rm.n & 7));
    return;
  }
  if (!strcmp(mn, "testq") && nops == 2) {
    Reg src = parse_reg(ops[0]), dst = parse_reg(ops[1]);
    if (src.cls != R_64 || dst.cls != R_64) {
      asm_err(a, "bad testq");
      return;
    }
    op_rr(a, 0, 0x85, true, src, dst);
    return;
  }
  // ---- shifts ----
  if ((!strncmp(mn, "sal", 3) || !strncmp(mn, "shl", 3) || !strncmp(mn, "shr", 3) ||
       !strncmp(mn, "sar", 3)) &&
      nops == 2) {
    // sar/sal/shr: sal and sar both have 'a' at [1] — the [2] letter
    // decides (sal/shl 'l'->4, sar 'r'->7, shr 'h?r'->5)
    int digit = mn[2] == 'l' ? 4 : (mn[2] == 'r' && mn[1] == 'a') ? 7 : 5;
    Reg rd = parse_reg(ops[1]);
    if (rd.cls != R_64) {
      asm_err(a, "bad shift");
      return;
    }
    if (ops[0][0] == '$') {
      int64_t imm;
      parse_imm(ops[0], &imm);
      prefix_rex(a, 0, true, (Reg){R_NONE, 0}, rd);
      emitb(a, 0xC1);
      emitb(a, 0xC0 | digit << 3 | (rd.n & 7));
      emitb(a, (int)(imm & 0xFF));
    } else {
      Reg cl = parse_reg(ops[0]);
      if (cl.cls != R_8 || cl.n != 1) {
        asm_err(a, "bad shift count");
        return;
      }
      prefix_rex(a, 0, true, (Reg){R_NONE, 0}, rd);
      emitb(a, 0xD3);
      emitb(a, 0xC0 | digit << 3 | (rd.n & 7));
    }
    return;
  }

  // ---- setcc / byte logic ----
  if (!strncmp(mn, "set", 3) && nops == 1) {
    int cc = cc_op(mn + 3);
    Reg rd = parse_reg(ops[0]);
    if (cc < 0 || rd.cls != R_8) {
      asm_err(a, "bad setcc");
      return;
    }
    prefix_rex(a, 0, false, (Reg){R_NONE, 0}, rd);
    emitb(a, 0x0F);
    emitb(a, 0x90 | cc);
    emitb(a, 0xC0 | (rd.n & 7));
    return;
  }
  if ((!strcmp(mn, "andb") || !strcmp(mn, "orb")) && nops == 2) {
    Reg src = parse_reg(ops[0]), dst = parse_reg(ops[1]);
    if (src.cls != R_8 || dst.cls != R_8) {
      asm_err(a, "bad byte logic");
      return;
    }
    op_rr(a, 0, !strcmp(mn, "andb") ? 0x20 : 0x08, false, src, dst);
    return;
  }

  // ---- branches / calls ----
  if (mn[0] == 'j' && nops == 1) {
    int cc = cc_op(mn + 1);
    if (cc < 0 && !strcmp(mn + 1, "z"))
      cc = 4; // jz/jnz are je/jne
    if (cc < 0 && !strcmp(mn + 1, "nz"))
      cc = 5;
    if (cc >= 0) {
      ASym *s = sym_get(a, ops[0], true);
      emitb(a, 0x0F);
      emitb(a, 0x80 | cc);
      fixup(a, F_REL32, s->name); // the rel32 slot, after the opcode bytes
      emit4(a, 0);
      return;
    }
  }
  if (!strcmp(mn, "jmp") && nops == 1) {
    ASym *s = sym_get(a, ops[0], true);
    emitb(a, 0xE9);
    fixup(a, F_REL32, s->name);
    emit4(a, 0);
    return;
  }
  if (!strcmp(mn, "call") && nops == 1) {
    if (ops[0][0] == '*') {
      Reg rm = parse_reg(ops[0] + 1);
      if (rm.cls != R_64) {
        asm_err(a, "bad indirect call");
        return;
      }
      prefix_rex(a, 0, false, (Reg){R_NONE, 0}, rm);
      emitb(a, 0xFF);
      emitb(a, 0xC0 | 2 << 3 | (rm.n & 7));
      return;
    }
    ASym *s = sym_get(a, ops[0], true);
    emitb(a, 0xE8);
    fixup(a, F_REL32, s->name);
    emit4(a, 0);
    return;
  }
  if (!strcmp(mn, "ret") && nops == 0) {
    emitb(a, 0xC3);
    return;
  }
  if (!strcmp(mn, "syscall") && nops == 0) {
    emitb(a, 0x0F);
    emitb(a, 0x05);
    return;
  }
  if (!strcmp(mn, "cld") && nops == 0) {
    emitb(a, 0xFC);
    return;
  }
  if (!strcmp(mn, "rep") && nops == 1 && !strcmp(ops[0], "movsb")) {
    emitb(a, 0xF3);
    emitb(a, 0xA4);
    return;
  }
  if ((!strcmp(mn, "pushq") || !strcmp(mn, "popq")) && nops == 1) {
    Reg r = parse_reg(ops[0]);
    if (r.cls != R_64) {
      asm_err(a, "bad push/pop");
      return;
    }
    if (r.n >= 8)
      emitb(a, 0x41);
    emitb(a, (!strcmp(mn, "pushq") ? 0x50 : 0x58) | (r.n & 7));
    return;
  }

  // ---- SSE ----
  static const struct {
    const char *mn;
    int prefix;
    int opc;
  } SSE2[] = {
      {"addss", 0xF3, 0x58}, {"addsd", 0xF2, 0x58}, {"subss", 0xF3, 0x5C},
      {"subsd", 0xF2, 0x5C}, {"mulss", 0xF3, 0x59}, {"mulsd", 0xF2, 0x59},
      {"divss", 0xF3, 0x5E}, {"divsd", 0xF2, 0x5E}, {"ucomiss", 0x00, 0x2E},
      {"ucomisd", 0x66, 0x2E}, {"cvtss2sd", 0xF3, 0x5A}, {"cvtsd2ss", 0xF2, 0x5A},
  };
  for (size_t i = 0; i < sizeof(SSE2) / sizeof(SSE2[0]); i++) {
    if (strcmp(mn, SSE2[i].mn) || nops != 2)
      continue;
    Reg dst = parse_reg(ops[1]), src = parse_reg(ops[0]);
    if (dst.cls != R_XMM || src.cls != R_XMM) {
      asm_err(a, "bad %s", mn);
      return;
    }
    prefix_rex(a, SSE2[i].prefix, false, dst, (Reg){R_NONE, 0});
    emitb(a, 0x0F);
    emitb(a, SSE2[i].opc);
    modrm_rr(a, dst, src);
    return;
  }
  if ((!strcmp(mn, "cvtsi2sd") || !strcmp(mn, "cvtsi2ss")) && nops == 2) {
    Reg dst = parse_reg(ops[1]), src = parse_reg(ops[0]);
    if (dst.cls != R_XMM || src.cls != R_64) {
      asm_err(a, "bad cvtsi2s*");
      return;
    }
    prefix_rex(a, !strcmp(mn, "cvtsi2sd") ? 0xF2 : 0xF3, true, dst, src);
    emitb(a, 0x0F);
    emitb(a, 0x2A);
    modrm_rr(a, dst, src);
    return;
  }
  if ((!strncmp(mn, "cvttss2si", 9) || !strncmp(mn, "cvttsd2si", 9)) && nops == 2) {
    Reg dst = parse_reg(ops[1]), src = parse_reg(ops[0]);
    bool q = mn[9] == 'q';
    if (src.cls != R_XMM || dst.cls != (q ? R_64 : R_32)) {
      asm_err(a, "bad cvtt*2si");
      return;
    }
    // cvttss2si vs cvttsd2si share 's' at [4]; the width letter is [5]
    prefix_rex(a, mn[5] == 's' ? 0xF3 : 0xF2, q, dst, (Reg){R_NONE, 0});
    emitb(a, 0x0F);
    emitb(a, 0x2C);
    modrm_rr(a, dst, src);
    return;
  }

  asm_err(a, "unknown instruction `%s'", mn);
  return;
unknown:
  asm_err(a, "unhandled `%s' operand forms", mn);
}

// -------------------------------------------------------------- driver ----

// vaddr of an offset inside a section (8-aligned chaining)
// vaddr of an offset inside a section under the final image layout (text
// at base, strings 8-aligned after text, data on its own page)
typedef struct Layout {
  uint64_t str_a, data_a, heap_a;
} Layout;

static uint64_t sec_vaddr(const Asm *a, const Layout *ly, int sec, uint64_t off) {
  switch (sec) {
  case SEC_TEXT:
    return a->base + off;
  case SEC_STR:
    return ly->str_a + off;
  default:
    return ly->data_a + off;
  }
}

static void resolve_fixups(Asm *a, const Layout *ly) {
  for (size_t i = 0; i < a->fixes.n; i++) {
    Fix *f = a->fixes.items[i];
    ASym *s = map_get(&a->by_name, str_from(f->sym));
    uint64_t target;
    if (s && s->defined)
      target = sec_vaddr(a, ly, s->sec, s->off);
    else if (!strcmp(f->sym, "_rho_rt_heap"))
      target = ly->heap_a;
    else {
      asm_err(a, "undefined symbol `%s'", f->sym);
      continue;
    }
    uint64_t pc = sec_vaddr(a, ly, f->sec, f->at);
    if (f->kind == F_ABS64) {
      uint64_t v = target;
      for (int i = 0; i < 8; i++)
        a->out[f->sec].buf[f->at + i] = (char)((v >> (8 * i)) & 0xFF);
      continue;
    }
    // rel32: branches/calls count from the next instruction, rip-relative
    // from the next byte — the same pc+4 either way
    int64_t d = (int64_t)target - (int64_t)(pc + 4);
    if (d < -(1LL << 31) || d >= (1LL << 31)) {
      asm_err(a, "relocation out of range for `%s'", f->sym);
      continue;
    }
    uint32_t w = (uint32_t)d;
    a->out[f->sec].buf[f->at] = (char)(w & 0xFF);
    a->out[f->sec].buf[f->at + 1] = (char)((w >> 8) & 0xFF);
    a->out[f->sec].buf[f->at + 2] = (char)((w >> 16) & 0xFF);
    a->out[f->sec].buf[f->at + 3] = (char)((w >> 24) & 0xFF);
  }
}

// assembles the full text (runtime blob first, then the emitted program).
// `base` is the vaddr the first text byte will have in the final image.
int asm86_assemble(char *text, uint64_t base, uint64_t page, uint64_t heap_override,
                   SB out[SEC_N], uint64_t *str_va, uint64_t *data_va,
                   uint64_t *heap_va) {
  Asm a;
  memset(&a, 0, sizeof(a));
  a.sec = SEC_TEXT;
  a.base = base;

  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *slash = strstr(line, "//");
    if (slash)
      *slash = 0;
    while (*line == ' ' || *line == '\t')
      line++;
    if (!*line)
      continue;

    // labels first: the amd64 literal pool uses dot-prefixed names
    size_t len = strlen(line);
    if (len && line[len - 1] == ':') {
      line[len - 1] = 0;
      ASym *s = sym_get(&a, line, true);
      if (s->defined) {
        asm_err(&a, "duplicate label `%s'", line);
        continue;
      }
      s->defined = true;
      s->sec = a.sec;
      s->off = a.out[a.sec].n;
      continue;
    }

    if (getenv("RHO_ASM_TRACE"))
      fprintf(stderr, "TRACE86 [%s]\n", line);
    if (line[0] == '.') {
      char *sp = strchr(line, ' ');
      char *rest = "";
      if (sp) {
        *sp = 0;
        rest = sp + 1;
        while (*rest == ' ')
          rest++;
      }
      if (!strcmp(line, ".text") || !strcmp(line, ".section")) {
        if (!strcmp(line, ".section")) {
          // ".rodata" folds into the text image (one r-x page in the
          // final ELF); ".data" splits off
          if (!strncmp(rest, ".rodata", 7) || !strncmp(rest, "__rodata", 8) ||
              !strncmp(rest, "__TEXT", 6))
            a.sec = SEC_TEXT;
          else if (!strncmp(rest, ".data", 5) || !strncmp(rest, "__data", 6) ||
                   !strncmp(rest, "__DATA", 6))
            a.sec = SEC_DATA;
          else
            asm_err(&a, "unknown section `%s'", rest);
        } else {
          a.sec = SEC_TEXT;
        }
      } else if (!strcmp(line, ".globl")) {
        sym_get(&a, rest, true);
      } else if (!strcmp(line, ".p2align")) {
        int n = atoi(rest);
        uint64_t mask = ((uint64_t)1 << n) - 1;
        while (a.out[a.sec].n & mask)
          sb_printf(&a.out[a.sec], "%c", 0);
      } else if (!strcmp(line, ".quad")) {
        int64_t v = 0;
        if (parse_imm(rest, &v)) {
          for (int i = 0; i < 8; i++)
            sb_printf(&a.out[a.sec], "%c", (int)((uint64_t)v >> (8 * i) & 0xFF));
        } else { // `.quad Lsymbol` — absolute address relocation
          ASym *s = sym_get(&a, rest, true);
          fixup(&a, F_ABS64, s->name);
          for (int i = 0; i < 8; i++)
            sb_printf(&a.out[a.sec], "%c", 0);
        }
      } else if (!strcmp(line, ".long")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        emit4(&a, (uint32_t)(uint64_t)v);
      } else if (!strcmp(line, ".long")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        for (int i = 0; i < 4; i++)
          sb_printf(&a.out[a.sec], "%c", (int)((uint64_t)v >> (8 * i) & 0xFF));
      } else if (!strcmp(line, ".byte")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        emitb(&a, (int)(v & 0xFF));
      } else if (!strcmp(line, ".zero")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        for (int64_t i = 0; i < v; i++)
          sb_printf(&a.out[a.sec], "%c", 0);
      } else if (!strcmp(line, ".ascii")) {
        char *q = strchr(rest, '"');
        if (!q)
          continue;
        q++;
        for (char *p = q; *p && *p != '"'; p++) {
          if (*p == '\\' && p[1] >= '0' && p[1] <= '7') {
            int v = 0, nd = 0;
            p++;
            while (*p >= '0' && *p <= '7' && nd < 3) {
              v = v * 8 + (*p - '0');
              p++;
              nd++;
            }
            p--;
            sb_printf(&a.out[a.sec], "%c", v);
          } else {
            sb_printf(&a.out[a.sec], "%c", *p);
          }
        }
      }
      // anything else: skipped
      continue;
    }

    char *op[8];
    int nops = split_ops(line, op, 8);
    if (nops)
      asm_line(&a, op[0], op + 1, nops - 1);
  }

  // layout: text | strings | page | data | page | heap (matches asm64.c)
  const uint64_t pmask = page - 1;
  Layout ly;
  ly.str_a = base + ((a.out[SEC_TEXT].n + 7) & ~(uint64_t)7);
  // the data page starts past EVERYTHING in the r-x image — the rodata
  // strings fold into the text section bytes, so rounding past str alone
  // would let __DATA overlap the string tail
  ly.data_a = (ly.str_a + a.out[SEC_STR].n + pmask) & ~pmask;
  if (ly.data_a < base + a.out[SEC_TEXT].n)
    ly.data_a = (base + a.out[SEC_TEXT].n + pmask) & ~pmask;
  ly.heap_a = heap_override
                  ? heap_override
                  : (ly.data_a + a.out[SEC_DATA].n + pmask) & ~pmask;

  resolve_fixups(&a, &ly);
  if (a.errors)
    return 1;

  for (int i = 0; i < SEC_N; i++)
    out[i] = a.out[i];
  *str_va = ly.str_a;
  *data_va = ly.data_a;
  *heap_va = ly.heap_a;
  return 0;
}
