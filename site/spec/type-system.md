# The rho type system

Types, their rules, and how the checker enforces them — aligned with the
boot compiler (`boot/src/check.c`). Surface syntax lives in
[syntax.md](syntax.md).

## Types

### Primitive types

| Type | Meaning |
|------|---------|
| `bool` | `true` / `false` |
| `i8 i16 i32 i64 isize` | signed two's-complement integers, wrapping |
| `u8 u16 u32 u64 usize` | unsigned integers, wrapping |
| `f32 f64` | IEEE-754 floats |
| `string` | immutable UTF-8 byte slice; `==` compares contents, `+` concatenates |

Pointer/`usize` width is fixed per target: 64 bits on `amd64-linux` and
`arm64-mac`, 32 bits on `wasm32-wasi`.

### Composite types

| Syntax | Meaning |
|--------|---------|
| `[N]T` | fixed-size array of `N` elements, inline value |
| `[]T` | slice: owning view `{buf, ptr, len}` into a heap array |
| `*T` | owning reference to a heap object |
| `weak[T]` | weak reference that does not keep the object alive |
| `Name` / `Name[T, U]` | struct or enum, possibly generic |
| `fn(P1, P2) -> R` | function value: static fn pointer or closure |

Every type is a **value type**: assignment, argument passing and return
copy the value. Copying copies fields shallowly and retains every managed
reference inside. There is no implicit move, no borrow, no address-of
operator.

**Slices own their buffer** — and, transitively, their elements: a slice
value `{buf, ptr, len}` retains the backing array and every managed element
in it; releasing a slice releases the elements, then the buffer. A string is
a slice of `u8` with the same ownership rule; literals point at static,
immortal buffers. `null` is a literal of any `*T` or `weak[T]`; rc
operations on `null` do nothing, dereferencing it panics.

The language is memory safe: bounds are checked, the reference-count
discipline prevents use-after-free and double free, and raw memory access
exists only through the `intrinsics` namespace.

## Operators

