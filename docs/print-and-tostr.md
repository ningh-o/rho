# print, `to_str`, and printing by type (0.3.0)

## The problem

Through 0.2.1 the console surface was `print(s: string)`, `print_int(v: i64)`,
`print_ln()`, `print_bytes(b: []u8)`, `eprint(s: string)` — one function per
type, named after it:

- `print_int` took `i64` only, so every smaller integer needed `as i64`
  at the call site. The corpus literally returned results as exit codes
  instead of printing them.
- No way at all to print `bool`, `f32`/`f64`, `u8`..`u32`, or a user value.
- `print_int` was triplicated across the three preludes (hosted, wasi,
  esp32c3) with only the byte sink differing — formatting logic, which is
  pure, was copy-pasted per target.
- It was wrong at the edge: `print_int(i64::MIN)` negated into itself and
  emitted garbage digits.
- The self-hosted compiler grew `itoa`, `cat`..`cat5`, `g_digits` (an exact
  bigint float formatter) as private workarounds — proof the language
  lacked its own vocabulary.

## Alternatives considered

**Overloading** (several `fn print` per module, resolved by argument type).
Rejected: rho resolves calls by name alone today, everywhere (modules,
methods, associated fns, fn values). Overloads add a resolution ranking
(exact / widening / literal adaptation) that interacts badly with
monomorphizing generics — is `print[T](x: T)` an overload of
`print(s: string)`? — and every rule would have to be implemented twice,
byte-compatibly, in the boot and self-hosted checkers. A whole type-system
concept bought for one family of functions.

**Macros** (varargs `print!`, compile-time expansion). Rejected: a macro
subsystem (definition syntax, hygiene, expansion ordering) is larger than
everything this solves, and rho's pillar is one small grammar with no
expansion phase between parse and check.

**Checker magic** (`print` a compiler builtin that formats per type).
Rejected: a magic *name* — the moment a user shadows it the language lies;
and the formatting rules would live in the checker instead of in rho, where
they cannot be reused (`to_str(x)` in your own code) or taught.

**Chosen: `print` is library code over a type-directed method protocol.**
No overloading, no macros, no magic. The enabling language feature is small
and uniform: **methods on primitive types**.

## The design

### 1. Methods on primitive types (the language feature)

```
fn i32.to_str(self: i32) -> string { ... }
fn f64.to_str(self: f64) -> string { ... }
```

- Declaration: `fn <prim>.<name>(self: <prim>, ...)` parses today (primitive
  names are identifiers, not keywords); the checker now accepts it instead
  of requiring `self` to be a struct/enum.
- Resolution: `x.m(...)` where `x` has a primitive type resolves `m` in that
  primitive's method table (pointers to a primitive auto-deref, as for
  structs). `n.to_str()`, `flag.to_str()` are ordinary calls.
- Registration is **global per primitive**: a duplicate `fn <prim>.m`
  anywhere after an existing one is an error (`primitive methods are
  global`). The prelude registers `to_str` on every primitive; user code
  may add new methods, and may define `to_str` on their own types, but
  cannot silently replace the prelude's.
- Structs/enums keep their existing per-type methods; nothing changes.

### 2. The `to_str` protocol (the convention)

A type is **printable** when it has a method `to_str(self) -> string`.

- Every primitive is printable via the prelude: `string` is the identity,
  `bool` yields `true`/`false`, integers yield minimal two's-complement
  decimal (correct at `MIN` — the magnitude is taken as `u64` after the
  wrapping negation, so `i64::MIN` prints `-9223372036854775808`), floats
  see §4.
- A user type prints itself by defining `to_str`; `cat` (string
  concatenation, §3) exists precisely so writing one is a few lines.
- Slices and arrays have no default `to_str` (no syntax for methods on
  unnamed types yet) — printing one element-wise in a loop is ordinary
  code; the honest error is `no method `to_str` for `[]i32``.

This is Go's `Stringer`: one method, checked at instantiation, no trait
machinery, no bounds syntax.

### 3. The print family (the surface)

```
fn print[T](x: T)     // x.to_str() to stdout
fn println[T](x: T)   // x.to_str() + '\n' to stdout
fn eprint[T](x: T)    // x.to_str() to stderr
fn cat(a: string, b: string) -> string
fn assert_eq[T](a: T, b: T)   // spec §8 finally honored; failure eprints both
```

- `print[T]`'s body is `__print_str(x.to_str())`. Because generics
  monomorphize per call site and the body is re-checked under the concrete
  `T`, `x.to_str()` binds to the receiver type's method — this IS the
  "print is sugar for the type's method" semantics, carried by the existing
  instantiation machinery rather than a special case.
- `print("literal")` instantiates `print[string]`; `string.to_str` is the
  identity, so string programs are unchanged.
- `panic(msg: string)` stays string-only: panic text is programmer-composed
  and `cat` composes it.
- `print_int`, `print_ln`, `print_bytes` are removed. (`__print_str` /
  `__eprint_str` are the per-target byte sinks — see §5.)
