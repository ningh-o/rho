# The rho Language Specification (v0.3)

rho (ρ) is a small, statically typed, compiled systems language. It has one
implementation goal per era: a boot compiler written in C through 0.0.x, and
a self-hosted compiler written in rho from 0.1.0 onward, with no C source in
the shipped toolchain.

This file is the overview and the runtime contract. The surface grammar
lives in [syntax.md](syntax.md), the type rules in
[type-system.md](type-system.md), and program composition in
[module-system.md](module-system.md).

Packages (path + git dependencies, consumed from an in-tree `vendor/` under
manifests and a reproducible lockfile kept by the separate `rho-pkg` tool)
are deliberately outside the language contract; their design lives in
[docs/package-manager.md](../docs/package-manager.md).

Design pillars:

1. **One binary toolchain.** `rho` does build/run/test/fmt/check. No external
   build system, no headers, no package ceremony.
2. **Direct code generation.** The compiler emits machine assembly and wasm
   itself. No LLVM, no C transpilation.
3. **Reference-counted memory, zero garbage-collection pauses.** Every heap
   object carries a reference count; the compiler inserts retain/release.
   Weak references break cycles; the programmer is responsible for cycles.
4. **Zero undefined behavior.** Variables are zero-initialized, integer
   overflow wraps (two's complement), slices are bounds-checked, and the
   memory model above makes use-after-free impossible for managed references.
5. **Determinism.** The same source compiled twice by the same compiler
   produces byte-identical output. Output never depends on hash iteration
   order, addresses, or timestamps.

---

## 1. Memory model

### 1.1 Object layout

Every heap allocation begins with a header:

```
{ rc: usize, wrc: usize, drop: fn(*void) }
```

`rc` is the strong count, `wrc` the weak count, `drop` the compiler-generated
destructor (releases fields recursively; may be trivial). When `rc` reaches 0
the destructor runs and the object storage is freed; if `wrc > 0` the header
survives (marked dead) so weak references can observe death. `rc_inc` on a
dead or `null` object does nothing; `rc_dec` on `null` does nothing.

Static data (string literals, const arrays) uses a header with `rc` set to
an immortal sentinel; decrements leave it alone.

### 1.2 The counting rules

The compiler enforces a uniform convention — *each copy of a managed value
takes one retain, each destruction takes one release*:

- `let x: *T = expr;` — the expression transfers its +1 into `x`.
- Reassignment `x = expr;` — retain the new value, release the old.
- Passing an argument — the callee retains the parameter on entry and
  releases it at body exit; the caller's reference is never stolen.
- `return expr;` — the value transfers its +1 to the caller.
- Scope exit — every managed local is released, in reverse declaration order.
- Struct/enum copy — managed fields are retained; destruction releases them
  (the generated `drop` glue).
- Slice/string copies — the slice value retains the buffer and every managed
  element; releasing runs the elements first, then the buffer.

This is purely local, syntactic insertion — no whole-program analysis is
required for correctness. Redundant pairs are eliminated later (§4.4).

### 1.3 weak references

`weak.from(p: *T) -> weak[T]` creates a weak reference (bumps `wrc` only).
`w.get() -> *T` returns the object or `null` if it died. Weak references
never resurrect; use them on the child side of parent↔child cycles.

### 1.4 Escape analysis (allocation placement)

A `new`/`make` whose result provably stays within the function — never
returned, never stored into another heap object, slice, static or closure
environment, never passed to a call — may be allocated on the stack with
its count traffic elided. Semantics are unchanged; this is an optimization
the compiler may apply at any time after 0.0.5.

## 2. Toolchain

```
rho build [file]  [-o out] [--target t]   compile & link (default ./main.rho)
rho run   [file] [-- args...]              build to temp & run
rho test  [file|dir] [--target t]          build & run all corpus tests
rho fmt    [-w] [file|dir]                 print/form canonical formatting
rho check  [file]                          type-check only
rho --version
```

`rho test` pairs each `<name>.rho` with `<name>.out` (plus an `// exit: N`
comment pin) — a file passes when stdout and exit code match exactly.
`test_*` function runners (pre-0.3 style) are superseded by the corpus.

Formatting is canonical: one true style, no configuration. `rho fmt`
rewrites source so that `fmt` is idempotent and `parse(fmt(parse(x))) ==
parse(x)`.

## 3. Compilation model

### 3.1 Pipeline

```
lex → parse → resolve+monomorphize+typecheck (+ printf/format desugar)
    → lower to SSA IR → per-target codegen
```

The IR is in strict SSA form: every instruction produces a unique virtual
register; control-flow joins use φ nodes. Control flow in the IR is
structured (if/else diamonds, while loops, returns) — a consequence of the
source grammar, which keeps wasm lowering direct and the CFG reducible.

`printf`/`eprintf`/`format` calls are rewritten during checking
(type-system.md): each `{}` becomes the corresponding value's `to_str()`,
and the call becomes a variadic `__fmt_print(...)` / `__fmt_eprint(...)` /
`__fmt_build(...) -> string` — so no backend knows the verbs exist.

### 3.2 Backends

| Target | Codegen | Host tool used |
|--------|---------|----------------|
| `wasm32-wasi` | binary module, WASI preview1 imports | none (runs on wasmtime/node/browser) |
| `arm64-mac` | static Mach-O, ad-hoc signed, in-tree | none — `build` writes a runnable image |
| `amd64-linux`, `arm64-linux` | static ELF64, no interpreter, no libc | none — in-tree assembler + linker |
| `amd64-mac` | SysV assembly (`.s`) | Rosetta test vehicle |

The tool never shells out to an assembler, linker, or codesign: image
targets write a runnable file in one process; `run`/`test` execute
`wasm32-wasi` through a WASI runtime and native images on matching hosts
(`arm64-linux` under qemu-user counts as matching).

Register allocation is spill-everything at present (every virtual register
gets a stack slot); a linear-scan allocator replaces it after self-hosting.
Same-size bit moves (`usize`↔pointer, f32/f64 bit extraction) are plain
copies. Aggregates are passed by reference to a caller-made temporary (a
documented deviation from the C ABI, which only matters for `extern`
functions — hosted externs use scalars and pointers only).

### 3.3 Emission: unreachable code is dropped

After lowering, a reachability pass over the IR keeps the entry function,
every function referenced by static initializers, the runtime helpers the
backend calls by symbol, and everything transitively reachable. Dropped
functions never appear in the artifact — prelude formatting machinery a
program never prints with costs nothing. The same walk now keeps only the
globals a surviving definition references (an unreferenced static is pure
dead data — its data segment, bss slot and attached string-literal block
all go), so a wholly-unused `use`d module or an untouched public static
costs nothing either. Keeping is transitive across both maps: a kept
global's relocation words can name functions and other globals, a kept
function's callees can name either. The pass preserves emission order,
so determinism is untouched.

### 3.4 rc-pair elimination

After lowering, a peephole pass on the IR removes `rc_inc(x)` immediately
following `rc_dec(x)` (same value, no intervening call that could release
the last reference), and merges count traffic on slots proven local by
escape analysis. The pass must preserve observable behavior: the only
observable effect of rc traffic is death timing, which only weak references
and finalizers can observe — and rho has neither before 0.2.

### 3.5 Determinism

Monomorphization order, symbol emission order, and data layout are
canonically sorted. Compiling the same inputs twice yields byte-identical
artifacts — a tested invariant (corpus builds are compared twice).

### 3.6 Constant folding and dead instructions

After the reachability pass (§3.3), the self-hosted compiler folds the IR:
an integer binop, comparison, width cast, or pointer `+imm` whose operands
are all compile-time constants is rewritten in place into a `const` with
the same destination (uses never change); instructions whose result no
remaining read references are dropped. Both passes run to a bounded
fixpoint in emission order — the output stays deterministic, and `--no-opt`
disables them.

Folding is semantics-preserving by law, not by luck:

- Wrap arithmetic folds exactly as the runtime wraps (`tests/lang/opt`
  pins both directions at the i32 edges).
- Shift counts mask by the lane width minus one, as the hardware does.
- Signed division and remainder with divisor −1 fold to `0 − a` — the
  wrap law the backends already emit (corpus `042_divrem_signed`).
- **Division by a constant zero never folds**: the runtime panic is the
  program's observable behavior, and the fold must not erase it.
- Float arithmetic never folds.

Boot has no optimizer (the seed stays frozen); the gate's differential
legs grade every corpus program boot-built vs mirror-built for identical
stdout and exit code, so a misfold is a red gate, not a silent change.

## 4. Version policy

- 0.0.x — boot compiler era (C). The language surface **only grows**.
- 0.0.5 is the freeze (declared with 0.0.5f): the 0.1.0 self-hosted
  compiler must be writable in exactly the language 0.0.5 defines.
  Escape analysis is explicitly deferred to the self-hosted compiler;
  element-wise aggregate `==` was deferred with it and has since arrived
  there (Operators, type-system spec) — boot still rejects it, frozen.
- 0.1.0 — self-hosting. The C compiler is thereafter frozen forever; it
  exists only to seed new-host bootstraps. Language evolution continues in
  the self-hosted compiler only.
- 0.3.0 — printing by type: the `to_str` protocol on every primitive.
- 0.3.1 — the esp32c3 backend is removed; native targets emit assembly and
  stop; `run`/`test` go through a WASI runtime.
- 0.3.2 — native images end to end: in-tree assemblers, static ELF and
  Mach-O writers, ad-hoc code signature, freestanding runtime blobs.
- 0.3.3 — formatted printing: `printf`/`eprintf` with `{}` placeholders,
  and variadic functions (`rest: T...`, spread `xs...`). `print`, `println`
  and `eprint` are removed.
- 0.3.4 — `format(fmt, ...) -> string`: the same desugar pointed at a
  string sink, so a `to_str` body is one format line.
- 0.4.0 — traits, bounds, and `dyn`: name-satisfied requirements,
  `[T: Trait]` verified per instantiation, and runtime dispatch through
  per-trait vtables riding the closure ABI (docs/traits.md).
- Bootstrap invariant, tested continuously once the mirror catches up
  (0.3.x, see `docs/todo.md`): `boot(self) == self(self)` byte for byte
  (stage2 == stage3), and `boot(corpus) == self(corpus)`.
