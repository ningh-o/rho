// The in-tree arm64 assembler for rho's own assembly format — the format
// the emit_arm64 backend prints. A straight-line parser (directives,
// labels, the ~45 instruction forms the backend emits) plus a fixup pass
// (branch / page / pc-relative relocations against the section symbol
// table) produces three raw section images (text / rodata strings / data)
// that macho64.c wraps into a static executable. Nothing here or downstream
// shells out to an external assembler or linker.
//
// Section vaddrs are laid out here (text, strings, then a page-aligned
// data segment whose end is the bump-allocator heap) so that ADRP pages
// match what macho64.c writes — one layout, one truth.
//
// Special undefined symbols, bound by the image builder:
//   _rho_rt_heap — the first byte after __DATA (the bump-allocator heap)
// (the program entry is `_main', which the emitter itself provides)
//
// Every encoding constant is pinned against the system assembler's output
// (tools/check_asm64.sh diffs all forms byte for byte — a development-time
// oracle, never a build or runtime dependency).

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
enum { F_ADRP, F_ADDPAGE, F_B26, F_BL26, F_BC19, F_CB19, F_ABS64 };

typedef struct ASym {
  const char *name;
  int sec;
  uint64_t off;
  bool defined;
} ASym;

typedef struct Fix {
  int sec;
  uint64_t at; // byte offset of the instruction word inside its section
  int kind;
  const char *sym;
} Fix;

typedef struct Asm {
  SB out[SEC_N];
  Vec syms;  // Sym*
  Map by_name;
  Vec fixes; // Fix*
  int sec;
  uint64_t base; // vaddr of the first text byte (image base + header size)
  int errors;
} Asm;

// ------------------------------------------------------------- helpers ----

static const char *SEC_NAMES[SEC_N] = {"__text", "__rhostr", "__data"};

static void asm_err(Asm *a, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "rho: asm64: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  a->errors++;
}

