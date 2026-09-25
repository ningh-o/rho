# rho — restart to 0.1.0

> **First action: back the current branch up to the remote archive.** See
> T0.1. Nothing happens before that copy is verified on the remote.

The language design in this file is **final** — ratified item by item on
2026-09-25. Implementation starts from zero: a fresh git history with no
carried-over commits, a fresh compiler, and exactly one release version:
**0.1.0**. There are no intermediate version numbers and no version
evolution — everything before the 0.1.0 gate is unversioned work on the
road to it. Do not invent versions; do not bump; do not tag anything else.

**0.1.0 scope: the wasm self-hosting bootstrap only.** boot (C, wasm
backend) → the self-hosted compiler written in rho → it compiles itself
to wasm → gate green → 0.1.0. Native compilation is **not** part of
0.1.0: it moves into the standard-library workstream after 0.1.0
(Phase 7).

## Working protocol (every phase, no exceptions)

- When a phase starts, expand it into a small TODO list **in this file**.
  Each small TODO is exactly **one commit**: English commit message,
  prefixed with the TODO id (`T3.4: checker — overload resolution`).
- **Every commit carries its tests, and they pass.** A commit without its
  tests does not go in; a red test blocks the commit. **No leftover
  issues, no skipping** — no skipped/ignored/known-failure markers
  anywhere in the tree, ever. A problem is either fixed in the commit
  that triggers it, or the commit does not happen.
- A completed TODO is marked `[x]` in the commit that completes it (or in
  the next commit at the latest). Never mark ahead of the work.
- Every run is time-capped (perl alarm / poll pattern; never an
  unbounded wait).
- Standing laws: every path that overwrites an executable writes to a
  temp file and renames; browser/GUI walkthroughs assert **output panel
  content**, never button recovery; verification runs at the **default
  wasm stack** (a big-stack run proves nothing).
- **The archive branch is reference-only.** Reading it, comparing
  approaches, and studying its bug ledgers are allowed and encouraged;
  **copying code from it is forbidden.** Corpus *programs* (test data)
  may be adapted; implementation code may not.
- The design section below is **the law**. When implementation and design
  disagree, the implementation is wrong. If the design itself must
  change, change it here first, in a commit of its own, with the reason
  written.

---

## Phase 0 — archive, then a fresh start

- [x] **T0.1 Back up the current branch to the remote archive.** Commit
      the outstanding working-tree changes (site editor work, the visual
      probe script) onto a local `archive/pre-0.1.0` branch, push it:
      `git push origin archive/pre-0.1.0`. Verify the remote branch
      contains the final old-implementation state (HEAD b348133 plus the
      working tree). This is the only lifeline for all prior material —
      verify before touching anything else.
- [ ] **T0.2 Fresh history.** Recreate `master` from an orphan branch:
      no commits carried over. The fresh tree holds only this TODO.md, a
      README stub, and LICENSE (MIT). Everything else — the old
      compiler, corpus, spec, site, tools — lives on the archive branch:
      **reference-only; copying code is forbidden.**
- [x] **T0.3 Rewrite the spec (the law).** From the design section below,
      write the four documents fresh: `spec/syntax.md`,
      `spec/type-system.md`, `spec/module-system.md`, `spec/spec.md`
      (memory model, compilation model, determinism, version policy —
      version policy says: 0.1.0 = the wasm self-hosting bootstrap, or
      nothing). Add a conformance map: every design rule → the test that
      will hold it.
- [x] **T0.4 Corpus plan.** Decide the corpus rebuild from the archive:
      which of the 105 old programs adapt to the new language and in
      what order, which new programs the new features need (overloads,
      `?T`, labels, inner shadowing, build-parameter widening). Goldens
      are behavioral (stdout + exit) and will be regenerated from the
      new seed once it exists — never hand-written.

## Phase 1 — the C seed implements the complete design

boot is rewritten in C: **the only backend in 0.1.0 is wasm32-wasi**;
arena-allocated, one-shot, implementing **every** rule in the design
section — no placeholder stages. boot is the reference compiler.

