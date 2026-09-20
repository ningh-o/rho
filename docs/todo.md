# todo

## 0.3.4 — format: the printf desugar pointed at a string sink

Boot-only (`__fmt_build` joins the prelude sinks; zero placeholders is the
literal itself). `to_str` bodies become one format line — corpus 029 pins
struct/enum/nested cases, and 024/028 now author their `to_str` bodies
through `format`. Mirror items, on top of the 0.3.3 set below:

1. checker: accept the `format` verb in the desugar block; rewrite to
   `__fmt_build(...)` (or `EX_STR` with zero placeholders).
2. prelude: `__fmt_build(parts: string...) -> string`.
3. the corpus file 029 and the diag d038.

## 0.3.3 — printf + variadics; the mirror set grows

Boot-only this round (`docs/print-and-printf.md` has the design): variadic
parameters (`rest: T...` — a `[]T` to the body, last parameter only,
concrete element type), spread calls (`xs...`), and `printf`/`eprintf` as
checker-rewritten builtins over the unchanged `to_str` protocol;
`print`/`println`/`eprint` are removed. Mirror items for the self-hosted
compiler, on top of the 0.3.x set below:

1. lex: the `...` token (three-char match before `..`).
2. parse: `T...` on the last named-`fn` parameter; `xs...` spread on the
   last call argument; fmt emits both (roundtrip-pinned).
3. checker: the variadic parameter wraps to `[]T` in the fn type; call
   sites check extra args against the element type (literal adaptation
   included); spread requires the exact slice type; the fn-value guard
   (no `...` in `fn(P) -> R`); the printf desugar — split the literal,
   diagnose stray braces and placeholder/value mismatch, rewrite to
   `__fmt_print`/`__fmt_eprint` (or the bare sink with zero placeholders),
   each value becoming `<v>.to_str()`.
4. checker: untyped-literal receivers resolve methods at the default width
   (i32/f64) — `5.to_str()` becomes legal source, not just desugar output.
5. lower: variadic call sites materialize the fresh slice (make + element
   stores with managed retains; post-call release through the per-type
   walker — slice values own their elements); spread passes the slice as
   an ordinary argument; the aggregate-return call path unifies through
   `build_call`.
6. the corpus files 023–028 and the diag set d030, d033–d037.

## 0.3.2 — native images land in boot; the self-hosted mirror follows

Boot now builds runnable images on every native target with zero external
tools — its own assemblers (asm64/asm86), its own ELF/Mach-O writers, its
own ad-hoc SHA-256 code signature:

- `arm64-mac`: static Mach-O via dyld (LC_MAIN into the freestanding RT
  blob). The kernel's bar, learned the hard way: 16K segment alignment,
  the PIE flag bit (0x200085, not 0x20085 — the latter is
  ALLOW_STACK_EXECUTION and is killed on sight), a parseable
  DYLD_CHAINED_FIXUPS blob (`imports_format` must be 1 even with zero
  imports — dyld rejects 0 as "unknown imports format"),
  LC_DYLD_EXPORTS_TRIE at 0x80000033 (0x80000038 is unknown to this
  dyld), no segment overlaps (the BSS heap is its own trailing
  S_ZEROFILL segment at VM_BASE+0x1000000), and the file ending exactly
  at dataoff+datasize (anything after the signature fails strict
  validation).
- `amd64-linux` / `arm64-linux`: static ELF64 (ET_EXEC, no interpreter,
  no libc, raw syscalls — freestanding by choice; libc stays allowed if
  a future target wants it), 0x400000 base, 232-byte header block,
  entry = first text byte (the RT blob).

Boot-side fixes this round: asm86 was a first-draft assembler whose whole
dialect needed the pinning the corpus + a GNU-as oracle gave: percent
stripping in register parses, AT&T operand order in the ALU/mov families,
REX.W only for genuine 64-bit movs, fixups recorded after opcode bytes
(jmp/jcc/call were overwriting their own opcodes), jz/jnz aliases, the
`.long` directive (f32 constant pools silently collapsed without it), the
sar/sal/shr digit table (all three pairwise confusions produced real
wrong-code: `1 << 10` ran as sar, u64 `>> 63` kept the sign), the movsx
opcode table (byte/word/long extensions were rotated — fmt_digits hung in
a loop on a clobbered digit), and the cvttSS/cvttsD prefix discriminator
(the width letter is [5], not [4] — `Circle(6.5) as i64` returned 0).
Also: emit_amd64's float compares picked their width from the CMP's
i->size instead of the operand type (an f64 compare rode movss/ucomiss —
only executable code caught it; amd64-mac was .s-only and never ran);
asm64 learned the standalone `.text`/`.data` directives and a data page
that starts past the whole r-x image; emit_arm64 unifies `@PAGE/@PAGEOFF`
across dialects and aligns the main trampoline; arm64-linux write is
syscall 64 (asm-generic table — 1 is io_destroy).