static void emit32(Asm *a, uint32_t w) {
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

// ---------------------------------------------------------- registers ----

enum { RC_X, RC_W, RC_SP, RC_VS, RC_VD, RC_NONE };

typedef struct Reg {
  int cls;
  int n;
} Reg;

static Reg parse_reg(const char *s) {
  Reg r = {RC_NONE, 0};
  if (!s || !*s)
    return r;
  if (!strcmp(s, "sp"))
    return (Reg){RC_SP, 31};
  if (!strcmp(s, "xzr"))
    return (Reg){RC_X, 31};
  if (s[1] >= '0' && s[1] <= '9') {
    int n = atoi(s + 1);
    if (n > 30)
      return r;
    switch (s[0]) {
    case 'x':
      return (Reg){RC_X, n};
    case 'w':
      return (Reg){RC_W, n};
    case 's':
      return (Reg){RC_VS, n};
    case 'd':
      return (Reg){RC_VD, n};
    }
  }
  return r;
}

static int parse_imm(const char *s, int64_t *out) {
  if (*s == '#')
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

static int cond_code(const char *s) {
  // ARM condition codes; hs/lo are aliases of cs/cc
  static const char *conds[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le"};
  for (int i = 0; i < 14; i++)
    if (!strcmp(s, conds[i]))
      return i;
  if (!strcmp(s, "hs"))
    return 2;
  if (!strcmp(s, "lo"))
    return 3;
  return -1;
}

// ------------------------------------------------------------ encoding ----

static void fixup(Asm *a, int kind, const char *sym) {
  Fix *f = arena_alloc(sizeof(Fix));
  f->sec = a->sec;
  f->at = a->out[a->sec].n;
  f->kind = kind;
  f->sym = sym;
  vec_push(&a->fixes, f);
}

static char *sym_ref(char *s) {
  char *at = strstr(s, "@PAGE");
  if (at)
    *at = 0;
  return s;
}

static void ins_addsub_imm(Asm *a, bool is_sub, bool set, Reg rd, Reg rn,
                           int64_t imm, const char *mn) {
  if (imm < 0) { // the emitter prints negative immediates; flip add/sub
    is_sub = !is_sub;
    imm = -imm;
  }
  if (imm > 4095) {
    asm_err(a, "%s immediate out of range: %lld", mn, (long long)imm);
    return;
  }
  uint32_t base;
  if (rd.cls == RC_W) // sp always rides the 64-bit form
    base = is_sub ? (set ? 0x71000000u : 0x51000000u) : (set ? 0x31000000u : 0x11000000u);
  else
    base = is_sub ? (set ? 0xF1000000u : 0xD1000000u) : (set ? 0xB1000000u : 0x91000000u);
  uint32_t rn_enc = rn.cls == RC_SP ? 31 : rn.n;
  uint32_t rd_enc = rd.cls == RC_SP ? 31 : rd.n;
  emit32(a, base | (uint32_t)imm << 10 | rn_enc << 5 | rd_enc);
}

static void ins_addsub_reg(Asm *a, bool is_sub, bool set, Reg rd, Reg rn, Reg rm) {
  uint32_t base = rd.cls == RC_X ? (is_sub ? (set ? 0xEB000000u : 0xCB000000u)
                                           : (set ? 0xAB000000u : 0x8B000000u))
                                 : (is_sub ? (set ? 0x6B000000u : 0x4B000000u)
                                           : (set ? 0x2B000000u : 0x0B000000u));
  uint32_t rn_enc = rn.cls == RC_SP ? 31 : rn.n;
  uint32_t rd_enc = rd.cls == RC_SP ? 31 : rd.n;
  emit32(a, base | rm.n << 16 | rn_enc << 5 | rd_enc);
}

static void ins_movz(Asm *a, Reg rd, int64_t imm, int hw) {
  if (imm < 0 || imm > 0xFFFF || hw < 0 || hw > (rd.cls == RC_X ? 3 : 1)) {
    asm_err(a, "movz/movk out of range: %lld", (long long)imm);
    return;
  }
  uint32_t base = rd.cls == RC_X ? 0xD2800000u : 0x52800000u;
  emit32(a, base | (uint32_t)hw << 21 | (uint32_t)imm << 5 | rd.n);
}

static void ins_movk(Asm *a, Reg rd, int64_t imm, int hw) {
  if (imm < 0 || imm > 0xFFFF || hw < 0 || hw > (rd.cls == RC_X ? 3 : 1)) {
    asm_err(a, "movk out of range: %lld", (long long)imm);
    return;
  }
  uint32_t base = rd.cls == RC_X ? 0xF2800000u : 0x72800000u;
  emit32(a, base | (uint32_t)hw << 21 | (uint32_t)imm << 5 | rd.n);
}

// loads/stores, unsigned-offset [Xn] forms (imm12 = 0: every form the
// backend emits uses a bare base register)
static void ins_mem(Asm *a, uint32_t base, Reg rt, Reg rn) {
  emit32(a, base | rn.n << 5 | rt.n);
}

static void ins_pair(Asm *a, bool load, bool post, Reg rt, Reg rt2, Reg rn,
                     int64_t imm) {
  if (imm % 8 != 0 || imm > 240 || imm < -256) {
    asm_err(a, "stp/ldp offset out of range: %lld", (long long)imm);
    return;
  }
  uint32_t imm7 = (uint32_t)(imm / 8) & 0x7F;
  uint32_t base = load ? (post ? 0xA8C00000u : 0xA9C00000u)
                       : (post ? 0xA8800000u : 0xA9800000u);
  emit32(a, base | imm7 << 15 | rt2.n << 10 | rn.n << 5 | rt.n);
}

static void ins_dprec2(Asm *a, uint32_t opcode6, Reg rd, Reg rn, Reg rm) {
  emit32(a, 0x9AC00000u | rm.n << 16 | opcode6 << 10 | rn.n << 5 | rd.n);
}

static void ins_dprec3(Asm *a, bool msub, Reg rd, Reg rn, Reg rm, Reg ra) {
  uint32_t base = 0x9B000000u | (msub ? 0x8000u : 0);
  emit32(a, base | rm.n << 16 | ra.n << 10 | rn.n << 5 | rd.n);
}

// FP data-processing (3-source): opcode FMUL=0 FDIV=1 FADD=2 FSUB=3
static void ins_fp3(Asm *a, int opcode, Reg rd, Reg rn, Reg rm) {
  uint32_t word = 0x1E000000u | (rd.cls == RC_VD ? 1u << 22 : 0) | 1u << 21 |
                  rm.n << 16 | (uint32_t)opcode << 12 | 1u << 11 | rn.n << 5 | rd.n;
  emit32(a, word);
}

// --------------------------------------------------------------- parse ----

// splits the mnemonic off the first operand, then the rest on top-level
// commas; bracketed groups and quoted strings keep their commas
// ("[sp, #-16]!" stays one operand)
static int split_ops(char *line, char **ops, int max) {
  int n = 0;
  char *p = line;
  while (*p == ' ' || *p == '\t')
    p++;
  if (!*p || *p == '\n')
    return 0;
  ops[n++] = p; // the mnemonic
  while (*p && *p != ' ' && *p != '\t' && *p != '\n')
    p++;
  if (!*p || *p == '\n') {
    *p = 0;
    return n;
  }
  *p++ = 0; // terminate the mnemonic at the space
  while (*p == ' ' || *p == '\t')
    p++;
  while (*p && *p != '\n' && n < max) {
    ops[n++] = p;
    bool in_br = false, in_str = false;
    while (*p) {
      if (*p == '"')
        in_str = !in_str;
      else if (!in_str && *p == '[')
        in_br = true;
      else if (!in_str && *p == ']')
        in_br = false;
      else if (!in_str && !in_br && *p == ',') {
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
  if (mn[0] == 'b' && mn[1] == '.') { // b.cond
    int cond = cond_code(mn + 2);
    if (cond < 0 || nops != 1) {
      asm_err(a, "bad b.cond");
      return;
    }
    ASym *s = sym_get(a, ops[0], true);
    fixup(a, F_BC19, s->name);
    emit32(a, 0x54000000u | (uint32_t)cond);
    return;
  }
  if (!strcmp(mn, "adrp") && nops == 2) {
    Reg rd = parse_reg(ops[0]);
    if (rd.cls != RC_X) {
      asm_err(a, "bad adrp");
      return;
    }
    ASym *s = sym_get(a, sym_ref(ops[1]), true);
    fixup(a, F_ADRP, s->name);
    emit32(a, 0x90000000u | rd.n);
    return;
  }
  if ((!strcmp(mn, "add") || !strcmp(mn, "sub")) && nops == 3) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    bool is_sub = mn[0] == 's';
    if (ops[2][0] == '#') {
      int64_t imm;
      if (!parse_imm(ops[2], &imm)) {
        asm_err(a, "bad %s immediate", mn);
        return;
      }
      ins_addsub_imm(a, is_sub, false, rd, rn, imm, mn);
    } else if (strchr(ops[2], '@') || parse_reg(ops[2]).cls == RC_NONE) {
      // @PAGEOFF marker (mac dialect) or a bare symbol (ELF dialect):
      // the low page half of a page-relative address
      if (is_sub || rd.cls != RC_X) {
        asm_err(a, "bad pageoff add");
        return;
      }
      ASym *s = sym_get(a, sym_ref(ops[2]), true);
      fixup(a, F_ADDPAGE, s->name);
      emit32(a, 0x91000000u | rn.n << 5 | rd.n);
    } else {
      ins_addsub_reg(a, is_sub, false, rd, rn, parse_reg(ops[2]));
    }
    return;
  }
  if (!strcmp(mn, "cmp") && nops == 2) {
    Reg rn = parse_reg(ops[0]), xzr = {RC_X, 31};
    if (ops[1][0] == '#') {
      int64_t imm;
      parse_imm(ops[1], &imm);
      ins_addsub_imm(a, true, true, xzr, rn, imm, mn);
    } else {
      ins_addsub_reg(a, true, true, xzr, rn, parse_reg(ops[1]));
    }
    return;
  }
  if (!strcmp(mn, "subs") && nops == 3) { // explicit setflags subtract
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    if (ops[2][0] == '#') {
      int64_t imm;
      parse_imm(ops[2], &imm);
      ins_addsub_imm(a, true, true, rd, rn, imm, mn);
    } else {
      ins_addsub_reg(a, true, true, rd, rn, parse_reg(ops[2]));
    }
    return;
  }
  if (!strcmp(mn, "cset") && nops == 2) {
    Reg rd = parse_reg(ops[0]);
    int cond = cond_code(ops[1]);
    if (rd.cls != RC_X || cond < 0) {
      asm_err(a, "bad cset");
      return;
    }
    // CSET Xd, cond = CSINC Xd, XZR, XZR, cond^1
    emit32(a, 0x9A9F07E0u | (uint32_t)(cond ^ 1) << 12 | rd.n);
    return;
  }
  if ((!strcmp(mn, "cbz") || !strcmp(mn, "cbnz")) && nops == 2) {
    Reg rt = parse_reg(ops[0]);
    ASym *s = sym_get(a, ops[1], true);
    fixup(a, F_CB19, s->name);
    bool nz = mn[2] == 'n';
    emit32(a, (rt.cls == RC_X ? (nz ? 0xB5000000u : 0xB4000000u)
                              : (nz ? 0x35000000u : 0x34000000u)) |
                   rt.n);
    return;
  }
  if (mn[0] == 'b' && !mn[1] && nops == 1) {
    ASym *s = sym_get(a, ops[0], true);
    fixup(a, F_B26, s->name);
    emit32(a, 0x14000000u);
    return;
  }
  if (!strcmp(mn, "bl") && nops == 1) {
    ASym *s = sym_get(a, ops[0], true);
    fixup(a, F_BL26, s->name);
    emit32(a, 0x94000000u);
    return;
  }
  if (!strcmp(mn, "blr") && nops == 1) {
    Reg rn = parse_reg(ops[0]);
    emit32(a, 0xD63F0000u | rn.n << 5);
    return;
  }
  if (!strcmp(mn, "ret") && nops == 0) {
    emit32(a, 0xD65F03C0u);
    return;
  }
  if (!strcmp(mn, "svc") && nops == 1) {
    int64_t imm = 0;
    parse_imm(ops[0], &imm);
    emit32(a, 0xD4000001u | (uint32_t)imm << 5);
    return;
  }
  if (!strcmp(mn, "mov") && nops == 2) {
    Reg rd = parse_reg(ops[0]);
    if (ops[1][0] == '#') {
      int64_t imm;
      parse_imm(ops[1], &imm);
      ins_movz(a, rd, imm, 0);
    } else if (rd.cls == RC_SP || !strcmp(ops[1], "sp")) {
      // mov SP, Xn / mov Xd, SP — the ADD-immediate #0 form
      ins_addsub_imm(a, false, false, rd, parse_reg(ops[1]), 0, "mov");
    } else {
      Reg rm = parse_reg(ops[1]);
      uint32_t base = rd.cls == RC_X ? 0xAA000000u : 0x2A000000u;
      emit32(a, base | rm.n << 16 | 31u << 5 | rd.n); // ORR Xd, XZR, Xm
    }
    return;
  }
  if (!strcmp(mn, "movz") && nops == 2) {
    int64_t imm = 0;
    parse_imm(ops[1], &imm);
    ins_movz(a, parse_reg(ops[0]), imm, 0);
    return;
  }
  if (!strcmp(mn, "movk") && (nops == 2 || nops == 3)) {
    int64_t imm = 0, hw = 0;
    parse_imm(ops[1], &imm);
    if (nops == 3 && !strncmp(ops[2], "lsl", 3))
      hw = atoi(ops[2] + 3) / 16;
    ins_movk(a, parse_reg(ops[0]), imm, (int)hw);
    return;
  }
  if ((!strcmp(mn, "stp") || !strcmp(mn, "ldp")) && nops >= 3) {
    bool load = mn[0] == 'l';
    Reg rt = parse_reg(ops[0]), rt2 = parse_reg(ops[1]);
    if (nops == 3 && !strncmp(ops[2], "[sp, ", 5)) { // pre-index [sp, #-N]!
      int64_t imm = 0;
      char *imm_s = ops[2] + 5;          // after "[sp, "
      char *close = strchr(imm_s, ']');  // "#-16]!"
      if (close)
        *close = 0;
      if (parse_imm(imm_s, &imm)) {
        Reg sp = {RC_SP, 31};
        ins_pair(a, load, false, rt, rt2, sp, imm);
        return;
      }
    } else if (nops == 4 && !strcmp(ops[2], "[sp]")) { // post-index [sp], #N
      int64_t imm = 0;
      if (parse_imm(ops[3], &imm)) {
        Reg sp = {RC_SP, 31};
        ins_pair(a, load, true, rt, rt2, sp, imm);
        return;
      }
    }
    asm_err(a, "bad %s addressing", mn);
    return;
  }
  if (!strcmp(mn, "mul") && nops == 3) {
    Reg xzr = {RC_X, 31};
    ins_dprec3(a, false, parse_reg(ops[0]), parse_reg(ops[1]), parse_reg(ops[2]),
               xzr);
    return;
  }
  if (!strcmp(mn, "msub") && nops == 4) {
    ins_dprec3(a, true, parse_reg(ops[0]), parse_reg(ops[1]), parse_reg(ops[2]),
               parse_reg(ops[3]));
    return;
  }
  if ((!strcmp(mn, "sdiv") || !strcmp(mn, "udiv")) && nops == 3) {
    ins_dprec2(a, !strcmp(mn, "sdiv") ? 0x03u : 0x02u, parse_reg(ops[0]),
               parse_reg(ops[1]), parse_reg(ops[2]));
    return;
  }
  if ((!strcmp(mn, "lslv") || !strcmp(mn, "lsrv") || !strcmp(mn, "asrv")) &&
      nops == 3) {
    uint32_t op = !strcmp(mn, "lslv") ? 0x08u : !strcmp(mn, "lsrv") ? 0x09u : 0x0Au;
    ins_dprec2(a, op, parse_reg(ops[0]), parse_reg(ops[1]), parse_reg(ops[2]));
    return;
  }
  if ((!strcmp(mn, "and") || !strcmp(mn, "orr") || !strcmp(mn, "eor")) && nops == 3) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]), rm = parse_reg(ops[2]);
    int opc = !strcmp(mn, "and") ? 0 : !strcmp(mn, "orr") ? 1 : 2;
    uint32_t base = rd.cls == RC_X ? 0x8A000000u : 0x0A000000u;
    base |= (uint32_t)(opc >> 1) << 30 | (uint32_t)(opc & 1) << 29;
    emit32(a, base | rm.n << 16 | rn.n << 5 | rd.n);
    return;
  }
  if (!strcmp(mn, "neg") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rm = parse_reg(ops[1]);
    uint32_t base = rd.cls == RC_X ? 0xCB0003E0u : 0x4B0003E0u;
    emit32(a, base | rm.n << 16 | rd.n);
    return;
  }
  if (!strcmp(mn, "sxtw") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    emit32(a, 0x93407C00u | rn.n << 5 | rd.n); // SBFM Xd, Wn, #0, #31
    return;
  }
  if (!strcmp(mn, "uxtw") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    emit32(a, 0xD3407C00u | rn.n << 5 | rd.n); // UBFM Xd, Wn, #0, #31
    return;
  }
  if (!strcmp(mn, "uxtb") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    emit32(a, 0x53001C00u | rn.n << 5 | rd.n); // UBFM Wd, Wn, #0, #7
    return;
  }
  if ((!strcmp(mn, "fadd") || !strcmp(mn, "fsub") || !strcmp(mn, "fmul") ||
       !strcmp(mn, "fdiv")) && nops == 3) {
    int opcode = !strcmp(mn, "fmul") ? 0 : !strcmp(mn, "fdiv") ? 1
               : !strcmp(mn, "fadd")            ? 2
                                                : 3;
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]), rm = parse_reg(ops[2]);
    uint32_t word = 0x1E000000u | (rd.cls == RC_VD ? 1u << 22 : 0) | 1u << 21 |
                    rm.n << 16 | (uint32_t)opcode << 12 | 1u << 11 | rn.n << 5 | rd.n;
    emit32(a, word);
    return;
  }
  if (!strcmp(mn, "fcmp") && nops == 2) {
    Reg rn = parse_reg(ops[0]), rm = parse_reg(ops[1]);
    uint32_t base = 0x1E202000u | (rn.cls == RC_VD ? 1u << 22 : 0);
    emit32(a, base | rm.n << 16 | rn.n << 5);
    return;
  }
  if (!strcmp(mn, "fcvt") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    if (rd.cls == RC_VD && rn.cls == RC_VS)
      emit32(a, 0x1E22C000u | rn.n << 5 | rd.n); // S -> D
    else
      asm_err(a, "bad fcvt");
    return;
  }
  if (!strcmp(mn, "fcvtzs") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    emit32(a, (rd.cls == RC_X ? 0x9E780000u : 0x1E780000u) | rn.n << 5 | rd.n);
    return;
  }
  if (!strcmp(mn, "fmov") && nops == 2) {
    Reg rd = parse_reg(ops[0]), rn = parse_reg(ops[1]);
    if (rd.cls == RC_X && rn.cls == RC_VD)
      emit32(a, 0x9E660000u | rn.n << 5 | rd.n);
    else if (rd.cls == RC_VD && rn.cls == RC_X)
      emit32(a, 0x9E670000u | rn.n << 5 | rd.n);
    else if (rd.cls == RC_VS && rn.cls == RC_W)
      emit32(a, 0x1E270000u | rn.n << 5 | rd.n);
    else
      asm_err(a, "bad fmov");
    return;
  }

  // loads/stores — mnemonic plus register class pick the encoding row
  static const struct {
    const char *mn;
    uint32_t base;
    int cls;
  } MEMS[] = {
      {"str", 0xF9000000u, RC_X},   {"ldr", 0xF9400000u, RC_X},
      {"str", 0xB9000000u, RC_W},   {"ldr", 0xB9400000u, RC_W},
      {"str", 0xBD000000u, RC_VS},  {"ldr", 0xBD400000u, RC_VS},
      {"str", 0xFD000000u, RC_VD},  {"ldr", 0xFD400000u, RC_VD},
      {"strb", 0x39000000u, RC_W},  {"ldrb", 0x39400000u, RC_W},
      {"strh", 0x79000000u, RC_W},  {"ldrh", 0x79400000u, RC_W},
      {"ldrsb", 0x39800000u, RC_X}, {"ldrsh", 0x79800000u, RC_X},
      {"ldrsw", 0xB9800000u, RC_X},
  };
  for (size_t i = 0; i < sizeof(MEMS) / sizeof(MEMS[0]); i++) {
    if (strcmp(mn, MEMS[i].mn))
      continue;
    Reg rt = parse_reg(ops[0]);
    if (rt.cls != MEMS[i].cls)
      continue;
    if (ops[1][0] != '[') {
      asm_err(a, "bad %s addressing", mn);
      return;
    }
    Reg rn = parse_reg(ops[1] + 1);
    ins_mem(a, MEMS[i].base, rt, rn);
    return;
  }

  asm_err(a, "unknown instruction `%s'", mn);
}