- [x] **T1.1** Skeleton: build, arena, selftest harness, AST dump.
- [x] **T1.2** Lexer: full grammar — keywords incl. `trait impl for dyn`,
      integer/float literals (default lanes i32/f64), string escapes,
      **fully verbatim triple-quoted strings**, dotted `use`, labels,
      `?T` notation.
- [x] **T1.3** Parser: declarations (fn/methods/associated/generic,
      struct, enum, trait, impl, const/static/extern, use/as/pub use),
      overloads (same name, many signatures), patterns, labels on
      `while`/`loop`.
- [x] **T1.4** Checker I — names: locals → module → root build params →
      prelude; **inner shadowing allowed** (same-scope rebind is still an
      error); two visibility tiers + package facades; method candidate
      sets = native methods (with their type) ∪ methods of modules in the
      use closure; same-signature ambiguity = compile error naming both
      modules.
- [x] **T1.5** Checker II — types: the eleven type-system rules
      (consumer-typed literals with fixed defaults; const inference with
      optional builtin annotations; `as`-only conversions with unified
      truncating semantics on constants and variables; overload
      resolution = exact match unique; trait satisfaction = name +
      signature match; `[T: Bound]` bounds verified per instantiation;
      **non-null pointers by default, `?T` = Option sugar**, no smart
      casts; element-wise `==` behind the comparability law).
- [ ] **T1.6** Checker III — errors and folding: `?` on Result/Option;
      panic law; comptime folding of root-build-parameter conditions
      (dead branch parsed then skipped whole; reachability prunes
      modules); `--set name=value` with the **widened type face** (bool,
      all integer widths, f32/f64, string; range-checked; refusal = exit
      2).
- [ ] **T1.7** Lower + wasm emit: rc insertion per the pure-local
      counting rules (container ownership: walk elements only at the
      rc==1 death check), zeroed allocations, defer LIFO on every exit
      path except panic, tail-call→loop, labels lowering.
- [ ] **T1.8** Kernel prelude (the whole kernel, nothing more):
      Option/Result + `?` machinery, `to_str` family + variadic format
      sinks (printf/eprintf/format desugaring), panic/assert + hooks,
      allocator + rc glue, string primitives (`__streq`, concat), wasi
      raw-syscall tail (fd_write/proc_exit/args/file io). `read_line`
      does **not** live here — it is std.io's job.
- [ ] **T1.9** CLI: build/run/test/fmt/check; `-g` full symbols; fmt
      canonical roundtrip byte-for-byte.
- [ ] **T1.10** Corpus tranche 1: adapt the first batch of archive
      corpus programs + the new-feature programs; goldens regenerated
      from boot. Every later tranche is its own TODO.

## Phase 2 — the self-hosted compiler, written in rho (wasm only)

- [ ] **T2.1** Compiler package skeleton (lex → parse → check → lower →
      ir → emit_wasm → fmt → cli), compiled **by boot**; hello-world
      compiles itself.
- [ ] **T2.2+** Port module by module; each module its own TODO; graded
      by behavioral parity with boot across the whole corpus
      (determinism law: same compiler + same input → identical bytes).
- [ ] **T2.x** Optimizer in the mirror: constant folding, dead-code
      elimination, globals tree-shaking, tail-call→loop — with the
      language suites (opt/eq/params/modsys/strops/multiline) rebuilt
      for the new language. wasm emit only — **no native backends in
      0.1.0.**

## Phase 3 — gates and the trust root

- [ ] **T3.1** gate.sh rebuilt: boot selftest; corpus differential
      (boot-built vs self-hosted-built, behavioral); diagnostic parity;
      the self chain (mirror → child → grandchild, graded behaviorally).
- [ ] **T3.2 Pure-source trust root**: every gate run rebuilds the seed
      from boot's C source on the spot. The pinned `seed.wasm` stays in
      the repo **as a canary**: rebuild, compare byte-for-byte (D1 makes
      this exact), inequality = determinism alarm.
- [ ] **T3.3** Differential fuzzing rebuilt for one implementation:
      opt-on vs opt-off self-differential + golden corpus replay. (The
      old boot-vs-mirror differential retired with the old code — the
      fuzz framework is the price of the clean slate; rebuild it before
      calling anything done.)
