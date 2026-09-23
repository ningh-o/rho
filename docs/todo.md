# todo

Living backlog. This file holds only what is still undone. The wasm
bootstrap is closed (its closing measurements are recorded once, below);
boot is frozen by decision; the native wave, the optimizer and
element-wise aggregate equality are in.

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

## The optimizer — landed 2026-09-23, tree-shaking completed same day

`libs/compiler/opt.rho` (spec §3.6): constant folding + dead-instruction
elimination, mirror-only, on by default (`--no-opt` disables). The gate's
differential legs are the referee (92 programs × boot vs mirror, plus
the grandchild chain), and `tests/lang/opt/` pins the folds themselves
(12 cases: arith chains, wrap edges at both widths, shift masking +
i64/u64 lanes, comparison folding, divisor-0 trap survival, the signed
−1 wrap law, the const-lane cast corner, the i64-min dogfood, fn
tree-shaking — s07 caught the first cut of the cast folds diverging
from the runtime and narrowed them to the two evidenced shapes: integer
bitcopy, and sext i32→i64 — and s10–s12 pin the GLOBAL shake: dead,
public-dead, module-dead and orphaned statics never enter the artifact,
asserted against the dump's `^global ` list). The differential fuzzer
re-ran with the optimizer live: **1000/1000 seeds identical behavior**
(seeds 1..1000, 96 s), and again after the global shake (87.6 s, 0
differences). Artifact effect: −2.1% total bytes across the corpus
programs (small programs are prelude-dominated; constant-heavy user
code gains more), −1.4% on the compiler's own 4.2 MB self-build — and
the pass costs ~0.5% compile time (12.70 s vs 12.64 s for the full
self-build, noise-level).

The reachability walk now covers globals too (spec §11.3): unreferenced
statics are pure dead data (rho statics are const-initialized; nothing
runs before main), so their data segments, bss slots and attached
string-literal blocks all go. Keeping is transitive across both maps —
a kept global's reloc words can name fns and other globals, a kept
fn's callees can name either. A wholly-unused `use`d module costs
nothing.

## The emitter half-diamond — fixed 2026-09-23

The eq-walk's variant ladder exposed a historical wasm-emitter bug: a
cbr whose then-arm chain continues into the ELSE target emitted that
target inline after a depth-0 br (dead), so **every enum variant rung
after the first compared nothing** — same-tag values of later payload
variants read equal. Corpus 058/098 pinned the quirk as shared boot/
mirror behavior; the mirror now nests a `block $else-target` around
the if (both arms br to it, its code follows) and the pins moved to
`tests/lang/eq/e04_enum_ladder.rho`. Boot, frozen, keeps the quirk —
the two compilers disagree there by design, and the corpus records
only what they agree on. The half-diamond check must stay linear
(`arm_falls_into`, ≤8 steps): chain_terminal's recursive walk on every
cbr made a self-build take 5+ CPU minutes (13 s is the bar).

## Element-wise aggregate == — landed 2026-09-23

Spec §4.2's deferred feature, now in the self-hosted compiler (boot
still rejects it, frozen): `==`/`!=` on structs, fixed arrays and
slices compare element-wise through memoized `rho__eq$<t>` helpers —
slices compare lengths first, then elements; struct/enum FIELDS now
recurse properly (string fields used to compare one raw word — a
pointer). Every participating type must itself be comparable (ints,
floats, bools, strings, pointers, weak handles, and the aggregate
kinds recursively; `fn`/`dyn`/`err` are rejected with a diagnostic).
Recursive containers lower to mutually recursive helpers, terminating
on acyclic data. `tests/lang/eq/` (6 cases) is the pin — mirror-only,
like every language feature past the freeze. Fixed arrays have no
user-visible construction path yet, so their eq support is compiled
but untested end to end; slices and structs are fully covered.

## Native real-machine exec — arm64-mac measured 2026-09-23

`tools/native-exec.sh` (deliberately OUTSIDE the gate — a corrupt image
can wedge the kernel, so the gate stays build-only): builds each
program to wasm and to arm64-mac, runs the wasm under wasmtime as the
behavioral baseline, then execs the native image directly on this
Apple-Silicon machine (fresh mktemp path per image, alarm-capped, exec
only after a clean build). **18 ok, 1 known** across a representative
corpus slice plus the whole eq suite — the eq$ helpers run correctly
natively. Two real assembler/emitter bugs fell on the way:

1. **Missing mnemonics** — `sxtb`/`sxth`/`uxth` (SBFM/UBFM aliases):
   corpus 042's i8/i16 signed corners were unbuildable natively; the
   crossings never emit them.
2. **W-form data-processing encoded as X-form** — `ar_dprec2`/
   `ar_dprec3` hardcoded sf=1, so `sdiv w8, w10, w9` assembled as a
   64-bit SDIV over operands a W load had just zero-extended: −7/2
   read as 4294967289/2 = 2147483644. Every W-lane sdiv/udiv/lslv/
   lsrv/asrv/madd/msub rode the same encoders. Divmod also gained the
   W lane itself (it loaded 8-byte zero-extending operands and divided
   at 64 bits regardless of the operand type).

`rho build --target <native>` now also writes the assembly sidecar
(`<out>.s`) — the toolchain never shells out, so it is the only way to
read what a backend emitted.

### Known: corpus 081 SIGSEGVs natively (pre-existing)

`corpus/081_generics_bounds.rho` crashes (rc 139) natively — the
pre-change mirror reproduces it, so it predates everything above.
Minimal repro (the tail alone or the head alone both pass; the
COMBINATION crashes at main's epilogue after both lines print):

```rho
fn main() -> i32 {
  printf("first={}\n", 5);
  let tags: []string = make([]string, 3);   // []i64 is fine
  printf("x={}\n", 3);
  return 0;
}
```

Diagnosis so far: the fault is inside an rc helper (`rho__rc_inc`/
`rc_dec` — the `>> 63` sign check of the count) called with **x0 =
0x100000000** — the image's own base, page-aligned, i.e. a leaked ADRP
result that reached a pointer slot; the value rides a stack slot into
the call (all 7 rc call sites in the repro's asm load x0 correctly, so
the corruption happens earlier — suspect the variadic-call
materialization or the []string make/zero-walk interacting with heap
state; the .s sidecar + `lldb -o 'settings set target.disable-aslr
false'` reproduce it deterministically). wasm-side behavior is
correct; `KNOWN` in tools/native-exec.sh carries it — remove that
entry when it falls.

## Later batches

- Native 081: the known arm64 SIGSEGV above (the only red in
  native-exec) — diagnosed to a leaked ADRP page address reaching an
  rc helper's argument.
- amd64-linux / arm64-linux exec: needs a container or matching
  hardware; the crossings stay build-only until then.
- In-container corpus: the corpus differential re-run inside a Linux
  container, per native target.
- GitHub release: owner-decided NOT this round — the release (and the
  zips carrying self-built native clients) waits until the native wave
  lands.
- Bench the shipped compiler: the suite's rho lane builds with the boot
  seed; running it through the mirror (optimizer on) alongside is a
  methodology decision plus a full re-record — owner's call.

## 0.2 backlog (unchanged except where struck)

- ~~element-wise `==` on structs/arrays/slices~~ — landed 2026-09-23
  (see above).
- linear-scan register allocator (replaces spill-everything)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)

## Standard library

Roadmap: `docs/stdlib-roadmap.md`.

## Owner's call

- The repo root has no LICENSE file (`tools/lsp` and
  `tools/vite-plugin-rho` declare MIT). Adding one is the owner's
  call, not the tree's.
