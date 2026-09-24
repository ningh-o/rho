# Bootstrap — the seed chain on one page

The C compiler under `boot/src` is the permanent bootstrap seed. It keeps
only: compile rho → wasm32-wasi, and the corpus oracle. Features are
frozen at 0.4.0 + triple-quote plus the module-system semantics and
string `+` (prelude `cat` retired — `+` concatenates strings and lowers
to the internal `__cat2` primitive) (see below); fmt and the native
backends (mac/linux, arm64(aarch64)/amd64) live in the self-hosted
compiler, the standard-library package `libs/compiler/`. Everything
downstream of it is built by rho itself. This page records the seed
chain, who produces which artifact, the two-layer feature law, and how
to rebuild each one.

## The chain

```
boot/src (C seed)
  └── build/rho-boot          cc over boot/src/*.c (+ generated prelude_data.c)
        └── boot/rho-seed.wasm      THE PINNED SEED — a byte copy of one era's
              │                     merged-root build (see "The seed" below)
              └── build/gate/m.wasm     the seed builds libs/compiler/cli.rho
                    │                   (default: --set native is true)
                    └── build/gate/web.wasm   m.wasm re-compiles cli.rho with
                    │                         --set native=false (the web config)
                    └── build/gate/child.wasm     m.wasm compiles cli.rho again
                          └── build/gate/grandchild.wasm   child compiles cli.rho
        └── build/gate/web-site.wasm  m.wasm compiles cli.rho --set native=false
              │                       (the web configuration, one generation
              │                       past the seed — its own frames follow the
              │                       mirror's fat-function memory-home law)
              └── site/assets/rho.wasm  wasm-opt -Oz deploy pass (pure copy
                    └── build/rho.wasm  when wasm-opt is absent; a copy at
                                        the historical path, for the tools
                                        that read it)
```

- **boot** — `build/rho-boot`, plain C, built by `make build/rho-boot`. The
  oracle: frozen corpus goldens (`corpus/*.out`) are its word, and it is
  the only tool allowed to regenerate them. It ships one target:
  `--target wasm32-wasi`; any other target is rejected with a clear error
  (the native backends live in the self-hosted compiler).
