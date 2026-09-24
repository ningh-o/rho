# todo

Living backlog. This file holds only what is still undone: the closed
rounds — the wasm bootstrap, the native wave, the optimizer, element-wise
`==`, tail calls, value containment, build parameters with the one
package root and the pinned seed, the dot-module round, the arm64
correctness wave of 2026-09-24 (which also closed corpus 081 — the old
slide/link-constant diagnosis was wrong; the allocator's `store_u64`
half-store left half-zeroed blocks and the rc walkers chased the junk),
the PIE-and-statics round of 2026-09-24 (the self-check crashes
bisected to absolute `.quad <symbol>` words in a PIE Mach-O's __DATA;
`_rho_statics_init` now stores the real addresses through adrp, and
static string literals gained the wasm rodata pair both compilers had
silently never emitted — corpus/101/102/103 pin the three cures), the
container-ownership round of 2026-09-24 (managed-slice rc walkers walk
elements only at the last-reference death check, and the walker's
synthesized loop carries loop_header — unmarked, every managed-slice
walker on wasm was dead code, a leak output-equality could not see;
corpus/105), and the native-ring round of 2026-09-24 (narrow signed
consts sign-extend on both native backends, the assembler streams its
lines — the FULL/native bootstrap ring closed 9/9) — are recorded once,
in git history and `docs/bootstrap.md`.

## NEXT — the native ring, gated