- [ ] **T3.4** Suites: lang/modsys/opt/eq/params/multiline rebuilt for
      the new language, incl. the fixes the design mandates (aggregate
      let-position values are legal; `as` truncates constants like
      variables; bitwise compound assignments verified end-to-end).

## Phase 4 — kernel boundary and the std library

- [ ] **T4.1** Kernel audit: the prelude contains exactly the
      mechanism-required set (Option/Result+`?`, to_str + format sinks,
      panic hooks, allocator + rc glue, string primitives, raw per-target
      syscall tails). Nothing else. The standing law: **the kernel grows
      only when a language mechanism grows.**
- [ ] **T4.2** `std` = the reserved in-repo directory; `use std.io;`
      resolves by the single rule (first segment `std` → the reserved
      directory); every other `use` stays two-base relative.
- [ ] **T4.3** std.collections: Vec and Map as real packages (extracted,
      not copied, from the compiler's own source; the compiler consumes
      them afterwards). Map iteration order is **deterministic and
      documented** (D3).
- [ ] **T4.4** std.io: read_line, file read/write wrappers over the raw
      tails.
- [ ] **T4.5** json as a package on the new language; rho-pkg updated
      (manifests, lock, vendored path+git deps).
- [ ] **T4.6** (library, non-blocking) utf-8 package: code-point
      iteration and friends — a package, never the kernel.

## Phase 5 — sites and course

- [ ] **T5.1** Language home (site/): hero, tour, playground, spec
      reader — rebuilt around the new compiler; playground = compile on
      the main thread, execute in a worker (V8 worker-context miscompile
      still unreported — minimize and report upstream); phase-split caps
      120/20/10 s; fat functions to linear memory (`w_memmode`); deploy
      asset = one generation past the seed + `wasm-opt -Oz
      --enable-bulk-memory`.
- [ ] **T5.2** Course (the bilingual app): all live blocks re-pinned to
      the new language; the honest-limitation notes rewrite (aggregate
      let-position now legal; `?T` non-null taught as the one true
      absence form); editor stays single-source from the repo.