// -------------------------------------------------------------- driver ----

// vaddr of an offset inside a section under the final image layout: text
// at base, strings 8-aligned after text (same page), data on its own page
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
      asm_err(a, "undefined symbol `%s' (fixup kind %d)", f->sym, f->kind);
      continue;
    }
    uint64_t pc = sec_vaddr(a, ly, f->sec, f->at);
    uint32_t w;
    memcpy(&w, a->out[f->sec].buf + f->at, 4);
    switch (f->kind) {
    case F_ADRP: {
      int64_t page = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
      if (page < -(1 << 20) || page >= (1 << 20)) {
        asm_err(a, "adrp page out of range");
        continue;
      }
      w |= (uint32_t)(page & 3) << 29 | (uint32_t)((page >> 2) & 0x7FFFF) << 5;
      break;
    }
    case F_ADDPAGE:
      w |= (uint32_t)(target & 0xFFF) << 10;
      break;
    case F_B26:
    case F_BL26: {
      int64_t d = ((int64_t)target - (int64_t)pc) >> 2;
      if (d < -(1 << 25) || d >= (1 << 25)) {
        asm_err(a, "branch out of range");
        continue;
      }
      w |= (uint32_t)d & 0x3FFFFFF;
      break;
    }
    case F_ABS64:
      w = (uint32_t)target;
      memcpy(a->out[f->sec].buf + f->at, &w, 4);
      w = (uint32_t)(target >> 32);
      memcpy(a->out[f->sec].buf + f->at + 4, &w, 4);
      continue;
    case F_BC19:
    case F_CB19: {
      int64_t d = ((int64_t)target - (int64_t)pc) >> 2;
      if (d < -(1 << 18) || d >= (1 << 18)) {
        asm_err(a, "branch out of range");
        continue;
      }
      w |= (uint32_t)(d & 0x7FFFF) << 5;
      break;
    }
    }
    memcpy(a->out[f->sec].buf + f->at, &w, 4);
  }
}

