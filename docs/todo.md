# todo

Living backlog. This file holds only what is still undone. The wasm
bootstrap is closed (its closing measurements are recorded once, below);
boot is frozen by decision; the native wave, the optimizer, element-wise
aggregate equality, tail-call optimization, the wasm-only web root, the
pinned seed, and the containment check are in.

## The wasm bootstrap — closed, measured 2026-09-23

- `make test` → `selftest ok` (boot AST + diag goldens).
- `tools/gate.sh --wasm`: **7 green, 0 red** — boot-selftest,
  build-mirror, corpus-diff (**92 programs**: boot-built vs mirror-built
  artifacts of every `corpus/*.rho` run under wasmtime, stdout + exit
  identical), self-chain (**92 programs**: child, grandchild, and the
  grandchild rebuilding the whole corpus to the boot goldens),
  seed-chain (the pinned seed builds the mirror — byte-identical to the
  chain's child today), web-root (the wasm-only root builds, runs hello,
  refuses native targets), diag-parity (**28 cases**, messages identical
  to boot's).
- Differential fuzzer (`tools/fuzz/gen.mjs`, seeded LCG): **300/300
  seeds identical behavior** with the optimizer AND the tail-call pass
  live (2026-09-23; 1000/1000 in the two rounds before them).
- The shipped compiler is the self-built chain: `make site` produces
  `site/assets/rho.wasm` from **`libs/compiler/web.rho`** — the
  wasm-only package root, one megabyte lighter than the full root
  because reachability drops the six native backend modules (measured:
  3,379,067 vs 4,388,023 bytes). wasi-sdk is retired — no C compiler
  takes part in any shipped artifact anymore.

## The 2026-09-23 tail-call/roots/seed round

- **Tail-call optimization** (spec §3.6): a direct self-call in tail
  position becomes a loop — arguments pinned before the release tail,
  moved into the parameter locals after it, control back to a split
  entry. `tests/lang/opt` t01–t11 pin it (5M-deep recursion in constant
  stack, string/ptr/slice/f64 lanes, two tail sites, void calls, the
  not-tail and defer controls, and a dump pin that the self-call left
  main's artifact). The wasm emitter gained a trailing `unreachable`
  after the body walk: a label that received a br — a loop header
  included — validates as reachable at its `end`, so code after a
  closed loop can be reachable with nothing on the stack; the dead byte
  makes every such shape validate.
- **Two package roots**: `libs/compiler/full.rho` (every target, through
  `native/pipe.rho` — the native image pipeline moved there wholesale)
  and `libs/compiler/web.rho` (wasm-only; a native target is refused
  with the same clear-error law boot uses). The driver is `cli.rho`
  (was `main.rho` — renamed so the roots can define `fn main` without
  colliding with `use main`); a native request rides a sentinel
  (`RC_NATIVE` + two statics) from `build_to` to the root, which either
  finishes it through the pipe or refuses. Zero new language surface.
- **The pinned seed**: `boot/rho-seed.wasm` — the self-built chain head
  (SHA-256 `1da6e085…`, 4,388,023 bytes), buildable by nothing but
  itself and boot. The seed is the FEATURE FRONTIER: `libs/compiler/`
  may use anything the seed accepts, which is a superset of boot's
  frozen set. The gate's seed-chain leg enforces it — the seed must
  build the current mirror (byte-identical to the chain's child while
  the mirror stays within boot's subset; behavior-graded once the
  frontier moves past it). Re-pinning ritual: docs/bootstrap.md.
- **Value containment** (spec §1.5): the `[2]S` hole is closed — a
  decl-time walk over every interned struct rejects inline cycles the
  in-flight resolution window missed (`tests/lang/eq` e09–e11: direct,
  mutual, and the legal-indirection controls).
- **Managed make zeroing — attempted and REVERTED same round**: the
  release walk may read every element of a `make([]T, n)`, and a reused
  heap block hands back junk (the wasm allocator's zero-filled fresh
  pages mask it — the same law as the layout-table zeroing). Emitting a
  `zero_bytes` call in `lower_make` for managed element types was
  correct on paper, green on the corpus (92 programs) and the app's
  corpus law, and could not fix 081 — but the SECOND self-hosting
  generation crashed: the child (even built `--no-opt`) panicked with a
  null dereference in its own `dce_sweep` compiling the mirror. The
  inserted call sequence (a zext ptr→usize and a same-width bitcopy
  cast feeding a two-arg prelude call) miscompiles somewhere between
  m.wasm's lowering and the child's execution; bisected precisely to
  this change (containment, TCO and the trailing `unreachable` are
  innocent — the chain closes without the call). Spec §1.5 records the
  law and the revert; whoever retries should emit the zero as a plain
  IR loop (no cross-type casts, no prelude dependency) and re-run the
  FULL gate, not just --wasm.

## Boot freeze — decided

- `boot/src` is frozen at 0.4.0 + triple-quote and ships ONE target,
  `wasm32-wasi`. A native target argument is refused with a clear
  error, exit 2. fmt and the native backends live in the self-hosted
  compiler only.
- The feature-subset law is now TWO-LAYER (see docs/bootstrap.md): boot
  defines the eternal floor (what a fresh host can always rebuild from
  C), the pinned seed defines the current frontier (what libs/compiler
  may actually use). The gate's build-mirror leg (boot builds the
  mirror) stays the tighter enforcement while it holds; the day the
  mirror grows past boot's subset, that leg's builder becomes the seed
  and the ritual below re-pins.
- `docs/bootstrap.md` is the map: the seed chain, who builds which
  artifact, the two-layer law, the rebuild commands.

## Native backends — closed, measured 2026-09-23

All native code lives in the mirror (`libs/compiler/`); boot cannot emit
a native image at all. The gate's crossing legs are build-only — a
corrupt native image is never exec'd here (the kernel-wedge lore lives
in this file's git history). `tools/gate.sh` full run: **every target's
hello smoke AND full self-build** for arm64-mac, arm64-linux,
amd64-linux.

## The optimizer — landed 2026-09-23, tree-shake + TCO same day

`libs/compiler/opt.rho` (spec §3.6): constant folding + dead-instruction
elimination + tail-call optimization, mirror-only, on by default
(`--no-opt` disables). The gate's differential legs are the referee (92
programs × boot vs mirror, plus the grandchild chain), `tests/lang/opt/`
pins the folds themselves (23 cases: arith chains, wrap edges at both
widths, shift masking, comparison folding, divisor-0 trap survival, the
signed −1 wrap law, const-lane casts, the global shake, and the eleven
tail-call cases). Differential fuzzing re-ran with everything live:
**300/300 seeds identical**.