- Integer literals adapt through the generic as everywhere else:
  `print(42)` is `print[i32]`.

### 4. Float formatting (the exact rule)

`f64.to_str` / `f32.to_str` produce the shortest-fixed form of the value's
**exact** decimal expansion, quantized to the round-trip digit count, with
trailing zeros stripped and `.0` kept on integral values:

- D digits, correctly rounded (half-to-even) from the exact expansion:
  **D = 17 for f64, D = 9 for f32** — by the round-trip theorem that is
  exactly enough to recover the identical float, and never more than
  necessary to be honest.
- Fixed notation when the decimal exponent is in `[-4, D)`, scientific
  (`d.dddde±XX`, two-digit exponent) otherwise. `0.1 + 0.2` prints
  `0.30000000000000004`; `1e300` prints `1e+300`; `2.5` prints `2.5`;
  `4.0` prints `4.0`; `inf`/`nan` print as such.

The algorithm is the self-hosted compiler's proven `g_digits` — exact
bigint arithmetic (`m2 * 2^e2` expanded in base 10^9 limbs), lifted verbatim
into the prelude core. One formatter, one truth: the language prints floats
the way its own compiler prints canonical goldens, and the implementation
is pure integer code, identical on all targets (esp32 soft-float included —
only `intrinsics.f64_bits`/`f32_bits` touch the value).

### 5. Prelude structure (the sync)

The three preludes shared ~140 duplicated lines; formatting logic was
copy-pasted per target. Now:

```
boot/prelude/core.rho     pure, target-independent, prepended to every prelude:
                          Option/Result + methods, to_str on every primitive
                          (integers, bool, f32/f64 via the bigint formatter),
                          cat, print/println/eprint (generic), assert,
                          assert_eq, panic, __panic_* helpers
boot/prelude/hosted.rho   tail: libc externs, __alloc/__free, __print_str,
                          __eprint_str (write(2))
boot/prelude/wasi.rho     tail: fd_write/proc_exit externs, bump __alloc,
                          __print_str/__eprint_str (iov)
boot/prelude/esp32c3.rho  tail: MMIO __print_str/__eprint_str, bump __alloc,
                          semihost exit, soft-float IEEE helpers (RV32)
```

`tools/embed.py` concatenates core + tail per target into `prelude_data.c`;
the self-hosted compiler reads `core.rho` + the selected tail from disk and
concatenates them the same way (it never embedded its own copy).

### 6. Dead-function elimination (why it gates this design)

`lower_program` emitted every non-template function in every module — with
a ~600-line prelude core, every hello world would ship the bigint float
formatter. 0.3.0 adds a post-lowering reachability pass over the IR:

- Seed: the entry symbol (`main`), plus statics' referenced functions.
- Edges: every `callee` reference in every IR instruction of a kept
  function (direct calls, address-taken fn/global refs, shims).
- esp32c3 keeps the soft-float helper set unconditionally (the RV32
  backend calls them by symbol from emitted code).
- Kept order is the original emission order (map order, deterministic);
  the pass never reorders, so the twice-compile determinism invariant is
  untouched.
- Implemented identically in boot and self — byte-identical output is the
  bootstrap gate, so the pass itself is pinned by `boot(self) == self(self)`.

### 7. Diagnostics at the call site

A failed `print(x)` (type without `to_str`) is an error *inside the
instantiated clone*, which without help would point into the prelude. The
checker records each instantiation's triggering call site; any diagnostic
raised while checking an instantiation body is re-anchored to that site
with a `(while instantiating print[*Point])`-style note. The user sees the
error where they wrote it.

### 8. Testing (the referee)

- Corpus: a print matrix program (every primitive type incl. `i64::MIN`,
  `u64::MAX`, bool, floats incl. `0.1+0.2`, `inf`/`nan`, scientific range),
  a user `to_str` program, `println`/`eprint`/`cat`/`assert_eq` coverage —
  run on `arm64-mac`, `wasm32-wasi`, `esp32c3`, plus the browser-compiler
  path (`make test-site`).
- Diag goldens: struct without `to_str` (call-site anchored), duplicate
  primitive method, `print()` arity, slice print.
- Frontend/AST goldens: primitive-method declarations and calls.
- Bootstrap: `boot(self) == self(self)` on arm64 and wasm, byte for byte;
  `boot(corpus) == self(corpus)`.
- Determinism: twice-compile comparison continues to hold with DCE on.
- CI (ubuntu + macos-14) runs `make test`; wasm runs via `make test-site`
  locally (wasmtime) — linux is gated by CI on push.

## What this does NOT add

No overloading, no macros, no trait/bound syntax, no auto-derived struct
`to_str`, no slice/array `to_str`, no format strings, no float parsing.
The extension path is recorded: methods on unnamed types would need a type
syntax in the `fn` name; shortest-round-trip floats would need a decimal
parser to verify candidates.