- [ ] **T5.3** (owner's call, do not self-deploy) add the deploy
      workflow and deploy the course site.

## Phase 6 — freeze and 0.1.0

- [ ] **T6.1** Full gate green: every leg, corpus differential, suites,
      fuzz, both sites building — all on the wasm self-hosting loop.
- [ ] **T6.2** Tag `v0.1.0` — the one and only version. Release zip:
      `rho-0.1.0-wasm32-wasi.zip`, binary named `rho.wasm`, SHA256SUMS,
      English RELEASE.md. Push/tag/deploy timing belongs to the owner.
- [ ] **T6.3** **Freeze.** boot and the language freeze together. From
      here the language grows no more; 0.1.0-era growth is libraries
      (utf-8, collections, io, net) and tooling quality (operand-stack
      emission, string pooling, linear-scan register allocation, escape
      analysis / rc-pair elimination — ordering decided when the freeze
      lands).

## Phase 7 — after 0.1.0: native compilation, in the std library

Not part of 0.1.0. Native compilation is a std-library-era workstream;
the language does not change when it lands.

- [ ] **T7.x** Native backends as std-library components: arm64-mac
      (Mach-O + ad-hoc codesign), amd64-linux + arm64-linux (static ELF,
      raw syscalls), own assemblers. The archive branch's bug ledgers
      (asm64/asm86 ten-pit tables, Mach-O kernel gates) are required
      reading — same iron, **read them, never copy them**.
- [ ] **T7.x** The native ring (self build, structure check, --version,
      native-compiled wasm hello, ring child, child rebuilds corpus) as
      a **time-capped step on an ephemeral CI runner** (background +
      poll + hard timeout; timeout = red; a wedge dies with the VM).
      Local gates stay build-only. Linux crossings stay build-only
      until a container exists. MCU remains a future backend: one
      emitter + assembler + image-writer triple, nothing in the
      language.

---

## The design (the law — ratified 2026-09-25)

### 1. Identity

A small, hand-forged systems language; one binary toolchain;
reference-counted memory; zero undefined behavior; deterministic
compilation. The long-term direction is production-grade — this is
deliberately **not** written into public docs. The mainline is
boot → wasm → wasm self-hosting → gradual refinement; 0.1.0 is the wasm
self-hosting loop alone. MCU targets are a future backend, zero design
weight.

### 2. Memory

- RC + weak; every heap block carries a 24-byte header
  `{rc, wrc, drop}`; rc→0 runs the generated destructor; wrc>0 keeps the
  header (dead) so weak can observe death. No GC ever promised; no
  borrow checking ever promised — an evolution slot, not a commitment.
- **Zero UB is law on the safe subset**; violations are defined panics.
  The only unsafe window is the `intrinsics.` namespace.
- **Allocations are zeroed** (make/new read as 0; includes reused
  blocks).
- Counting is pure-local syntax insertion (one retain per copy of a
  managed value, one release per destruction); redundant pairs may be
  eliminated later; container ownership walks elements only at the
  rc==1 death check.

### 3. Types (the eleven rules)

1. Literals adapt to their consumer; with no consumer the default is
   **fixed forever**: integers `i32`, floats `f64`.
2. `const` infers from its initializer; an annotation may pin any
   builtin type.
3. The only conversion is `as` (integer wrap/truncate/extend; float→int
   truncates toward zero, saturates out of range; enum→tag). `as` has
   **one** semantics — constants truncate exactly like variables.
4. traits: `impl` blocks and bare methods coexist; multiple impl blocks;
   methods/impls may be defined in any module.
5. **Function overloading** with exact-match-unique resolution
   (parameter types + count + receiver); zero or many matches = compile
   error naming candidates. Trait satisfaction = name + signature match.
6. Method visibility is **import-scoped**: native methods travel with
   their type; extension methods participate only from modules in the
   caller's use closure; same-signature conflicts are a compile error
   naming both modules.
7. `use` has one form: dotted `use a.b.c;` (segments are identifiers).
8. Trait bounds (`[T: Show]`) exist and are verified per instantiation.
9. **Pointers are non-null by default**; nullability requires `?T`.
   `?T` is sugar for `Option[T]` — one absence mechanism; `weak.get()`
   returns `?*T`; no smart casts (unwrap via match/methods).
10. `==` comparability law: pointers identity, strings content, enums
    tag-then-payload, aggregates element-wise; `fn`/`dyn`/err-payloads
    never compare; comparing cyclic data ends in a stack-overflow panic
    (documented, not detected).
11. Everything is a value type; no moves, no borrows, no address-of.

### 4. Errors

panic = a programmer bug, and **process-fatal**: no catch, no recover,
ever; no unwinding machinery. `defer` runs on every scope exit path
except panic. `Result[T,E]`/`Option[T]` + `?` are the only error
channel.

### 5. Determinism

- Same compiler + same version + same target + same input →
  **byte-identical output** (diagnostics, symbol order, folds included).
- Different backends / different compilers: bytes differ, **behavior
  must match** (stdout + exit).
- Library containers have a **deterministic, documented** iteration
  order.

### 6. Declarations and visibility

`let` immutable by default (`mut` explicit; initializer required);
inner shadowing allowed, same-scope rebind is an error; `static mut` =
module-lifetime global (the only global state; sync laws arrive with
threads); visibility = pub/private + package facades; declaration order
free within a module; static initializers acyclic.

### 7. Build parameters

Root-file consts are ordinary consts (zero declaration restrictions).
`--set name=value` overrides any const whose type has a text form (bool,
all integer widths, f32/f64, string; range-checked; refusal = exit 2).
Comptime-known conditions fold: only the live branch is checked and
emitted; dead branches skip whole; reachability prunes modules. Root
consts are visible in every module (prelude status); shadowing them is
an error.

### 8. Strings

Immutable UTF-8 byte slices; len/index/slice are byte-based; no built-in
char abstraction (a utf-8 **package** may add code-point iteration).
`+` concatenates, never coerces. printf/eprintf/format: literal format
string, `{}` counted at compile time, every value prints via `to_str`;
aggregates are not printable. Triple-quoted strings are fully verbatim.
Hot string building uses `[]u8` buffers (the `__fmt_build` pattern).

### 9. Control flow

`if`/`match` are expressions (same-type arms; `if` requires `else`);
conditions are `bool`, no truthiness; aggregate values in let position
are **legal** (the old runtime panic is a bug to fix, not a feature).
**Labels, Go form**: `outer: while … { break outer; }` — plain
identifier + colon on `while`/`loop`; `break`/`continue LABEL`;
labels are function-unique and live in their own namespace. No goto.
TCO: direct self tail calls become loops; `return_call` is out.
Match is exhaustive unless `_` is present.

### 10. Modules and packages

One file, one module; two lookup bases (user's directory, then the
entry's); exactly one real body (`lib.rho` and `x.rho` coexisting is an
ambiguity error); package = directory behind a `lib.rho` facade, interior
files package-private, subpackages closed; `pub use` four forms;
`main` reserved in the root; use bindings are private; methods/impls
definable anywhere (the old ownership rule is gone — coherence is
enforced at call sites by exact-match-unique).

### 11. Operators

Integers wrap; `MIN / -1 = MIN`; `/0` `%0` panic; shifts mask by the
left width; floats are IEEE-754 (`/0.0` = inf, `NaN != NaN`); precedence
C-style, 11 levels + postfix, left-associative; `&&`/`||` short-circuit;
assignment is a statement with no value.

### 12. Closures and variadics

Closures capture immutables by copy; **mut capture stays rejected**
(shared mutable state is a heap object: `new` a counter, pass `*T`).
Variadics: `rest: T...` last parameter, concrete element type, `[]T` in
the body, spread `xs...` last argument, call materializes a fresh slice,
variadic functions are not first-class values.

### 13. Toolchain architecture

boot = the complete design in C (the wasm backend only for 0.1.0), then
**frozen together with the language** — the design above is the ceiling;
growth from here is libraries only. The self-hosted compiler
(`libs/compiler`, one root, build parameters per §7) is written in rho
and graded behaviorally against boot. 0.1.0 ships **one backend:
wasm32-wasi**; native compilation joins afterwards as std-library
components (§ Phase 7), the language untouched. Trust root = **pure
source rebuild** (every gate run builds the seed from boot's C source);
the pinned `seed.wasm` remains as a byte-exact canary. Symbol policy:
lib artifacts keep public names and minify internals; cli/app artifacts
minify everything but `main`/`_start`; diagnostics never reference
mangled names; `-g` keeps full names. Optimizer backlog (ordered at
implementation time): operand-stack emission, string pooling, linear-scan
register allocation, escape analysis / rc-pair elimination.

### 14. Kernel and std

The prelude holds exactly the mechanism-required kernel (Option/Result
+`?`, to_str + format sinks, panic hooks, allocator + rc glue, string
primitives, raw per-target syscall tails). **The kernel grows only when
a language mechanism grows.** `std` is the reserved in-repo directory;
collections (Vec/Map, deterministic documented order) and io (read_line,
file wrappers) are packages; the compiler consumes them; json is a
package; utf-8 is a future package. The bootstrap chain vendors in-tree
packages only. After 0.1.0, native compilation also lives in the std
library (Phase 7).

### 15. Sites and delivery

Two sites, one source: the repo's `site/` is the language home (GitHub
Pages); the bilingual course references the project, never duplicates
it. Playground: compile on the main thread, run in a worker (V8
worker-context miscompile — report upstream); caps per phase
(boot 120 s, compile 20 s post-download, run 10 s); fat functions put
vregs in linear memory; deploy asset = web config one generation past
the seed + wasm-opt. Editor code flows one way: repo → app.

### 16. Version policy

One version: **0.1.0**, tagged when the wasm self-hosting gate is green.
No other tags, no intermediate numbers, no evolution after the freeze —
the design above is the whole language. Native compilation is
post-0.1.0 std-library work, not a version of the language.
