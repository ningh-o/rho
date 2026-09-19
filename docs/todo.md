# todo

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
   `__panic_div`, the esp32 soft-float set; preserve emission order).
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
7. esp32 backend: f64 params spill as register pairs (the prologue pair
   case must include TY_F64).
8. literal defaults: unadapted INT_LIT → i32, FLOAT_LIT → f64 in
   ir_type_of; unary minus forwards the context type to a literal operand
   and negates floats via a float zero.
9. after all of the above: re-run the twice-compile gate on arm64 + wasm
   and `boot(corpus) == self(corpus)`.

## 0.3.1 — esp32c3: float print corrupts a later soft-float divide

`println(2.5)` on esp32c3 prints correctly, but a subsequent `f64`
division miscompiles (`4.0 / 2.0` reads as inf). Facts established
(2026-09-20, boot 0.3.0 tree):

- The division VALUE is correct when stored and compared without printing
  (`if e / f == 1.6` passes with no prior float print, fails after one).
- The trigger is the first `println` of a float — the `__fmt_digits` bigint
  path (`f64.to_str` → `__fmt_digits`, ~7.3KB frame) — not allocation per
  se (800-byte `make` before a divide is harmless), and not the shallow
  path (`let s = 5.0.to_str(); print(s);` is harmless).
- `__fmt_digits`, the runtime stubs, and the call sites compile
  byte-identically to a working HEAD build apart from label numbering;
  `main`'s asm at both call sites is structurally correct; the RV32
  prologue/epilogue are balanced; slot addressing handles >2048 offsets.
- Suspects remaining: the in-tree simulator's handling of the deep frame
  chain, or a register/stack clobber in the aggregate-return path
  (`println` → `f64.to_str` returns a 24-byte string through a caller-made
  out slot) — a simulator watchpoint run is the next step.

Until fixed: `rho test corpus --target esp32c3` fails `025_fmt_floats`
only; amd64-linux, arm64-mac and wasm32-wasi run the full corpus green.

## 0.2 backlog (unchanged)

- linear-scan register allocator (replaces spill-everything)
- element-wise `==` on structs/arrays/slices (spec §4.2 defers to self-host)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)