- **the mirror** — `libs/compiler/`, the compiler written in rho. Since
  the build-parameters round it has ONE package root, `libs/compiler/cli.rho`
  (`main` must live in the root file; the shared body kept the `cli.rho`
  name when it was promoted from module to root — the native modules'
  `use cli;` loads the same file as a namespace, which is legal and
  shadow-exempt). The root const `native: bool = true` is a build
  parameter: the full toolchain is the default build, and the native
  pipe (`native/pipe.rho`, the six backend modules, the runtime blobs,
  the image writers) hangs behind `if (native)`.
  - **full build** (the default) — every target, wasm and native. The
    gate grades it as `build/gate/m.wasm`.
  - **web build** (`--set native=false`) — the fold skips the native
    side at check time, so nothing references the pipe and reachability
    drops the six backends from the artifact (measured: 3.6 MiB vs
    4.7 MiB); a native target refuses with the same clear-error law boot
    uses (exit 2), straight from the fold's `else` branch. `make site`
    produces `site/assets/rho.wasm` from the chain's SECOND generation
    (the seed-built mirror compiles the web configuration — one
    generation past the seed, so the shipped compiler's own frames
    follow `w_memmode` and the browser's ~1 MB wasm stack holds), then
    a `wasm-opt -Oz` deploy pass shrinks it when the binary is on PATH
    (pure copy otherwise; the chain artifacts themselves stay pure);
    `build/rho.wasm` is a copy for the tools that read the historical
    path (the site tests, the LSP, the vite plugin). No wasi-sdk
    anywhere: rho emits its own wasm, in-process.
- **child** — the mirror compiling itself: `m.wasm` builds
  `build/gate/child.wasm`.
- **grandchild** — the child compiling itself: `child.wasm` builds
  `build/gate/grandchild.wasm`. Stage 2 == stage 3 is never assumed
  byte-for-byte (two correct compilers may lay out functions
  differently); the grandchild is graded on behavior — it must rebuild
  every corpus program to the same stdout + exit code as boot.

## The seed

`boot/rho-seed.wasm` is a PINNED self-built compiler — the chain head of
one era (pinned 2026-09-25, SHA-256 `103e9b1e…`, the arm64-correctness
build: the store-width law for the store intrinsics plus the native
backend's encoding and calling-convention fixes; every internal symbol
compiles to the shortest serial name a..z, aa.. unless `-g` keeps the
readable forms; the pin's own chain is told under
Provenance below). It exists so the mirror's sources may grow
PAST boot's frozen feature set:

- **boot defines the eternal floor** — the feature subset a fresh host
  can always rebuild from C. It never grows again.
- **the seed defines the current frontier** — `libs/compiler/` may use
  anything the seed accepts (a strict superset of boot's set: the seed
  IS a self-built mirror of its era).
- The gate's **seed-chain** leg enforces the frontier every run: the
  seed must build the current mirror. Since the dot round it and the
  **build-mirror** leg are the same build — the seed's build IS the
  chain's m.wasm (the mirror's own sources speak the seed's language,
  which boot cannot parse; since the params round that includes the
  merged root's own `if (native)` fold). The web configuration is
  seed-era too: the web-root leg has this run's mirror (m.wasm) build
  the root again with `--set native=false`.

**The re-pinning ritual** (the day the mirror adopts a feature the seed
lacks — a post-freeze language feature landing in the mirror's own
sources). `tools/reseed.sh` IS the ritual, scripted and idempotent:

1. Land the feature in the mirror's front half (parse/check/lower/emit)
   WITHOUT using it in `libs/compiler/` sources yet. The script's
   builder probe picks boot while boot can still parse the mirror's
   sources, so the new pin stays boot-built for free.
2. Run `tools/reseed.sh`. It builds the package root with the best
   builder available (boot, else the current pin — the honest lineage
   since the dot round), writes the artifact to `boot/rho-seed.wasm`,
   keeps the superseded pin at `build/gate/rho-seed-superseded.wasm`,
   and rewrites the pin slot and ledger below mechanically (date +
   SHA-256). **Idempotent**: at a settled frontier the mirror compiles
   to the pin byte for byte, the script proves it and changes nothing —
   the ritual doubles as the standing "is the pin expired?" probe.
3. Author the one sentence the script refuses to invent: the Provenance
   line saying WHICH feature forced the re-pin.
4. Prove the new pin: `sh tools/gate.sh`. Every later run re-checks the
   record — the seed-chain leg calls `tools/reseed.sh --check`, which
   compares the file's actual SHA-256 against the slot below, so the
   pin cannot rot silently.

Now `libs/compiler/` may use the feature.

The dot round (2026-09-24) walked steps 1–3 in order: the feature landed
in the mirror's front half, the seed was re-pinned boot-built
(`554b80e8…`), then `native/pipe.rho` and `full.rho` moved to dots and
the build-mirror leg's builder became the seed.

The params round (2026-09-24) walked them again, four pins in one day —
the ritual is cheap once boot is out of the loop: the params machinery
(root consts, `--set`, the comptime fold) landed in the mirror's front
half with `libs/compiler/` still not using it; the seed was re-pinned to
that frontier (`24c185b2…`); then the roots merged — `cli.rho` became
the one root, declared `const native: bool = true`, and gated the pipe
behind `if (native)`. The first merged build tripped the shadow diag on
the `use cli;` aux copies (the checker then lacked the same-path
exemption), so a transitional root — `round_root.rho`, no consts,
deleted in the same breath — rebuilt the seed with the exemption
(`cdeeed88…`), and that seed built the merged root (`33d386df…`). The
web-root leg's builder switched from boot to this run's mirror compiling
`--set native=false`. Next, the internal-symbol slimming (short module
indices instead of full module paths in every internal name) took two
more pins to reach its fixpoint — a seed built by a long-name compiler
emits long names (`051f4b3b…` → `84a60281…`) — and exposed one hardcoded
hanger-on: the native runtime blobs spelled the prelude heap label the
old long way; `native/pipe.rho` now takes it from `sym_symbol`
(`heap_label`), pinned as `4bdba9d3…`. The seed compiles the mirror to
itself, byte for byte.

The serial-symbol law (still 2026-09-24) walked the ritual twice more:
every internal symbol — user items and the synthesized rc/eq/env helpers
alike — now compiles to the shortest serial name a..z, aa.., interned
per long-form base so caller and callee always agree; `-g` keeps the
readable forms. The rename flushed out three consumers of name *shape*
(never of a name): the wasm emitter found the prelude heap cell by a
`_HEAP` suffix, the optimizer's tail-call law spelled its rc helpers
`rho__…` (twice: the classification AND the pins the rewrite injects),
and the rc runtime glued the header-convention `h` onto an
already-interned serial name — all now resolve structurally
(`prelude_symbol`, `lower.is_synth_callee`, `lower.synth_name` at the
rewrite, and the base composed before interning). Two pins per step to
the fixpoint (`73dca867…` → `3242a7ee…` → `f3fe3f90…` → `01685bd9…` →
`1a91cdb9…` → `84d30232…`); the seed compiles the mirror to itself,
byte for byte.

Provenance: `103e9b1e…` — seed-built at the 2026-09-24 stdin round: the
prelude gained its first user-facing input function, `read_line()`
(wasi.rho, byte-at-a-time so a reader never consumes past the line;
"" at EOF), synced into both compilers' embedded copies — programs can
finally read input on every target. Every guest run in the tree's
scripts now redirects stdin from /dev/null so the EOF path is the
deterministic one (an inherited tty could block a corpus run); the real
fed-bytes path is graded by the site and app pipelines.

Older pin: `1a814a1a…` — seed-built at the 2026-09-24 browser-stack
round: the wasm emitter gained the fat-function memory-home law
(`w_memmode`, threshold 2048 vregs) — a function past the threshold
keeps every vreg in an 8-byte linear-memory home and four scratch
locals, the native backends' own discipline, because the
spill-everything local map gave `check_expr` a frame taller than the
browser's whole ~1 MB wasm stack (a 50-term arithmetic chain was the
smallest witness; the cliff moved past depth 1600). The SITE asset now
builds one generation past the seed (make site: m.wasm compiles
`--set native=false`, then a wasm-opt -Oz deploy pass when available —
2.47 MiB / gzip 748 kB, smaller than the old seed-built asset AND
overflow-free), so the shipped compiler's own frames follow the new
law; the chain artifacts stay pure.

