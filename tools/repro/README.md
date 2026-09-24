# tools/repro — durable repros for open defects

Open items in docs/todo.md reference these; they lived under build/
first (ephemeral) and were pinned here so a `make clean` cannot lose
them. Each file notes the defect it repros and the two ends' verdicts;
all were minimized 2026-09-24.

- `arm64-map-n24-literals.rho` — the arm64 managed-map miscompile,
  semantic form: 24 short literal keys through an FNV+linear-probe map.
  wasm `n=24`, native `n=13` (deterministic). N=23 passes.
- `arm64-map-n24-heapkeys.rho` — same map, `"key-" + k.to_str()` heap
  keys: native SIGSEGVs at the 32→64 rehash (boundary N=23/24).
- `fold-shift-width-seed46.rho` — the comptime fold's shift-width
  family (fuzz seed 46): `86 <= (61 << 28)` folds TRUE, runs FALSE.
- `fold-static-vs-if-divergence.rho` — the probe that pins the seed-46
  root: the SAME expression folds FALSE as a static initializer and
  TRUE as an if condition in one binary (compare-typing retypes an
  INT_LIT operand where boot does not).

Usage: build each with `wasmtime run --dir . build/gate/m.wasm build
<file> --target wasm32-wasi|arm64-mac -o out` and diff the two ends'
behavior against the notes above.