The reachability walk covers globals too (spec §11.3): unreferenced
statics never enter any artifact — which is exactly the mechanism the
wasm-only web root rides to drop the native backends.

## The emitter half-diamond — fixed 2026-09-23

The eq-walk's variant ladder exposed a historical wasm-emitter bug: a
cbr whose then-arm chain continues into the ELSE target emitted that
target inline after a depth-0 br (dead), so **every enum variant rung
after the first compared nothing**. The mirror now nests a
`block $else-target` around the if. Boot, frozen, keeps the quirk — the
two compilers disagree there by design, and the corpus records only
what they agree on. The half-diamond check must stay linear
(`arm_falls_into`, ≤8 steps): chain_terminal's recursive walk on every
cbr made a self-build take 5+ CPU minutes (13 s is the bar).

## Element-wise aggregate == — landed 2026-09-23

Spec §4.2's deferred feature, in the self-hosted compiler (boot still
rejects it, frozen): `==`/`!=` on structs, fixed arrays and slices
compare element-wise through memoized `rho__eq$<t>` helpers. Every
participating type must itself be comparable; `fn`/`dyn`/`err` are
rejected with a diagnostic. `tests/lang/eq/` (11 cases) is the pin.
Fixed arrays construct as struct fields (element writes, pinned by
e07); a standalone array literal still does not exist.

