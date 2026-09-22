# todo

Living backlog. This file holds only what is still undone. The wasm
bootstrap is closed (its closing measurements are recorded once, below);
boot is frozen by decision; the native backends are the current batch.

## The wasm bootstrap — closed, measured 2026-09-23

- `make test` → `selftest ok` (boot AST + diag goldens).
- `tools/gate.sh --wasm`: **5 green, 0 red** — boot-selftest,
  build-mirror, corpus-diff (**91 programs**: boot-built vs mirror-built
  artifacts of every `corpus/*.rho` run under wasmtime, stdout + exit
  identical), self-chain (**91 programs**: child, grandchild, and the
  grandchild rebuilding the whole corpus to the boot goldens),
  diag-parity (**28 cases**, messages identical to boot's).
- Differential fuzzer (`tools/fuzz/gen.mjs`, seeded LCG): **150/150
  seeds identical behavior**, 0 differences, 0 both-invalid,
  0 both-reject (13.3s wall clock of the 900s budget).
- The shipped compiler is the self-built chain: `make site` produces
  `site/assets/rho.wasm` (boot compiles `self/rho.rho`), byte-identical
  to `build/rho.wasm` and to the gate's `build/gate/m.wasm`
  (SHA-256 `c5dd9fc7…`, 3,975,646 bytes). Verified under wasmtime: it
  answers `--version` (`rho 0.4.0`), compiles `corpus/001_hello.rho`
  (`built`), and the built program prints `hello, world` (exit 0).
  wasi-sdk is retired — no C compiler takes part in any shipped
  artifact anymore.

## Boot freeze — decided

- `boot/src` is frozen at 0.4.0 + triple-quote and ships ONE target,
  `wasm32-wasi`. A native target argument is refused with a clear
  error, exit 2 (measured: `rho: unsupported target: arm64-mac (boot
  ships wasm only; native backends live in the self-hosted
  compiler)`). fmt and the native backends live in the self-hosted
  compiler only.
- Triple-quoted multiline strings landed on BOTH ends in the same
  round with the same fully-literal semantics (boot's lexer + the
  mirror; `corpus/031_multiline.*` pins the shared behavior, the
  language tests roundtrip it byte for byte) — the last feature the
  two ends ever had to land together.
- The feature subset law stands and is ENFORCED, not conventional:
  `self/rho.rho` may only use features boot already implements, and
  the gate's build-mirror leg is the enforcement — a mirror source
  that runs ahead of the seed simply fails to build, red gate.
  `docs/bootstrap.md` is the map: the seed chain, who builds which
  artifact, the feature-subset law, the rebuild commands.

## Native backends — the current batch (open)

All native code lives in the mirror (`self/rho.rho`); boot cannot emit
a native image at all. The gate's crossing legs are build-only — a
corrupt native image is never exec'd here (the kernel-wedge lore lives
in this file's git history). Measured 2026-09-23:

- `amd64-linux`: **GREEN** — the hello smoke (41,256 bytes) AND the
  full self-build (6,374,172 bytes).
- `arm64-mac`: RED — the hello smoke aborts (rc 134).
- `arm64-linux`: RED — the hello smoke aborts (rc 134).

The repair batch targets the arm64 pair first (mac + linux, aarch64),
then amd64 completes. Repro seats carried from the earlier rounds
(full notes in git history): the emit-phase O(n²) offset-table build
(`ac_layout`'s growing `e.offs`; the pre-sized fix is written but
parked until the next item falls), the loop-carried field-reassignment
lowering bug (`e.offs = push_i64(…)` inside `while` — surfaces as
`ac_slot_off#N index out of bounds` within the first ~25 functions of
an arm64 self-build), the assembler's whole-text `asm_split_lines`
memory ceiling, and the mac image's fixed heap segment (~1 GB baked
into the image; either size it from the target's need or mmap-grow it
in the mac RT blob).

## Later batches

- Native real-machine exec verification: the gate never runs a native
  image; per-target exec happens on real hardware / in a container
  once the crossings are green.
- In-container corpus: the corpus differential re-run inside a Linux
  container, per native target.
- GitHub release: owner-decided NOT this round — the release (and the
  zips carrying self-built native clients) waits until the native wave
  lands.
- Package-manager polish: argv instead of stdin (`__program_args`
  works on wasm, measured), native pkg targets after the crossings.

## 0.2 backlog (unchanged)

- linear-scan register allocator (replaces spill-everything)
- element-wise `==` on structs/arrays/slices (spec §4.2 defers to
  self-host)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)

## Standard library

Roadmap: `docs/stdlib-roadmap.md`.

## Owner's call

- The repo root has no LICENSE file (`tools/lsp` and
  `tools/vite-plugin-rho` declare MIT). Adding one is the owner's
  call, not the tree's.
