// rv32sim.c — minimal RV32IM simulator for the esp32c3 target.
//
// The flat image emitted by emit_riscv32.c loads at address 0 with the pc
// starting at 0. Machine contract (see spec §11.2 esp32c3):
//   entry state: a0 = heap base, a1 = heap size, sp = STACK_TOP
//   sw to 0x80000000: byte to host stdout
//   sw to 0x80000004: process exit code (machine stops)
//   stores to any other unmapped address: fault

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_TOP 0x01000000u
#define HEAP_BASE 0x00100000u
#define HEAP_SIZE 0x00800000u
#define SEMI_CHAR 0x80000000u
#define SEMI_EXIT 0x80000004u

typedef struct {
  uint32_t regs[32];
  uint32_t pc;
  uint8_t *mem;
  uint32_t mem_size;
  int exited;
  int exit_code;
  uint64_t steps;
  uint32_t hist_pc[64];
  uint32_t hist_sp[64];
  int hist_n;
  uint32_t calls[256];
  int calls_n;
} Sim;

static void sim_call(Sim *s, uint32_t ret) {
  if (s->calls_n < 256) {
    s->calls[s->calls_n] = ret;
    s->calls_n++;
  }
}

static void hist_push(Sim *s) {
  s->hist_pc[s->hist_n] = s->pc;
  s->hist_sp[s->hist_n] = s->regs[2];
  s->hist_n = (s->hist_n + 1) % 64;
}

static uint32_t load32(Sim *s, uint32_t addr) {
  uint32_t v = 0;
  if (addr + 3 < s->mem_size)
    memcpy(&v, s->mem + addr, 4);
  return v;
}

static void store32(Sim *s, uint32_t addr, uint32_t v) {
  if (addr + 3 < s->mem_size)
    memcpy(s->mem + addr, &v, 4);
}

static uint32_t load16(Sim *s, uint32_t addr) {
  uint32_t v = 0;
  if (addr + 1 < s->mem_size) {
    v = s->mem[addr];
    v |= (uint32_t)s->mem[addr + 1] << 8;
  }
  return v;
}

static void store16(Sim *s, uint32_t addr, uint16_t v) {
  if (addr + 1 < s->mem_size) {
    s->mem[addr] = (uint8_t)v;
    s->mem[addr + 1] = (uint8_t)(v >> 8);
  }
}

static uint8_t load8(Sim *s, uint32_t addr) {
  if (addr < s->mem_size)
    return s->mem[addr];
  return 0;
}

static void store8(Sim *s, uint32_t addr, uint8_t v) {
  if (addr < s->mem_size)
    s->mem[addr] = v;
}