## Native real-machine exec — arm64-mac measured 2026-09-23

`tools/native-exec.sh` (deliberately OUTSIDE the gate — a corrupt image
can wedge the kernel, so the gate stays build-only): builds each
program to wasm and to arm64-mac, runs the wasm under wasmtime as the
behavioral baseline, then execs the native image directly. **18 ok,
1 known** across a representative corpus slice plus the whole eq suite.

`rho build --target <native>` also writes the assembly sidecar
(`<out>.s`) — the toolchain never shells out, so it is the only way to
read what a backend emitted.

### Known: corpus 081 SIGSEGVs natively (pre-existing, open)

`corpus/081_generics_bounds.rho` crashes (rc 139) natively. Minimal
repro: `printf(...)` followed by `make([]string, 3)` (either alone
passes; make-**first** also passes; `[]i64` elements pass). Diagnosis as
of 2026-09-23's deep round — much narrowed, still open:

- The fault is inside an rc walker (`rel$s___string`'s element walk →
  `rc_dech`): a **stack** string struct's `hdr` field reads
  `0x100000000` — the image's LINK-TIME base, not a leaked adrp (a
  pc-relative adrp under a random slide cannot produce the bare link
  constant; there is no page-0 adrp and no baked 2^32 qword anywhere in
  `__DATA`).
- **Slide-dependent**: under a fixed base (lldb
  `target.disable-aslr true`) the same binary exits 0 and `rc_dech` is
  never even called with the bad value — the corruption's downstream
  path differs, so something upstream branches on an address value.
- The value is a link constant computed or carried at runtime; the
  per-run fault address is exactly `0x100000000` across different
  slides. Suspects: the fmt/varargs machinery's interaction with the
  free-list allocator handing a reused block to the later make, with
  some fn-pointer/addr materialization in that path encoding a link
  constant instead of a pc-relative address (the W-form encoder bugs of
  the last round are the precedent).
- Ruled out: the tail-call pass (crashes identically with `--no-opt`),
  and uninit elements (the managed-make zeroing landed; the crash
  shape persists).
- Next steps for whoever picks it up: assemble `out.s` + the rt blob
  with the SYSTEM toolchain to get a symbol-rich twin (the in-tree
  image is stripped; `clang -arch arm64 -c` on `cat <blob> out.s` —
  the blob's `_rho_rt_heap` needs a `__HEAP` segment stand-in), then
  watchpoint the string struct that receives the bad hdr. `KNOWN` in
  tools/native-exec.sh carries it — remove that entry when it falls.

## Later batches

- fmt-canonicalize `libs/compiler/` itself: the tree predates the
  mirror's own `fmt` (the blank-line convention around `use` blocks
  drifted); `m.wasm fmt -w` over the package is a mechanical round of
  its own — do not slip it into a feature round.

- Native 081: the open arm64 SIGSEGV above (the only red in
  native-exec) — now narrowed to a slide-dependent link constant.
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

- ~~element-wise `==` on structs/arrays/slices~~ — landed 2026-09-23.
- ~~tail-call optimization~~ — landed 2026-09-23 (the loop form; true
  cross-function tail calls, wasm `return_call`, remain out of scope).
- linear-scan register allocator (replaces spill-everything)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)
- enum payload containment (the decl-time walk covers structs and
  arrays; variant payload types resolve through their own lane and get
  their pass when a real shape demands it)

## Standard library

Roadmap: `docs/stdlib-roadmap.md`.

## Owner's call

- The repo root has no LICENSE file (`tools/lsp` and
  `tools/vite-plugin-rho` declare MIT). Adding one is the owner's
  call, not the tree's.
