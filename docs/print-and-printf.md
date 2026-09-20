# printf, format, variadics, and printing by type (0.3.3–0.3.4)

0.3.0 made printing type-directed (`to_str` on every primitive, generic
`print`/`println`/`eprint` — the design history is in git). 0.3.3 replaces
that surface with **formatted printing**: `printf` and `eprintf` fill each
`{}` in a literal format with the next value's `to_str`, and the language
grows **variadic functions** to carry it — user code can declare them too.

## The problem

One value per call was tolerable; composing a line was not:

```
print("sum: ");
print(n.to_str());     // to_str visible at every call site
print("\n");
```

Three statements, one line of output, and the string-conversion protocol
leaks into every program's foreground. `cat` chains were worse
(`cat(cat(..))`), and `println` only ever took one value.

## Alternatives considered

**Keep `println`, add more verbs** (`println2`, `print_pair`...). Rejected:
the verb count grows with arity needs and none of them compose.

**`print[T](xs: []T)` with slice literals.** Rejected: rho has no slice
literals, and even with them, `print(["sum: ", n.to_str()])` still makes
the caller do the conversion — the exact leak the round removes.

**True C-style `printf` with `%d`-per-type directives.** Rejected: `%d`
vs `%lld` vs `%zu` is a bug factory; rho already knows each value's type,
so directives would restate (and be able to contradict) what the checker
knows. A single `{}` keeps formatting type-directed and the format string
free of type grammar.

**Boxed formatting** (`fmt(fmt, args: []Value)` with a prelude `Value`
enum and implicit boxing at variadic positions). Rejected: an implicit
conversion rule that exists only for one prelude type — a type-system
special case wearing a library costume.

**Chosen: `{}`-placeholders over `to_str`, desugared in the checker onto
variadic functions.** One printable protocol (unchanged from 0.3.0), one
new calling feature (variadics — real, user-usable, no magic), and printf
itself is two lines of rewriting in the checker.

## The design

### 1. Variadic functions (the language feature)

```
fn sum(ns: i64...) -> i64 { /* ns: []i64 here */ }
fn join(sep: string, parts: string...) -> string { ... }
```

- `rest: T...` must be the **last** parameter; its element type must be
  concrete (no `T...` inside generics — monomorphizing a tuple of types is
  a much larger machine than this round needs).
- To the function's *type* the parameter is a plain `[]T`: the body sees a
  slice, and every existing slice operation (`len`, index, sub-slice,
  escape via copy) applies unchanged.
- A call supplies zero or more extra arguments (`sum(1, 2, 3)`), or one
  **spread** (`sum(xs...)`) passing an existing `[]T` whole — spread must
  be the last argument and match the element type exactly.
- The call site materializes the extra arguments as a fresh slice:
  allocate, store (retaining managed elements — slice values own their
  elements), call, release. A variadic call therefore allocates; that is
  the honest cost and it is documented, not hidden.
- A variadic function cannot be used as a value: `fn(P) -> R` has no `...`
  spelling, so there is no way to call it except directly — which is
  exactly the calling form the lowering supports.

Because the desugared parameter is an ordinary slice, **no backend changes
at all** — wasm, arm64, amd64 pass one more aggregate argument, which they
already knew how to do.

### 2. printf / eprintf / format (the surface)

```
printf("x = {}, y = {}\n", x, y);   // stdout
eprintf("bad: {} (code {})\n", why, n); // stderr
let s: string = format("({}, {})", x, y); // the same string, as a value
```

- The format string must be a **literal**. The checker splits it at
  compile time: `{{`/`}}` are literal braces, `{}` is a placeholder, a
  stray brace is an error, and the placeholder count must equal the value
  count — the arity bugs C's printf is famous for cannot compile here.
- Each value is rewritten to `<value>.to_str()`; a type without `to_str`
  reports `no method to_str for ...` anchored at the value's own
  position, exactly as a hand-written call would.
- The whole call becomes `__fmt_print("...", x.to_str(), "...", ...)`
  (prelude, variadic over `string`) — or, with zero placeholders, the raw
  byte sink `__print_str(lit)`, so `printf("hello, world\n")` allocates
  nothing.
- **`format` is the same desugar pointed at a string sink** (0.3.4): the
  call becomes `__fmt_build(...) -> string`, a two-pass join (total
  length, then one copy) with no intermediate strings; with zero
  placeholders the call is the literal itself — no call, no copy. It is
  the canonical way to write a `to_str` body:

  ```
  fn Point.to_str(self: *Point) -> string {
    return format("({}, {})", self.x, self.y);
  }
  ```

  `cat` stays binary and low-level on purpose — one way to assemble a
  string at the surface, and it is not `cat`.
- `print`, `println`, `eprint` are **removed**. They were the 0.3.0
  surface; `printf("{}", x)` is the same call with one honest syntax.

The sinks are three lines of prelude each:

```
fn __fmt_print(parts: string...) {
  let mut i: usize = 0;
  while i < len(parts) { __print_str(parts[i]); i += 1; }
}
```

The prelude itself uses `eprintf` in `panic` — dogfooding in the same
file that defines the feature.

### 3. What carries over unchanged from 0.3.0

- The `to_str` protocol (a type is printable when it has
  `to_str(self) -> string`), primitive methods and their global
  registration, user `to_str` via `cat`, and the honest
  no-`to_str`-for-slices error.
- Float formatting: the exact-decimal round-trip formatter — D = 17 for
  f64, 9 for f32, half-to-even, fixed in `[-4, D)`, scientific otherwise,
  `inf`/`nan` as such. `{}` prints exactly what `println` printed.
- Dead-function elimination: a program that never prints still ships none
  of the formatter.

### 4. Untyped literals as receivers

`printf("{}", 42)` rewrites to `42.to_str()`. Method resolution now treats
an untyped literal receiver as its default width (`i32` / `f64`) so the
call resolves; the existing method-arg adapter then materializes the
constant at `self`'s width. This also makes `5.to_str()` legal as
hand-written source, which it quietly was not before.

### 5. Testing (the referee)

- Corpus: `027_variadics` (user variadics incl. zero-arg, spread,
  fixed+variadic mix, a variadic returning a string), `028_printf`
  (every printable kind, brace escapes, expressions, eprintf) and
  `029_format` (struct/enum/nested `to_str` via `format`, string
  asserts, the zero-placeholder literal) run on `wasm32-wasi` and the
  native image targets; the 0.3.0 print matrix files were rewritten in
  place with byte-identical outputs.
- Diag goldens: variadic arity, printf placeholder/value mismatch,
  non-literal format, stray brace, spread type mismatch,
  not-the-last-parameter, no-`to_str` (call-site anchored).
- Frontend/AST goldens: declarations, calls, spread, fmt roundtrip.
- Determinism: twice-compile comparison holds with the desugar and the
  slice-materializing calls.

## What this does NOT add

No `%`-style directives, no width/precision flags, no runtime format
strings (the literal rule is the honest v0 — a runtime format needs a
`format(fmt, parts...) -> string` verb and a scanner in the prelude),
no generic variadics, no variadic fn values. The extension path is
recorded here for whoever needs one of those.