Older pin: `08277f23…` — the settled head after the 2026-09-24
seed-46 experiment round-tripped and reverted: three comptime-eval
cures were attempted (shift-rule i32 retype, value-lane mask with a
lane-aware signed compare, count-lane mask). The retype and value/lane
halves heal seed 46, but every combination that heals it turns seeds
55/114 — the shift-COUNT lane: the evaluator's `y & 63` must become the
runtime's per-lane hardware mask (mod-32 on 32-bit lanes), and with the
count masked the if-condition and let-binding folds still disagreed on
the same expression. The tree reverted clean (the pin reproduces
08277f23 byte for byte); the recorded diagnosis in docs/todo.md now
carries the three-part law for the next attempt.

Older pin: `08277f23…` — seed-built at the 2026-09-24 native-ring round:
two cures closed the FULL/native bootstrap ring. (1) The native const
path materialized narrow SIGNED constants zero-extended (the fold keeps
an i32 -1 as the u64 pattern 4294967295) — every `field == -1` on an
i32 compared false, and the self-built compiler's own addrc lit==-1
check was the witness; both native backends now sign-extend per the
destination vreg's type (corpus/106). (2) The assembler split the full
text into a per-line vector — a self-build's ~70 MiB of assembly
allocated ~0.5 GiB of transients and blew the 1.75 GiB native heap
ceiling mid-ring; the walk is now a cursor over the two source strings
(the rt blob rides ahead without a concat). The ring: 9/9 under
tools/native-ring.sh, and native-exec runs 21 programs wasm-vs-native
identical on the machine.

Older pin: `ecd9ff7e…` — seed-built at the 2026-09-24 container-ownership
round: the managed-slice rc walkers let a by-value binding walk the
array's CURRENT elements on release (the rehash's re-put elements were
never seen by the binding's retain, and the unconditional release freed
them — the native ring's map repro); retain is now the buf count alone
and release walks elements only at the last-reference death check
(rc == 1, not immortal). The same round marked the walker's synthesized
loop `loop_header`/`loop_exit` (the StWhile law): unmarked, the wasm
emitter's re-nesting emitted the back-edge as a function exit and every
managed-slice walker on wasm was dead code — a pure leak the
output-equality gate could not see. corpus/105 pins the cure.

