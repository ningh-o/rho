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
        └── site/assets/rho.wasm    the seed compiles cli.rho --set native=false
              └── build/rho.wasm    (the web configuration — about a megabyte
                                    lighter; a copy at the historical path, for
                                    the tools that read it)
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
    produces `site/assets/rho.wasm` this way (the pinned seed, run
    through wasmtime, is the builder — boot cannot compile the merged
    root's own fold); `build/rho.wasm` is a copy for the tools that read
    the historical path (the site tests, the LSP, the vite plugin). No
    wasi-sdk anywhere: rho emits its own wasm, in-process.
- **child** — the mirror compiling itself: `m.wasm` builds
  `build/gate/child.wasm`.
- **grandchild** — the child compiling itself: `child.wasm` builds
  `build/gate/grandchild.wasm`. Stage 2 == stage 3 is never assumed
  byte-for-byte (two correct compilers may lay out functions
  differently); the grandchild is graded on behavior — it must rebuild
  every corpus program to the same stdout + exit code as boot.

## The seed

`boot/rho-seed.wasm` is a PINNED self-built compiler — the chain head of
one era (pinned 2026-09-24, SHA-256 `84d30232…`, the merged-root build
of the params round at the serial-internal-symbol frontier: every
internal symbol compiles to the shortest serial name a..z, aa.. unless
`-g` keeps the readable forms; the pin's own chain is told under
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

Provenance: `84d30232…` — seed-built from the merged root `cli.rho` at
the 2026-09-24 build-parameters round (root consts as build parameters,
`--set`, comptime fold, the roots merged, shortest-serial internal
symbols with the `-g` full-name switch, blob heap labels and rc-helper
classification through the naming site). The pinned seed is the chain's
m.wasm, byte for byte — the compiler reproduces itself exactly at this
frontier.

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

**Deferred, by the owner's explicit call** (next round's first item,
see docs/todo.md): the FULL/native configuration bootstrap ring — a
native-target-compiled compiler compiling the compiler again, static
structure verification of the self-images, and a small native-exec pass
of the self-built compiler. The pieces short of the ring are already
graded (the crossings build and structure-check every native image);
what is deferred is closing the RING through a native-hosted compiler.

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
make site                                                  # the site asset (the seed builds the web configuration)
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