The FULL/native configuration bootstrap ring CLOSED 2026-09-24:
`tools/native-ring.sh` runs 9/9 (native self build + structure check,
--version, a native-compiled wasm hello, the native→native ring child,
and the child rebuilding six corpus programs to the goldens), and
`tools/native-exec.sh` runs 21 programs wasm-vs-native identical on the
machine. The ring stays OUTSIDE the gate by law (exec'ing a native
image can wedge the kernel's page-hash check); the standing decision to
make: whether a wrapper leg re-runs it per release, or it stays a
documented manual step next to the gate.

## Open compiler bugs

### boot-vs-mirror differential gaps: 10 seeds (2026-09-24, found by widening the campaign) — 9 CLOSED same day

`node tools/fuzz/gen.mjs` (seeds 1–150) and `--from 151 --to 280` were
re-run after the PIE-and-statics round: 10 seeds diverged boot-vs-mirror,
all verified pre-existing (the superseded pin `da7d3b1a…` reproduced the
mirror's side on every one — none was the round's regression). NINE of
the ten shared one root, fixed the same day: the comptime compare fold
judged untyped negative literals UNSIGNED (`((-51) >= 24)` folded TRUE —
the -51 wrapped as u64) because `cc_signed` skipped `TyIntLit`; the fix
rides the let-binding law (an INT_LIT's default domain is i32), is
pinned by `corpus/104_neg_lit_cmp`, and both campaigns now stand at
149/150 and 130/130.

Still open — **seed 46** (`tools/repro/fold-shift-width-seed46.rho`): the fold
computes `61 << 28` at u64 (1635778560) while the runtime pins INT_LIT
to i32 and wraps (0xD0000000, -805306368 signed) — so `86 <= (61 << 28)`
folds TRUE but runs FALSE. The 2026-09-24 evening attempt mapped the
whole law and reverted clean (the pin reproduced 08277f23 byte for
byte); the three parts, from the live experiments: (1) the shift rule
may NOT hard-snap an INT_LIT left side to i32 — `libs/compiler` itself
needs the literal to keep adapting (`let cap: usize = 1 << 16` breaks);
(2) a value-lane mask in cc_value's shift (mask the folded value to the
left operand's resolved lane, INT_LIT reading as i32) PLUS a
lane-aware signed compare (each operand sign-extended from its own
expression's lane) heals seed 46; (3) but the shift COUNT must ALSO mask
to the lane (`y & 63` → mod-32 on 32-bit lanes) or `82 << 43` folds 0
while the runtime computes `82 << 11` — and with the count masked, the
if-condition fold and the let-binding fold still disagreed on the same
expression through `cond_known`. Next attempt starts at part 3: diff
`cond_known`'s cc_value path against the let initializer's — the
remaining divergence lives there, not in the lane laws.

### CLOSED 2026-09-24: arm64 managed-map rehash (the container-ownership round)

The over-release was real but the arm64 backend was innocent: the rc
walker design itself let a by-value slice binding walk the array's
CURRENT elements on release (the rehash's re-put elements were never
seen by the binding's retain), and the wasm side only looked correct
because the walker's synthesized loop lacked `loop_header` — the wasm
emitter's re-nesting turned its back-edge into a function exit, so
every managed-slice walker on wasm was dead code (a pure leak). Retain
is now the buf count alone; release walks elements only at the
last-reference death check. corpus/105 pins it; the repros stay under
`tools/repro/`.

### Waiting on a Linux container (or matching hardware)

- amd64-linux / arm64-linux exec — the crossings stay build-only.
- In-container corpus — the corpus differential re-run inside a Linux
  container, per native target.

## Owner's call

- GitHub release — and the zips carrying self-built native clients.
- Bench the shipped compiler — the suite's rho lane builds with the
  boot seed; running it through the mirror (optimizer on) alongside is
  a methodology decision plus a full re-record.
- ~~A LICENSE file at the repo root~~ — added 2026-09-24 (MIT, matching
  `tools/lsp` and `tools/vite-plugin-rho` declarations).

## Later batches

- **Wasm artifact slimming — the 2026-09-24 audit, partially landed.**
  The STACK half is fixed: the wasm emitter's fat-function law
  (`w_memmode`, threshold 2048 vregs) keeps every vreg of a huge
  function in an 8-byte linear-memory home with four scratch locals —
  `check_expr`'s frame had been taller than the browser's whole ~1 MB
  wasm stack, so a 50-term arithmetic chain failed to COMPILE in the
  playground; the cliff now sits past depth 1600 on every axis
  (chain/parens/stmts), and the site asset builds one generation past
  the seed so the shipped compiler itself obeys the law, then a
  `wasm-opt -Oz` deploy pass ships it at 2.47 MiB / gzip 748 kB —
  smaller AND overflow-free against the old seed-built 3.31 MiB. Still
  open, the SIZE half: full-config mirror 5.0 MiB / web 3.8 MiB (the
  memmode expansion added ~0.7 MiB), ~51% of the instruction stream is
  `local.get`/`local.set` traffic, and the DATA section spends 586 kB
  on 11,206 one-string-per-segment heap images with no pooling. The two
  remaining levers: (1) the operand-stack discipline (values ride the
  wasm stack in straight-line runs, spill at joins) — roughly halves
  the code AND would obsolete w_memmode's expansion; (2) string
  pooling + dedup into one rodata block. Known edge: depth ~3200
  arithmetic chains now exhaust LINEAR memory (the homes) with a plain
  out-of-bounds — an honest degradation at an absurd depth.
- fmt-canonicalize `libs/compiler/` itself: the tree predates the
  mirror's own `fmt` (the blank-line convention around `use` blocks
  drifted); `m.wasm fmt -w` over the package is a mechanical round of
  its own — do not slip it into a feature round.

## Language backlog

- linear-scan register allocator (replaces spill-everything)
- escape analysis / rc-pair elimination (spec §3.4, §11.5)
- enum payload containment (the decl-time walk covers structs and
  arrays; variant payload types resolve through their own lane and get
  their pass when a real shape demands it)
- true cross-function tail calls (wasm `return_call`) — explicitly out
  of scope so far; the landed TCO is the loop form

## Standard library

`libs/json` is LANDED (parse+encode, `rho.toml`, corpus-style cases in
`libs/json/tests/` — `sh libs/json/tests/run.sh` green) and the roadmap
paper now says so (2026-09-24). The open design call before the next
package: the Ring-1 `std/` resolution extension (a std root outside the
program tree is unreachable under the relative-only `use` law) and its
naming.