- Integer arithmetic wraps (two's complement). `MIN / -1` wraps to `MIN`;
  `x / 0` and `x % 0` panic.
- Shifts accept any integer right operand, masked to the left width.
  Signed shifts are arithmetic.
- Floats follow IEEE-754; `/ 0.0` gives infinities, not panics.
- `+` on two strings concatenates (left-associative, the same precedence
  level as integer addition). Nothing coerces: a non-string operand is a
  type error, never an implicit `to_str` — build mixed lines with
  `format`, or splice `x.to_str()` by hand.
- `==`/`!=`: pointers compare identity, strings compare contents, enums
  compare tag then payload (compiler-generated, recursive). Element-wise
  `==` on structs, arrays and slices arrives with the self-hosted compiler.
  Not defined on `fn` values.
- `as` casts: between integer types (wrap/truncate/extend), float↔integer
  (truncates toward zero; out-of-range saturates), enum→tag integer.
  Known corner (both compilers agree, `tests/lang/opt/s07`): a narrowing
  cast of a fully constant expression keeps the value instead of
  truncating (`((1000 + 12) as u8)` is 1012, not 244) — the const lane
  pins the literal before the cast narrows it; narrowing a VARIABLE
  truncates as specified (corpus `051`).
  Nothing else.

## Functions

Parameters are typed, passed by value, and are mutable local copies. The
callee retains managed parameters on entry and releases them at exit —
passing a value never steals the caller's reference. The first parameter
named `self` makes the function a method. Declaration order does not matter
within a module.

### Generics

`fn`, `struct`, `enum` may take type parameters: `fn id[T](x: T) -> T`.
Instantiation is implicit and monomorphizing: each distinct concrete use
produces a specialized copy whose body is re-typechecked. There are no
trait bounds; a generic body may only use operations that each
instantiation satisfies — verified per instantiation. Recursive generic
instantiation is detected and rejected.

### Variadic functions

```
fn sum(ns: i64...) -> i64 { ... }        // ns has type []i64 in the body
fn max_of(first: i64, rest: i64...) -> i64
```

Rules, as enforced:

- The variadic parameter must be the last one; its element type must be
  **concrete** (no type parameters — `fn f[T](xs: T...)` is rejected).
- To the function's type the parameter is a plain `[]T`: the callee sees a
  slice, and `len`/indexing work on it directly.
- A call supplies zero or more extra arguments, each checked against `T`
  (untyped literals adapt): `sum(1, 2, 3)`.
- A spread call `sum(xs...)` passes an existing slice whole; `xs` must be
  exactly `[]T` after instantiation.
- The call site materializes the arguments as a fresh slice: an array is
  allocated, the values are stored (retaining managed elements), and the
  slice is handed to the callee, which retains and releases it like any
  parameter. A variadic call therefore allocates.
- A variadic function cannot be used as a value — `fn(P) -> R` has no `...`
  spelling — so it cannot be assigned, passed, or captured.

### Methods on primitive types

`self` may name a primitive: `fn i32.to_str(self: i32) -> string`.
Primitive methods live in one global table per primitive — a second
`fn <prim>.m` anywhere (including the prelude) is a duplicate-method error,
unlike struct methods, which are scoped per defining module. Method calls
auto-deref through pointers; untyped literal receivers (`5.to_str()`)
resolve at their default width (i32 / f64).

## Printing: `printf`, `format`, the `to_str` protocol

`printf`, `eprintf`, and `format` are compiler builtins (stdout, stderr,
and a string value):

```
printf("x = {}, y = {}\n", x, y);        // write to stdout
let s: string = format("({}, {})", x, y); // build the same string
```

- The format string must be a **string literal**.
- Each `{}` consumes one value; `{{` and `}}` are literal braces. A stray
  brace is a compile error, as is a `{}`/value count mismatch.
- Every value must be **printable**: it has a method `to_str(self) ->
  string`. Every primitive is printable via the prelude; user types print
  themselves by defining `to_str` — `format` is the canonical way to write
  that body:

  ```
  fn Point.to_str(self: *Point) -> string {
    return format("({}, {})", self.x, self.y);
  }
  ```

  Slices and arrays are not printable — print their elements in a loop.
- The call desugars in the checker to the prelude's variadic sinks
  (`__fmt_print` / `__fmt_eprint` / `__fmt_build`, each called as
  `sink("...", x.to_str(), ...)`), so all three ride the same slice
  machinery user variadics do. With no `{}` at all, `printf`/`eprintf`
  lower to the raw byte sink and allocate nothing, and `format` is the
  literal itself — no call, no copy.

Formatting is canonical, deterministic, identical on every target:

- Integers: minimal two's-complement decimal, correct at `MIN`.
- `bool`: `true` / `false`. `string`: the string itself.
- Floats: the exact decimal expansion, correctly rounded (half-to-even) to
  the round-trip digit count — 17 significant digits for `f64`, 9 for
  `f32` — trailing zeros stripped, integral values keep `.0`, fixed
  notation for decimal exponents in `[-4, D)`, scientific (`d.dddde±XX`)
  otherwise, `inf`/`nan` as such. The digits always suffice to recover the
  identical float.

`panic(msg)` takes a `string` only; compose with `format` or `+`.
`assert_eq` prints both values (via `to_str`) in its failure message.

## Traits, bounds, and `dyn`

A trait declares requirements; a type implements it when every requirement
has a matching method (docs/traits.md has the design):

```
trait Show {
  fn to_str(self) -> string,
}

impl Show for Point {
  fn to_str(self: *Point) -> string {
    return format("({}, {})", self.x, self.y);
  }
}
```

- Satisfaction is **computed from the method tables**: a hand-written
  `fn Point.to_str(...)` implements `Show` with no impl block, and every
  primitive implements the prelude's `Show` the moment it is declared.
  An `impl` block is method definitions plus an eager check — a missing
  or mismatched method is an error at the impl, named. The impl lives in
  the module owning the trait or the type; foreign × foreign is rejected.
- The receiver is implicit in a requirement (`fn m(self) -> R` has no
  type); the implementing method decides it. Signatures must agree
  exactly apart from the receiver.
- **Bounds**: `fn f[T: Show](v: *T)` documents the contract and is
  verified at each instantiation — a `T` without the required method
  fails anchored at the call site that demanded it. Bounds do not change
  codegen: monomorphization and static dispatch proceed as before.
- **`dyn Trait`** is a two-word value `{vtable, obj}` — the closure
  pair's shape. Coercion from `*T` is implicit wherever an expected type
  is known (let annotations, arguments, returns, field initializers,
  assignments), and only from pointer types whose element satisfies the
  trait; enums and primitives are not dyn-able yet (they are values, not
  heap objects). Method calls load the vtable slot and dispatch at
  runtime through the closure ABI; `==` on two dyn values compares the
  object pointers. The vtable is an immortal heap block built once per
  coercion site (one small allocation — the honest v1 cost); releasing a
  dyn value is a plain count operation, because every object's RC header
  already carries its own drop glue.
- `[]dyn Trait` works: slices stride by the 16-byte element, so
  heterogeneous collections dispatch correctly.

## Errors: Result, Option, `?`, panic

```
enum Result[T, E] { Ok(T), Err(E) }
enum Option[T]    { Some(T), None }
```

`expr?` requires a `Result[T, E]` or `Option[T]`: the success value
unwraps; failure expands to `return Err(e)` / `return None` against the
enclosing function's return type. `panic` prints its message plus source
location to stderr and exits with code 101. Panics unwind nothing — `defer`
does not run on panic (deliberate: panic is for bugs, not control flow).
