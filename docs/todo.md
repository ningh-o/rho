# todo

Living backlog. This file holds only what is still undone. The wasm
bootstrap is closed (its closing measurements are recorded once, below);
boot is frozen by decision; the native wave and the optimizer are in.

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
  `site/assets/rho.wasm` (boot compiles `libs/compiler/main.rho`), byte-identical
  to `build/rho.wasm` and to the gate's `build/gate/m.wasm`
  (SHA-256 `c5dd9fc7…`, 3,975,646 bytes, at closure; now
  `dc8e25ea…`, 4226060 bytes — cat retired to `+`, the optimizer aboard, the
  root double-lowering gone, the HEAP@ scratch prints removed).
  Verified under wasmtime: it
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
  `libs/compiler/` may only use features boot already implements, and
  the gate's build-mirror leg is the enforcement — a mirror source
  that runs ahead of the seed simply fails to build, red gate.
  `docs/bootstrap.md` is the map: the seed chain, who builds which
  artifact, the feature-subset law, the rebuild commands.

## Native backends — closed, measured 2026-09-23

All native code lives in the mirror (`libs/compiler/`); boot cannot emit
a native image at all. The gate's crossing legs are build-only — a
corrupt native image is never exec'd here (the kernel-wedge lore lives
in this file's git history). `tools/gate.sh` full run: **11 green,
0 red** — every target's hello smoke AND full self-build:

- `arm64-mac`: hello + self-build (13,110,736-byte Mach-O).
- `arm64-linux`: hello + self-build.
- `amd64-linux`: hello + self-build.

What the batch actually was (three bugs, none of them the suspected
lowering bug):

1. **Unzeroed layout table** — `ac_layout` pre-sizes `e.offs` but rho's
   `make()` does not zero (the law; wasm's grow-zeroed pages only mask
   it until the allocator hands back reused memory). `ac_slot_off`
   reads 0 as "not cached", so a garbage nonzero slot read as a giant
   offset and the offset walk looped on it — gigabytes of emitted text,
   OOM, the arm64 rc-134. The same unzeroed-`make` bug also lived in
   the optimizer's const table (it folded a load as garbage).
2. **Root module lowered twice** — a sibling's `use main` registers the
   ROOT file as an aux module (separate Module object, same path);
   lowering both emitted the root's statics twice → duplicate labels in
   the native assemblers (and dead duplicate globals in every wasm
   artifact — the fix shrinks them all).
3. **`itoa(i64 MIN)`** — the negation wrapped back into itself and
   emitted garbage bytes, a comma among them, which split a `movabsq`
   immediate mid-number in the amd64 assembler feed.

The pre-seat notes (loop-carried field reassignment, asm_split_lines
ceiling, mac heap segment) never fired once these three fell; the
62.9 MB self-build assembly text rides the wasm32 heap fine today.

## The optimizer — landed 2026-09-23

`libs/compiler/opt.rho` (spec §3.6): constant folding + dead-instruction
elimination, mirror-only, on by default (`--no-opt` disables). The gate's
differential legs are the referee (92 programs × boot vs mirror, plus
the grandchild chain), and `tests/lang/opt/` pins the folds themselves
(9 cases: arith chains, wrap edges at both widths, shift masking +
i64/u64 lanes, comparison folding, divisor-0 trap survival, the signed
−1 wrap law, the const-lane cast corner, the i64-min dogfood, and
uncalled-fn tree-shaking — s07
caught the first cut of the cast folds diverging from the runtime and
narrowed them to the two evidenced shapes: integer bitcopy, and sext
i32→i64). The differential fuzzer
re-ran with the optimizer live: **1000/1000 seeds identical behavior**
(seeds 1..1000, 96 s, 2026-09-23). Artifact effect: −2.1% total bytes across the corpus programs (small
programs are prelude-dominated; constant-heavy user code gains more),
−1.4% on the compiler's own 4.2 MB self-build — and the pass costs
~0.5% compile time (12.70 s vs 12.64 s for the full self-build,
noise-level).

## Later batches

- Native real-machine exec verification: the gate never runs a native
  image; per-target exec happens on real hardware / in a container —
  the crossings are green, so this is unblocked.
- In-container corpus: the corpus differential re-run inside a Linux
  container, per native target.
- GitHub release: owner-decided NOT this round — the release (and the
  zips carrying self-built native clients) waits until the native wave
  lands.
- Package-manager polish: argv instead of stdin (`__program_args`
  works on wasm, measured), native pkg targets after the crossings.
- Bench the shipped compiler: the suite's rho lane builds with the boot
  seed; running it through the mirror (optimizer on) alongside is a
  methodology decision plus a full re-record — owner's call.

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