Two latent front-end gaps the round surfaced (corpus-green on all four
targets, so deferred to the mirror set): the constant folder masks shift
counts with 31 even for i64 operands (`1 << 40` folds to 256), and a
float LITERAL narrowed with `as i32` lowers wrong on wasm32 (validation
error) — runtime-value casts are fine.

Known-open boot bug: one more wasm32 structured-control gap — the
mirror still fails wasmtime validation in one function ("control frames
remain at end of function body", around the dump/render-diags fns).
That plus the rest of the mirror set below gates the bootstrap.

## 0.3.x — mirror the 0.3.0 language work into the self-hosted compiler

The bootstrap gate (`boot(self) == self(self)`, byte for byte) is
INTENTIONALLY OPEN until this lands; `boot` remains the reference
toolchain (the site and corpus pin its binary). The mirror set:

1. checker: accept `self: <primitive>` methods; register them in a global
   per-primitive table with the duplicate-method error; resolve method
   calls on primitive receivers (auto-deref through pointers); literal
   receiver adaptation in method-arg checking.
2. symbol mangling: methods fold the receiver name into the symbol
   (`rho_<mod>__<recv>_<name>`) — required for `to_str` per type.
3. lower: the post-IR reachability pass (drop unreachable fns; keep entry,
   `__panic_div`; preserve emission order).
4. wasm backend: `i32.wrap_i64` on runtime `memory.copy` lengths; the
   `emit_region_run` fix — an arm that begins at a claimed join must br
   (its phi edge copies), not fall through silently (the `&&`-stale-result
   bug).
5. arm64 backend: f32 rides s-registers everywhere (loads, spills, consts,
   args, params, returns); FCONST materializes FLOAT bits for f32;
   ZEXT zero-extends; I2F loads narrow ints per signedness (ucvtf for
   u64); F2I by widths.
6. amd64 backend: movss/movsd by width in the slot helpers; f32 const
   pool (`.long` float bits); f32 args/params/returns; the u64→float
   unsigned convert sequence; F2I 32-bit saturating form.
7. literal defaults: unadapted INT_LIT → i32, FLOAT_LIT → f64 in
   ir_type_of; unary minus forwards the context type to a literal operand
   and negates floats via a float zero.
8. the 0.3.2 image layer, ported to rho: asm64 + asm86 (including the
   page-parameter layout and the heap override), elf64 + macho64 + the
   in-tree SHA-256, the per-target RT blobs, and the build/run/test
   dispatch for the image targets. The mirror's own externs (malloc/
   open/read/write/popen/…) move from libSystem to RT-blob syscall
   shims — the self compiler runs freestanding too; argv comes from the
   LC_MAIN entry registers, captured by the blob.
9. after all of the above: re-run the twice-compile gate on arm64-mac +
   wasm and `boot(corpus) == self(corpus)`.

## 0.3.1 — the esp32c3 backend was removed instead of fixed

0.3.0 left one known esp32c3 bug (float print corrupting a later soft-float
divide; investigation notes in git history) and a backend that had only
ever run on the in-tree simulator. Rather than chase it, 0.3.1 removes the
whole RV32IM/esp32c3 backend — and, in the same stroke, the toolchain
stops shelling out to the system `cc`: native targets emit assembly and
stop, `wasm32-wasi` is the one end-to-end target, and `run`/`test`
execute through a WASI runtime (wasmtime). If the target ever returns it
starts from the 0.2.x git history, with real hardware as the bar.

## 0.2 backlog (unchanged)

- linear-scan register allocator (replaces spill-everything)
- element-wise `==` on structs/arrays/slices (spec §4.2 defers to self-host)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)
