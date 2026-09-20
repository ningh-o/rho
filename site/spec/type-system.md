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
| `string` | immutable UTF-8 byte slice; `==` compares contents |

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
- `==`/`!=`: pointers compare identity, strings compare contents, enums
  compare tag then payload (compiler-generated, recursive). Element-wise
  `==` on structs, arrays and slices arrives with the self-hosted compiler.
  Not defined on `fn` values.
- `as` casts: between integer types (wrap/truncate/extend), float↔integer
  (truncates toward zero; out-of-range saturates), enum→tag integer.
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

## Printing: `printf`, the `to_str` protocol

`printf` and `eprintf` are compiler builtins (stdout / stderr):

```
printf("x = {}, y = {}\n", x, y);
```

- The format string must be a **string literal**.
- Each `{}` consumes one value; `{{` and `}}` are literal braces. A stray
  brace is a compile error, as is a `{}`/value count mismatch.
- Every value must be **printable**: it has a method `to_str(self) ->
  string`. Every primitive is printable via the prelude; user types print
  themselves by defining `to_str`. Slices and arrays are not printable —
  print their elements in a loop.
- The call desugars in the checker to the prelude's variadic sinks
  (`__fmt_print("...", x.to_str(), ...)`), so printing rides the same
  slice machinery user variadics do. With no `{}` at all it lowers to the
  raw byte sink and allocates nothing.

Formatting is canonical, deterministic, identical on every target:

- Integers: minimal two's-complement decimal, correct at `MIN`.
- `bool`: `true` / `false`. `string`: the string itself.
- Floats: the exact decimal expansion, correctly rounded (half-to-even) to
  the round-trip digit count — 17 significant digits for `f64`, 9 for
  `f32` — trailing zeros stripped, integral values keep `.0`, fixed
  notation for decimal exponents in `[-4, D)`, scientific (`d.dddde±XX`)
  otherwise, `inf`/`nan` as such. The digits always suffice to recover the
  identical float.

`panic(msg)` takes a `string` only; compose with `cat`. `assert_eq` prints
both values (via `to_str`) in its failure message.

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