// assembles the full text (runtime blob first, then the emitted program).
// `base` is the vaddr the first text byte will have in the final image;
// `page` is the target's segment alignment (0x1000 linux, 0x4000 arm64-mac
// — Apple Silicon maps segments on 16K boundaries and the kernel's strict
// validation rejects anything else). `heap_override` pins the bump heap's
// vaddr (mac images carry the heap in a fixed trailing segment); 0 = lay
// it out right after the data page. On success the three section images
// live in out[], and the layout the image writer must reproduce is
// returned.
int asm64_assemble(char *text, uint64_t base, uint64_t page, uint64_t heap_override,
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

    if (line[0] == '.') {
      char *sp = strchr(line, ' ');
      char *rest = "";
      if (sp) {
        *sp = 0;
        rest = sp + 1;
        while (*rest == ' ')
          rest++;
      }
      if (!strcmp(line, ".text") || !strcmp(line, ".rodata")) {
        // standalone section switches (the ELF dialect emits them)
        a.sec = SEC_TEXT;
      } else if (!strcmp(line, ".data")) {
        a.sec = SEC_DATA;
      } else if (!strcmp(line, ".section")) {
        // the rodata strings fold into the text section image: they ride
        // the same bytes in the final image (one r-x page). mach-o names
        // come in as __text/__rhostr/__data fragments; ELF targets use
        // the plain dot names
        if (!strcmp(rest, ".text") || !strcmp(rest, ".rodata"))
          a.sec = SEC_TEXT;
        else if (!strcmp(rest, ".data"))
          a.sec = SEC_DATA;
        else {
          a.sec = SEC_TEXT;
          for (int i = 0; i < SEC_N; i++)
            if (strstr(rest, SEC_NAMES[i]))
              a.sec = i == SEC_STR ? SEC_TEXT : i;
        }
      } else if (!strcmp(line, ".globl")) {
        char *e = strchr(rest, '\n');
        if (e)
          *e = 0;
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
        } else { // `.quad Lsymbol` — absolute address relocation (any label name)
          ASym *s = sym_get(&a, rest, true);
          fixup(&a, F_ABS64, s->name);
          for (int i = 0; i < 8; i++)
            sb_printf(&a.out[a.sec], "%c", 0);
        }
      } else if (!strcmp(line, ".long")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        for (int i = 0; i < 4; i++)
          sb_printf(&a.out[a.sec], "%c", (int)((uint64_t)v >> (8 * i) & 0xFF));
      } else if (!strcmp(line, ".byte")) {
        int64_t v = 0;
        parse_imm(rest, &v);
        sb_printf(&a.out[a.sec], "%c", (int)(v & 0xFF));
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
      // .build_version / .subsections_via_symbols / anything else: skipped
      continue;
    }

    size_t len = strlen(line);
    if (len && line[len - 1] == ':') {
      line[len - 1] = 0;
      if (getenv("RHO_ASM_DEBUG") && strstr(line, "HEAP"))
        fprintf(stderr, "DBG label line=[%s] sec=%d off=%zx\n", line, a.sec,
                a.out[a.sec].n);
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

    char *ops[8];
    int nops = split_ops(line, ops, 8);
    if (nops)
      asm_line(&a, ops[0], ops + 1, nops - 1);
  }

  // layout: text | strings | page | data | page | heap
  const uint64_t pmask = page - 1;
  Layout ly;
  ly.str_a = base + ((a.out[SEC_TEXT].n + 7) & ~(uint64_t)7);
  // the data page starts past EVERYTHING in the r-x image — the rodata
  // strings fold into the text section bytes, so rounding past str alone
  // would let __DATA overlap the string tail
  ly.data_a = (ly.str_a + a.out[SEC_STR].n + pmask) & ~pmask;
  if (ly.data_a < base + a.out[SEC_TEXT].n)
    ly.data_a = (base + a.out[SEC_TEXT].n + pmask) & ~pmask;
  ly.heap_a = heap_override ? heap_override
                            : (ly.data_a + a.out[SEC_DATA].n + pmask) & ~pmask;

  if (getenv("RHO_ASM_DEBUG")) {
    for (size_t i = 0; i < a.syms.n; i++) {
      ASym *sy = a.syms.items[i];
      fprintf(stderr, "DBG sym [%s] sec=%d off=%#llx defined=%d -> %#llx\n",
              sy->name, sy->sec, (unsigned long long)sy->off,
              sy->defined, sec_vaddr(&a, &ly, sy->sec, sy->off));
    }
    fprintf(stderr, "DBG heap_va=%#llx text=%#llx str=%#llx data=%#llx\n",
            ly.heap_a, ly.str_a, ly.data_a, ly.heap_a);
    fprintf(stderr, "DBG sizes: text=%zu str=%zu data=%zu\n",
            a.out[0].n, a.out[1].n, a.out[2].n);
  }
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

// development-time oracle path (`rho asmtest in.s out.bin`): raw
// text|str|data with 8-byte padding, no Mach-O wrapper
int asm64_test(const char *text, const char *out_path) {
  SB out[SEC_N];
  uint64_t str_va, data_va, heap_va;
  char *buf = arena_alloc(strlen(text) + 1);
  strcpy(buf, text);
  if (asm64_assemble(buf, 0, 0x1000, 0, out, &str_va, &data_va, &heap_va))
    return 1;
  FILE *f = fopen(out_path, "wb");
  if (!f)
    return 1;
  for (int i = 0; i < SEC_N; i++) {
    fwrite(out[i].buf, 1, out[i].n, f);
    for (uint64_t pad = (8 - out[i].n % 8) % 8; pad; pad--)
      fputc(0, f);
  }
  fclose(f);
  printf("asmtest %s\n", out_path);
  return 0;
}