static int32_t sext(uint32_t v, int bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

// run the image until semihost exit or step budget; returns exit code and
// writes 1 to *faulted when the machine died another way
int sim_run(unsigned char *image, uint32_t image_size, uint32_t entry,
                   uint32_t heap_base, uint32_t heap_size, int *faulted) {
  uint32_t mem_size = STACK_TOP + 0x10000u; // RAM covers the descending stack
  Sim s;
  memset(&s, 0, sizeof(s));
  s.mem = calloc(1, mem_size);
  if (!s.mem) {
    fprintf(stderr, "rho: sim out of memory\n");
    *faulted = 1;
    return 1;
  }
  s.mem_size = mem_size;
  if (image_size > mem_size)
    image_size = mem_size;
  memcpy(s.mem, image, image_size);
  memset(s.regs, 0, sizeof(s.regs));
  s.regs[2] = STACK_TOP;      // sp
  s.regs[8] = STACK_TOP;      // s0 frame pointer base
  s.regs[10] = heap_base;     // a0
  s.regs[11] = heap_size;     // a1
  s.pc = entry;
  *faulted = 0;

  while (!s.exited) {
    if (s.pc + 3 >= s.mem_size || (s.pc & 3)) {
      fprintf(stderr, "rho: sim pc fault at 0x%08x\n", s.pc);
      *faulted = 1;
      break;
    }
    uint32_t ins = load32(&s, s.pc);
    uint32_t next = s.pc + 4;
    if (getenv("RHO_SIM_TRACE"))
      fprintf(stderr, "pc=%08x ins=%08x sp=%08x s0=%08x t0=%08x t2=%08x a0=%08x\n",
              s.pc, ins, s.regs[2], s.regs[8], s.regs[5], s.regs[7], s.regs[10]);
    uint32_t op = ins & 0x7f;
    uint32_t rd = (ins >> 7) & 31;
    uint32_t f3 = (ins >> 12) & 7;
    uint32_t rs1 = (ins >> 15) & 31;
    uint32_t rs2 = (ins >> 20) & 31;
    uint32_t f7 = (ins >> 25) & 127;
    uint32_t a = s.regs[rs1];
    uint32_t b = s.regs[rs2];
    switch (op) {
    case 0x37: { // LUI
      if (rd) s.regs[rd] = ins & 0xfffff000u;
      break;
    }
    case 0x17: { // AUIPC
      if (rd) s.regs[rd] = s.pc + (ins & 0xfffff000u);
      break;
    }
    case 0x6f: { // JAL
      uint32_t imm = ((ins >> 31) & 1) << 20 | ((ins >> 12) & 0xff) << 12 |
                     ((ins >> 20) & 1) << 11 | ((ins >> 21) & 0x3ff) << 1;
      if (rd) s.regs[rd] = s.pc + 4;
      if (rd == 1)
        sim_call(&s, s.pc + 4);
      next = s.pc + sext(imm, 21);
      break;
    }
    case 0x67: { // JALR
      int32_t imm = sext(ins >> 20, 12);
      uint32_t target = (a + (uint32_t)imm) & ~1u;
      if (rd == 0 && s.calls_n > 0)
        s.calls_n--; // return
      if (target == 0) {
        // nothing in the image may return to 0 — the entry stub exits via
        // semihost, never by returning; treat this as a fatal fault
        fprintf(stderr, "sim: jalr to 0 at pc=%08x ra=%08x sp=%08x s0=%08x t0=%08x steps=%llu\n",
                s.pc, s.regs[1], s.regs[2], s.regs[8], s.regs[5],
                (unsigned long long)s.steps);
        fprintf(stderr, "sim: image[0..8] = %08x %08x\n", load32(&s, 0), load32(&s, 4));
        fprintf(stderr, "sim: call stack:\n");
        for (int i = s.calls_n - 1; i >= 0 && i > s.calls_n - 24; i--)
          fprintf(stderr, "  called from %08x\n", s.calls[i]);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      if (rd) s.regs[rd] = s.pc + 4;
      if (rd == 1)
        sim_call(&s, s.pc + 4); // a call; returns pop via jalr zero,0(ra)
      next = target;
      break;
    }
    case 0x63: { // branches
      uint32_t imm = ((ins >> 31) & 1) << 12 | ((ins >> 7) & 1) << 11 |
                     ((ins >> 25) & 0x3f) << 5 | ((ins >> 8) & 0xf) << 1;
      int take = 0;
      switch (f3) {
      case 0: take = a == b; break;
      case 1: take = a != b; break;
      case 4: take = (int32_t)a < (int32_t)b; break;
      case 5: take = (int32_t)a >= (int32_t)b; break;
      case 6: take = a < b; break;
      case 7: take = a >= b; break;
      default:
        fprintf(stderr, "rho: sim bad branch f3=%u at 0x%08x\n", f3, s.pc);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      if (take)
        next = s.pc + sext(imm, 13);
      break;
    }
    case 0x03: { // loads
      int32_t imm = sext(ins >> 20, 12);
      uint32_t addr = a + (uint32_t)imm;
      uint32_t v;
      if (addr >= s.mem_size) {
        // no MMIO reads exist in this machine: an out-of-range load is a
        // wild pointer, not data — fault instead of returning fabric
        fprintf(stderr, "rho: sim load fault at pc=0x%08x addr=0x%08x\n", s.pc, addr);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      switch (f3) {
      case 0: v = (uint32_t)(int32_t)(int8_t)load8(&s, addr); break;
      case 1: v = (uint32_t)(int32_t)(int16_t)load16(&s, addr); break;
      case 2: v = load32(&s, addr); break;
      case 4: v = load8(&s, addr); break;
      case 5: v = load16(&s, addr); break;
      default:
        fprintf(stderr, "rho: sim bad load f3=%u at 0x%08x\n", f3, s.pc);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      if (rd) s.regs[rd] = v;
      break;
    }
    case 0x23: { // stores
      int32_t imm = sext(((ins >> 25) & 0x7f) << 5 | ((ins >> 7) & 0x1f), 12);
      uint32_t addr = a + (uint32_t)imm;
      if (addr == SEMI_CHAR) {
        putchar((int)(b & 0xff));
        fflush(stdout);
        break;
      }
      if (addr == SEMI_EXIT) {
        s.exited = 1;
        s.exit_code = (int)(b & 0xff);
        break;
      }
      static uint32_t watch = 0xFFFFFFFFu;
      if (watch == 0xFFFFFFFFu) {
        const char *w = getenv("RHO_SIM_WATCH");
        watch = w ? (uint32_t)strtoul(w, NULL, 0) : 0xFFFFFFFFu;
      }
      if (addr == watch)
        fprintf(stderr, "sim: watch store pc=%08x val=%08x\n", s.pc, b);
      if (addr >= s.mem_size) {
        // non-semihost store above RAM: wild pointer — fault, don't drop
        fprintf(stderr, "rho: sim store fault at pc=0x%08x addr=0x%08x\n", s.pc, addr);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      switch (f3) {
      case 0: store8(&s, addr, (uint8_t)b); break;
      case 1: store16(&s, addr, (uint16_t)b); break;
      case 2: store32(&s, addr, b); break;
      default:
        fprintf(stderr, "rho: sim bad store f3=%u at 0x%08x\n", f3, s.pc);
        *faulted = 1;
        s.exited = 1;
        break;
      }
      break;
    }
    case 0x13: { // OP-IMM
      int32_t imm = sext(ins >> 20, 12);
      uint32_t v = 0;
      switch (f3) {
      case 0: v = a + (uint32_t)imm; break;
      case 2: v = (int32_t)a < imm ? 1 : 0; break;
      case 3: v = a < (uint32_t)imm ? 1 : 0; break;
      case 4: v = a ^ (uint32_t)imm; break;
      case 6: v = a | (uint32_t)imm; break;
      case 7: v = a & (uint32_t)imm; break;
      case 1: v = a << (rs2 & 31); break;
      case 5:
        // srli f7=0x20, srai f7=0x60 — the deciding bit is imm[11] (bit31),
        // NOT bit30, which both variants set
        if (f7 & 0x40)
          v = (uint32_t)((int32_t)a >> (rs2 & 31));
        else
          v = a >> (rs2 & 31);
        break;
      }
      if (rd) s.regs[rd] = v;
      break;
    }
    case 0x33: { // OP
      uint32_t v = 0;
      if (f7 == 1) { // M extension
        int64_t sa = (int32_t)a, sb2 = (int32_t)b;
        switch (f3) {
        case 0: v = a * b; break;
        case 1: v = (uint32_t)(((sa * sb2) >> 32) & 0xffffffffu); break;
        case 2: v = (uint32_t)(((sa * (uint64_t)b) >> 32) & 0xffffffffu); break;
        case 3: v = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
        case 4:
          if (b == 0) v = 0xffffffffu;
          else if (a == 0x80000000u && b == 0xffffffffu) v = 0x80000000u;
          else v = (uint32_t)((int32_t)a / (int32_t)b);
          break;
        case 5: v = b == 0 ? 0xffffffffu : a / b; break;
        case 6:
          if (b == 0) v = a;
          else if (a == 0x80000000u && b == 0xffffffffu) v = 0;
          else v = (uint32_t)((int32_t)a % (int32_t)b);
          break;
        case 7: v = b == 0 ? a : a % b; break;
        }
      } else {
        switch (f3) {
        case 0: v = (f7 & 0x20) ? a - b : a + b; break;
        case 1: v = a << (b & 31); break;
        case 2: v = (int32_t)a < (int32_t)b ? 1 : 0; break;
        case 3: v = a < b ? 1 : 0; break;
        case 4: v = a ^ b; break;
        case 5: v = (f7 & 0x20) ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
        case 6: v = a | b; break;
        case 7: v = a & b; break;
        }
      }
      if (rd) s.regs[rd] = v;
      break;
    }
    case 0x0f: break; // FENCE: nop
    case 0x73: { // ECALL/EBREAK
      fprintf(stderr, "rho: sim trap at 0x%08x\n", s.pc);
      *faulted = 1;
      s.exited = 1;
      break;
    }
    default:
      fprintf(stderr, "rho: sim unknown opcode 0x%02x at 0x%08x\n", op, s.pc);
      *faulted = 1;
      s.exited = 1;
      break;
    }
    hist_push(&s);
    s.pc = next;
    s.steps++;
    if (s.steps > 4000000000ull) {
      fprintf(stderr, "rho: sim step budget exhausted\n");
      *faulted = 1;
      break;
    }
  }
  int code = s.exit_code;
  free(s.mem);
  return code;
}