Older pin: `49c97f09…` — seed-built at the 2026-09-24 fuzz-widening
find: the mirror's comptime compare fold judged untyped negative
literals unsigned (`((-51) >= 24)` folded TRUE — the -51 wrapped as
u64), because cc_signed skipped TyIntLit; the fix rides the let-binding
law (an INT_LIT's default domain is i32) and healed 9 of the 10
boot-vs-mirror divergences the widened differential campaign had just
found (corpus/104 pins it; seed 46's shift-width family stays open).
The pinned seed is the chain's m.wasm, byte for byte.

Older pin: `4af1ffc3…` — seed-built at the 2026-09-24 PIE-and-statics
round: the mirror moved (the arm64 wave's backend fixes below, then the
PIE statics law — an arm64-mac image is PIE and a `.quad <symbol>` in
__DATA is an absolute address no fixup rebases, so the emitter now
zeroes those words and `_rho_statics_init` stores the real addresses
through adrp before main — plus static string literals gaining their
wasm rodata pair, which both compilers had silently never emitted). The
pinned seed is the chain's m.wasm, byte for byte — the compiler
reproduces itself exactly at this frontier.

Older pin: `da7d3b1a…` — seed-built at the 2026-09-24 arm64
correctness wave: the mirror's front half moved (the store-width law
for the store intrinsics, the arm64 encoding/compare/shift/cast fixes,
frame zeroing, the narrow-signed call-result extension), so the pin
follows it — two pins to the fixpoint (`608c9c9a…`, built by the
pre-fix seed, then this one: the fixes change the wasm emit, so a
fixed compiler emits a different mirror than the old seed did). The
pinned seed is the chain's m.wasm, byte for byte — the compiler
reproduces itself exactly at this frontier.

Older pin: `84d30232…` — seed-built from the merged root `cli.rho` at
the 2026-09-24 build-parameters round (root consts as build parameters,
`--set`, comptime fold, the roots merged, shortest-serial internal
symbols with the `-g` full-name switch, blob heap labels and rc-helper
classification through the naming site). The pinned seed is the chain's
m.wasm, byte for byte — the compiler reproduces itself exactly at this
frontier.

## The arm64 correctness wave (2026-09-24)

The native ring's first executor pass (a self-built arm64-mac `rho`)
crashed on EVERY compile; bisecting it through program-level probes and
a wasm-vs-native differential fuzzer run (40 seeded programs;
`tools/fuzz/gen.mjs --emit N` regenerates any case) found nine real
defects, all fixed in the mirror this round:

- **asm64 encodings** — the single-precision forms systematically wrong
  at the ftype bit: `fcvt s,d` (opcode 1100 vs 100), `fcvtzs`/`fcvtzu`
  (source-width bit + the zu form absent), `scvtf`/`ucvtf` D-dest (bit
  22; the old code cleared bit 31 instead), `fmov wd,sn` (read the D
  register). Verified against clang-assembled ground truth.
- **f64 compares** — `ac_cmp` keyed the operand width off an `i.size`
  the lowerer never sets; it now follows the operand's type (the wasm
  emit's own law). Every f64 comparison compared the low 32 bits as
  f32s before.
- **narrow variable shifts** — the count masks to the operand's lane
  (wasm's shift law): 32-bit lanes now use the W-form variable shift,
  whose hardware mod-32 is exactly that law.
- **float→int casts** — the trunc_sat law (out-of-range saturates):
  the lane follows the TARGET width and the opcode its signedness
  (`fcvtzs` vs `fcvtzu`), matching the wasm saturating conversions.
- **frame zeroing** — every arm64 prologue now zeroes the whole frame
  (qword loop): a vreg or slot read — or RELEASED — on a path that
  never defined it must see 0, the wasm-local law; native frames carry
  stale bytes from earlier calls and the rc walkers chased them.
- **the store-width law (lowerer, both backends)** — the store
  intrinsics' NAMES fix the width: `store_u64(p, 0)` with the literal
  defaulted to i32 stored FOUR bytes, leaving every "zeroed" qword's
  high half as recycled-block junk — on wasm too, masked by fresh
  pages. The allocator's own `zero_bytes` was the headline victim; the
  fix forces the width from the name at the lowering site.
- **narrow SIGNED call results** — a W-register write zeroes the upper
  half of the 64-bit register, so an i32 `-1` spilled as `str x0`
  became +4294967295 for every later 64-bit compare; the call result
  now sign-extends (`sxtb/sxth/sxtw`) before the 8-byte spill.

After the wave: the self-built native compiler passes `--version`,
`fmt` (canonical output), and small parses; `check`/`build` at package
scale remain flaky (~60% crash — see docs/todo.md's open section). The
wasm gate stayed 8-green through every fix; the differential fuzzer
went 40/40 on both targets.

## The PIE-and-statics round (2026-09-24, same day)

The wave's leftover — "check flaky, correlates with the initial stack
layout, lldb clean" — bisected to a diagnosis nobody had on the list:
**the arm64-mac image is PIE, and the static string initializers wrote
`.quad <symbol>` words into __DATA — absolute compile-time addresses
that no fixup ever rebases.** Every kernel slide turned them into wild
pointers; rc_dec chased one into the read-only string pool and the
write faulted (lldb's no-ASLR default is why it always exited clean
there; the env-padding luck moved the slide). The fix is structural:
`emit_glob_data` diverts those words for mac builds into a collector,
emits `.quad 0`, and a new `_rho_statics_init` routine (called by the
rt blob before `main`) stores the real addresses through adrp —
slide-proof by construction. ELF targets stay ET_EXEC at a fixed base;
their absolute words remain legal. After the fix the self-check went
20/20 on the exact workload that crashed 18/20.

The same round closed a hole the corpus had never covered: **static
string literals on wasm never worked on EITHER compiler** — boot and
mirror alike emitted the {h, buf} words as zero and never laid the
rodata block into the data section, so every read saw garbage (the
native emitters always wrote the pair). Both ends now lay the block
out, patch the words, and emit it; `corpus/103_static_str` pins the
law, and `corpus/101_f64_fields` / `corpus/102_make_zero` pin two
wave-era cures the corpus had equally never covered (the f64-field
deref-new shape; managed make() over recycled blocks).

Still open from this round: one arm64 codegen defect (managed-map
rehash losing entries — the compiler's own reachability pass runs on
exactly that shape, which is what breaks a native-built self; see
docs/todo.md's minimized repros).

Previous pins, same round: `84a60281…` and `051f4b3b…` (the two
short-symbol fixpoint steps), `33d386df…` (the merged root,
pre-slimming), `cdeeed88…` (seed-built through the transitional
`round_root.rho` — carried the same-path shadow exemption),
`24c185b2…` (the params machinery, pre-merge). Older:
`554b80e8…` — boot-built from full.rho at the 2026-09-24 dot-module
round (dots, `as` aliases, `pub use`, package facades; the build-mirror
leg's builder switched from boot to the seed that round). Older still:
`1da6e085…` — boot-built from full.rho at the 2026-09-23 tail-call/roots
round.

- `73dca867…` — 2026-09-24, seed-built (supersedes `4bdba9d3…`)
- `3242a7ee…` — 2026-09-24, seed-built (supersedes `73dca867…`)
- `f3fe3f90…` — 2026-09-24, seed-built (supersedes `3242a7ee…`)
- `01685bd9…` — 2026-09-24, seed-built (supersedes `f3fe3f90…`)
- `1a91cdb9…` — 2026-09-24, seed-built (supersedes `01685bd9…`)
- `84d30232…` — 2026-09-24, seed-built (supersedes `1a91cdb9…`)
- `608c9c9a…` — 2026-09-24, seed-built (supersedes `84d30232…`)
- `da7d3b1a…` — 2026-09-24, seed-built (supersedes `608c9c9a…`)
- `4af1ffc3…` — 2026-09-24, seed-built (supersedes `da7d3b1a…`)
- `49c97f09…` — 2026-09-24, seed-built (supersedes `4af1ffc3…`)
- `ecd9ff7e…` — 2026-09-24, seed-built (supersedes `49c97f09…`)
- `08277f23…` — 2026-09-24, seed-built (supersedes `ecd9ff7e…`)
- `08277f23…` — 2026-09-24, seed-built; the seed-46 experiment's
  `328229a0…`/`65bde005…` pins lived one hour and reverted to this pin
  byte for byte
- `1a814a1a…` — 2026-09-24, seed-built (supersedes `08277f23…`)
- `103e9b1e…` — 2026-09-25, seed-built (supersedes `1a814a1a…`)
<!-- pin-ledger -->

## The feature subset law (two layers)

`libs/compiler/` may only use language features the CURRENT FRONTIER
already implements — boot's set while the mirror stays within it, the
pinned seed's set thereafter. This is not a convention — `tools/gate.sh`
enforces it twice: the **build-mirror** leg (the pinned seed compiles
the mirror — one build with the seed-chain leg since the dot round) and
the **seed-chain** leg (the same frontier, graded byte- or
behavior-wise). A mirror source that runs ahead of the
frontier simply fails to build and the gate goes red. A new syntax feature
lands in the mirror's front half first, then (legally, once the frontier
accepts it) in `libs/compiler/` sources and the mirror's own modules —
see the re-pinning ritual above. The differential corpus (e.g.
`corpus/031_*` for triple-quoted multiline strings) pins the shared
semantics both ends must agree on.

### The module map

`libs/compiler/` is the compiler as a standard-library package — one file,
one module, `use` with relative paths:

| module | contents |
| --- | --- |
| `cli.rho` | THE package root (the shared body promoted): host helpers (Vec/Map/string utils), diagnostics, the CLI driver (`run`), and `main` — the root consts are the build parameters (`native` gates the pipe), `pub static RC_NATIVE` + the request statics are the pipe seam |
| `native/pipe.rho` | the native image pipeline: the six backend modules, `build_native_image`, the `rt_*` blobs, `pipe_emit` |
| `lex.rho` | the lexer: `Tok`, `Token`, the scanners |
| `parse.rho` | the AST types and the recursive-descent parser |
| `check.rho` | the checker (check.c port): types, symbols, generics, expression/statement checking, generic fn instantiation — flattened to boot's phase model (registration → decl passes for every module → body passes for every module → global instantiation worklist) |
| `ir.rho` | the IR structures (LOW-1) |
| `lower.rho` | lowering: builders, glue synthesis, layout, rc runtime, reachability (LOW-2..LOW-8) |
| `opt.rho` | the optimizer: constant folding + dead-instruction elimination (spec §3.6; mirror-only, boot stays frozen; `--no-opt` disables) |
| `emit_wasm.rho` | the wasm32-wasi emitter (LOW-10) and the IR dump |
| `prelude_src.rho` | the embedded prelude sources + `read_prelude` |
| `fmt.rho` | the formatter (AST → canonical source) |
| `native/emit_arm64.rho` | IR → arm64 assembly text |
| `native/emit_amd64.rho` | IR → x86-64 assembly text |
| `native/asm64.rho` | the arm64 assembler + shared assembly frontend |
| `native/asm86.rho` | the x86-64 assembler |
| `native/macho64.rho` | the mach-o writer (Sha256, section buffer) |
| `native/elf64.rho` | the ELF writer |
| `native/pipe.rho` | the native pipeline behind `if (native)` in the root (see above) |

### The multi-module semantics this layout rests on

Three semantics landed in boot and were ported 1:1 into the mirror's own
checker (`libs/compiler/check.rho`), with tests in `tests/lang/modsys/`:

- **`pub static mut`** — a mutable static writable across modules, spelled
  `pub static mut NAME: T = init;` (parse.c recognizes `mut` behind `pub`).
  Reads, writes, and compound writes lower exactly like in-module statics;
  `pub static` without `mut` stays immutable everywhere.
- **module-qualified enum variants** — `mod.E.V` works in value position
  and `mod.E.V(...)` constructs, mirroring the unqualified `E.V` forms;
  match patterns were already qualified-path capable.
- **use-path root fallback** — `use` resolves against the using file's
  directory first, then the entry file's directory (so `native/*.rho`
  reach their root-level siblings and vendored packages keep nesting).

And one structural note for the module phase: the mirror's `check_all` was
flattened to boot's phase model — registration cascade, then declaration
passes for every module, then body passes for every module, then one
global instantiation-worklist drain. The previous per-use recursion
checked bodies mid-registration and broke on cross-module ordering.

The **dot round** (2026-09-24) rebuilt the module system's front on the
seed side: dot-separated use paths (slashes retired with a diag), `as`
aliases, `pub use` re-exports (full flatten, single item, namespace form),
package facades (`lib.rho`) with package-private interiors, the
exactly-one-real-body resolution law (lib.rho + file.rho coexistence is an
ambiguity diag), `main` reserved in the root file, and chain access
through one re-exported namespace (`pkg.sub.item`). The seed carries the
front half; `libs/compiler/` sources use the syntax themselves
(`native/pipe.rho`, `cli.rho`), legal only after the re-pin. The language
law lives in `spec/module-system.md`; the fixtures in
`tests/lang/modsys/` (marker file `mirror` = runs on the seed-built
compiler) and the seed-era corpus entry `corpus/032_pkg` pin it.

## The gate

`tools/gate.sh --wasm` is the wasm acceptance run — eight legs, one
verdict each, functional equivalence the only grading:

1. **boot-selftest** — the oracle is healthy (AST + diag goldens).
2. **build-mirror** — the pinned seed builds the merged root
   (`libs/compiler/cli.rho`) in its full configuration. The frontier
   leg: the mirror's own sources must speak the seed's language.
3. **corpus-diff** — boot-era `corpus/*.rho` boot-built vs
   mirror-built under wasmtime, stdout + exit identical; seed-era
   entries (boot cannot parse them) graded against their committed
   goldens; the boot runs stay the corpus goldens every later leg
   grades against.
4. **self-chain** — the full ring: mirror → child → grandchild, the
   grandchild rebuilding every corpus program to the goldens in
   behavior.
5. **seed-chain** — `tools/reseed.sh --check` first (the pin slot's
   SHA-256 against the file's real hash), then the pinned seed builds
   the mirror: byte-identical to the chain's child at a settled
   frontier, behavior-graded once the frontier moves.
6. **web-root** — this run's mirror compiles the root again with
   `--set native=false`: version, hello build, the native refusal
   (exit 2, the one-line reason), and the FOLD'S FINGERPRINTS — the
   artifact strictly lighter, a smaller function section (944 vs 1158
   functions measured 2026-09-24), and the native image writers'
   diagnostic strings (`macho64`, `elf64`) absent from the web artifact
   while still present in the full one (the positive control that keeps
   the greps honest).
7. **web-chain** — the web configuration's OWN bootstrap loop:
   web.wasm builds itself → child → grandchild, all `--set
   native=false`; the grandchild satisfies the same web behavior law,
   stays pruned (the same fingerprints), and rebuilds every corpus
   program to the shared goldens — the web compiler is a full citizen
   of the equivalence law, not a shrugged-off subset.
8. **diag-parity** — `tests/diag/*.rho` messages byte-identical between
   boot and mirror.

The full run adds the native crossings — per target (arm64-mac,
arm64-linux, amd64-linux) a hello smoke and the full self-build, every
image statically structure-checked by `tools/image-check.py` (Mach-O /
ELF headers and load commands; the gate never executes a native image —
a corrupt one can wedge the kernel, so structure is the evidence).
`--fast` drops the self chains.

Every wasmtime invocation in the gate and the suites runs at the DEFAULT
wasm stack — no `max-wasm-stack` flag anywhere. The browser gives wasm
about 1 MB (V8's frames are the fat ones), so the default stack is the
honest grading condition: anything that only survives with a bigger stack
is a real red. The return-ness judgment is cycle-safe and memoized
(`check.rho`'s `call_returns`: a callee mid-judgment answers from its
declared signature) precisely so diagnostics never depend on stack depth —
pinned by `tests/lang/modsys/tail_dive` and `tail_down_form`.

## The closure criteria ("bootstrap complete")

The wasm bootstrap counts as closed only while every clause below is
RE-PROVEN by the current tree — never inherited from a past round's
measurements:

- **the seed chain holds end to end** — boot → pinned seed → mirror →
  child → grandchild, with the corpus as the shared behavior oracle and
  the pinned seed byte-reproducing the mirror at the settled frontier
  (gate legs 2, 4, 5);
- **the web configuration is a full citizen** — its own two-generation
  ring closes (`--set native=false` throughout), the fold's fingerprints
  prove the six backends are gone from every web artifact, the
  grandchild rebuilds the whole corpus to the shared goldens (gate leg
  7), and the shipped site asset built this way compiles AND runs every
  playground example in a real headless Chrome
  (`node tools/test_chrome_examples.mjs`; the node-side twins are
  `tools/verify_examples.mjs` for the examples and
  `tools/test_browser_compiler.mjs` for the corpus);
- **the frontier is scripted** — `tools/reseed.sh` performs the
  re-pinning ritual (idempotent: at a settled frontier a run is a
  verified no-op), and the gate's seed-chain leg re-checks the pin slot
  every run;
- **diagnostics are boot-identical** (gate leg 8), and the differential
  fuzzer's recorded verdicts stand.

**The FULL/native ring CLOSED 2026-09-24**: `tools/native-ring.sh`
runs 9/9 — the self-built arm64-mac `rho` builds the package root to a
native image (structure-checked), reports its version, compiles+runs a
wasm hello, compiles the root AGAIN through the native host (the ring
child, structure-checked), and the child rebuilds six corpus programs
to the goldens; `tools/native-exec.sh` runs 21 programs wasm-vs-native
identical on the machine. The ring stays OUTSIDE the gate by law —
executing a native image can wedge the kernel's page-hash check, so
structure is the gate's evidence and execution is the separate,
deliberate step the crossings unblock.

## The remaining surface

`boot/src`, one line per file — the whole of what is left:

| file | responsibility |
| --- | --- |
| `main.c` | CLI dispatch (`check`/`build`/`run`/`test`/`selftest`), `parse_target` (wasm32-wasi or clear error), the selftest goldens, the corpus test runner |
| `lex.c` | lexer: tokens, triple-quoted multiline strings |
| `parse.c` | parser: source text → AST |
| `check.c` | typechecker; registers the one prelude (core + wasi tail) into every compilation |
| `lower.c` | lowering: checked AST → SSA IR over virtual registers + frame slots |
| `ir.h` | the IR data model, the single-target `Target` enum, shared inline helpers |
| `emit_wasm.c` | the one backend: IR → binary wasm32-wasi module |
| `dump.c` | AST dumper behind the selftest's golden AST files (format shared with the mirror) |
| `util.c` | arena, `Str`/`SB`/`Vec`/`Map`, diagnostic rendering |
| `rho.h` | common definitions: arena, strings, AST/type declarations, symbol naming |
| `prelude_data.c` | generated (`tools/embed.py`): `core.rho` + `wasi.rho` embedded as one C string |

Dead-branch inventory from the slimming (what was removed, what stayed):

- Removed: `g_prelude_wasm` / `g_prelude_native` and the hosted/mac
  prelude selection — the target enum has one value, so target detection
  is dead; the lowerer's `wasm ? 4 : 8` pointer-width branches collapsed
  to `4` (wasm32); `boot/prelude/hosted.rho` and `mac.rho` are deleted,
  and boot embeds exactly one prelude (core + wasi). The mirror carries
  its own embedded prelude copies inside `libs/compiler/prelude_src.rho`
  (kept in sync by `tools/embed-prelude.py` — it reads `core.rho` +
  `wasi.rho`, which still exist; the hosted/mac tails it no longer finds
  are simply skipped, and a `make` guard would be the honest wiring the
  day the prelude grows again).
- Kept: the `#if !defined(__wasm__)` shims in `main.c` (argv/`system`
  stub, tmp+rename atomic write, directory listing, run-temp naming) —
  boot itself still runs in two forms, the native `build/rho-boot`
  binary and `rho-boot.wasm` in the browser, and these are its own
  file/spawn shims for those two hosts, not target backends.

fmt is gone from boot (`fmt.c`, the `fmt` command, the selftest fmt
roundtrip, `make fmt-check`): canonicalization and roundtrip are the
self-built chain's job — `build/gate/m.wasm fmt <file>` (or the shipped
`rho.wasm`) is the formatter now, including any corpus canonicalization.
Corpus goldens stay frozen as behavior (stdout + exit), which never
needed fmt.

## Rebuild commands

```sh
make build/rho-boot                                        # the C seed (~10s)
wasmtime run --dir . boot/rho-seed.wasm \
  build libs/compiler/cli.rho --target wasm32-wasi \
  -o build/gate/m.wasm                                     # the mirror, full configuration (the seed builds it; 900s cap)
wasmtime run --dir . build/gate/m.wasm \
  build libs/compiler/cli.rho --target wasm32-wasi --set native=false \
  -o build/gate/web.wasm                                   # the web configuration (~23% lighter)
make site                                                  # the site asset (m.wasm builds the web configuration; wasm-opt -Oz when available)
make test                                                  # boot selftest (goldens + diag)
sh tools/corpus-run.sh                                     # corpus vs goldens (boot-era via boot, seed-era via the web config)
tools/reseed.sh                                            # the re-pinning ritual (idempotent; --check verifies the pin slot only)
tools/gate.sh --wasm                                       # the wasm acceptance run (8 legs)
tools/gate.sh                                              # the full run (adds the native crossings)
node tools/verify_examples.mjs                             # every playground example, compiled + run in node
node tools/test_browser_compiler.mjs                       # every corpus program through the site's browser pipeline
node tools/test_chrome_examples.mjs                        # every playground example in headless Chrome (CHROME_BIN to override)
./build/rho-boot build x.rho --target arm64-mac            # clear error, exit 2
```

Every rho invocation in scripts runs under a wall-clock cap (`perl -e
'alarm shift; exec @ARGV' <seconds> <command>` — macOS has no GNU
`timeout`); the gate already wraps its own runs this way.
