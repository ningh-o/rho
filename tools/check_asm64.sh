#!/bin/sh
# Development-time oracle for the in-tree arm64 assembler: assembles a file
# covering every instruction form the backend emits, once with rho's own
# assembler and once with the system assembler, and diffs the __text bytes.
# The system assembler is a test oracle only — the shipped toolchain never
# invokes it.
set -e
cd "$(dirname "$0")/.."

f=$(mktemp -d)
trap 'rm -rf "$f"' EXIT

# every operand shape emit_arm64 can print (minus adrp/add-pageoff, whose
# values are layout-dependent — an object file leaves them to the linker;
# the corpus executing real images is their referee); local labels only, so
# the system assembler resolves everything internally (no relocations)
cat > "$f/forms.s" <<'EOF'
.text
.globl _forms_entry
_forms_entry:
  sub x12, x29, #8
  sub sp, sp, #128
  sub x8, x8, x9
  str x8, [x12]
  str w9, [x8]
  str s0, [x8]
  str d0, [x12]
  ldr x8, [x12]
  ldr w8, [x12]
  ldr s0, [x12]
  ldr d0, [x12]
  ldrb w8, [x12]
  strb w9, [x8]
  ldrh w8, [x12]
  strh w9, [x8]
  ldrsb x8, [x12]
  ldrsh x8, [x12]
  ldrsw x8, [x12]
  stp x29, x30, [sp, #-16]!
  ldp x29, x30, [sp], #16
  mov x8, x12
  mov w8, w8
  mov x8, #63
  mov w11, #7
  mov x29, sp
  mov sp, x29
  movz x8, #52429
  movz w8, #52429
  movk x8, #13399, lsl 32
  movk w8, #15820, lsl 16
  cmp x8, x9
  cmp x9, #-1
  cset x8, eq
  cset x9, vs
  cset x8, hs
  cset x8, lo
  cset x8, hi
  cset x8, ls
  cset x8, ge
  cset x8, lt
  cset x8, gt
  cset x8, le
  cbz x8, Lback
  cbnz x9, Lback
  b Lback
  b.ne Lfwd
  b.ge Lfwd
  bl Lfwd
  blr x17
  ret
Lback:
  mul x8, x8, x9
  msub x8, x8, x9, x10
  sdiv x8, x10, x9
  udiv x8, x10, x9
  and x8, x8, x9
  eor x8, x8, x9
  orr x8, x8, x9
  neg x8, x8
  lsrv x8, x8, x9
  lslv x8, x8, x9
  asrv x8, x8, x9
  sxtw x8, w8
  uxtw x8, w8
  uxtb x8, w8
  fadd d0, d0, d1
  fsub d0, d0, d1
  fmul d0, d0, d1
  fdiv d0, d0, d1
  fadd s0, s0, s1
  fcmp d0, d1
  fcmp s0, s1
  fcvt d0, s0
  fcvtzs x8, d0
  fmov x8, d0
  fmov d0, x8
  fmov s0, w8
  svc #0x80
Lfwd:
.section __TEXT,__rhostr,regular
  .p2align 3
Lstr0:
  .quad 0x8000000000000000
  .quad 0
  .quad 0
Lstr0_b:
  .ascii "hello\012\000"
  .byte 0
.section __DATA,__data
  .p2align 2
Ldata:
  .quad 0
  .byte 0
EOF

as -arch arm64 -o "$f/forms.o" "$f/forms.s"
# lines look like " 0000 c0035fd6 .._.." — skip the address column, keep
# the hex groups, stop at the ascii column
objdump -s --section=__text "$f/forms.o" |
  awk '{ for (i = 2; i <= NF; i++) if ($i ~ /^[0-9a-f]+$/) printf "%s", $i; else break }' \
  > "$f/apple.hex"
./build/rho-boot asmtest "$f/forms.s" "$f/forms.mine" >/dev/null

python3 - "$f" <<'PY'
import binascii, sys, os
f = sys.argv[1]
apple = binascii.unhexlify(open(f + "/apple.hex").read().strip())
mine = open(f + "/forms.mine", "rb").read()[: len(apple)]
if mine != apple:
    for i in range(0, len(apple), 4):
        a, m = apple[i : i + 4], mine[i : i + 4]
        if a != m:
            print(f"MISMATCH at instruction {i//4}: apple {a.hex()} mine {m.hex()}")
            sys.exit(1)
print(f"asm64 oracle ok: {len(apple)} bytes, byte-identical to the system assembler")
PY
