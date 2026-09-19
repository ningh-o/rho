# The rho Language Specification (v0.0.x)

rho (ρ) is a small, statically typed, compiled systems language. It has one
implementation goal per era: a boot compiler written in C through 0.0.x, and a
self-hosted compiler written in rho from 0.1.0 onward, with no C source in the
shipped toolchain.

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

## 1. Lexical structure

Source files are UTF-8, extension `.rho`. Whitespace separates tokens.
Comments run from `//` to end of line.

Keywords:

```
fn let mut if else while loop break continue return defer
struct enum use pub static const match as new null true false weak self
```

Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Reserved: keywords above, plus the
primitive type names (§2.1) and builtin names (§10).

Integer literals: decimal (`12345`), hex (`0xFF`), binary (`0b1010`),
optionally with `_` separators (`1_000_000`). An unsuffixed integer literal
has no fixed type; it adapts to the expected integer type in context and
defaults to `i32`.

Float literals: `3.14`, `1e9`, `2.5e-3`. No fixed type; adapts to `f32` in
context, defaults to `f64`.

String literals: `"..."` with escapes `\n \t \r \\ \" \0 \xNN`. String
literals are immutable static data.

Operators and punctuation:

```
+ - * / %  == != < > <= >=  && || !  & | ^ << >>  ~
= += -= *= /= %= &= |= ^= <<= >>=
-> => ? . , : ; ( ) { } [ ]
..  (slice range)
```

## 2. Types

### 2.1 Primitive types

| Type | Meaning |
|------|---------|
| `bool` | `true` / `false` |
| `i8 i16 i32 i64 isize` | signed two's-complement integers, wrapping |
| `u8 u16 u32 u64 usize` | unsigned integers, wrapping |
| `f32 f64` | IEEE-754 floats |
| `string` | immutable UTF-8 byte slice; `==` compares contents |
| `usize` | indices and lengths; same width as the target pointer |

Pointer/`usize` width is fixed per target: 64 bits on `amd64-linux` and
`arm64-mac`, 32 bits on `wasm32-wasi`.

### 2.2 Composite types

| Syntax | Meaning |
|--------|---------|
| `[N]T` | fixed-size array of `N` elements, inline value |
| `[]T` | slice: owning view `{buf, ptr, len}` into a heap array |
| `*T` | owning reference to a heap object (§3) |
| `weak[T]` | weak reference that does not keep the object alive |
| `Name` / `Name[T, U]` | struct or enum, possibly generic |
| `fn(P1, P2) -> R` | function value: static fn pointer or closure |

Every type is a **value type**: assignment, argument passing and return copy
the value. Copying a value copies its fields shallowly and increments the
reference count of every managed reference it contains. There is no implicit
move, no borrow, no address-of operator.

**Slices own their buffer.** A slice is `{buf: *void, ptr: *T, len: usize}`
where `buf` is a managed reference to the backing array object. Copying a
slice increments `buf`'s count; dropping decrements it. A string is a slice
of `u8` with the same ownership rule; literal strings point at static,
immortal buffers.

`null` is a literal of any `*T` or `weak[T]` type. `rc` operations on `null`
do nothing; dereferencing `null` panics.

### 2.3 Memory safety

The language is memory safe. Bounds are checked (panic on violation), the
reference-count discipline prevents use-after-free and double free, and there
is no address-of, no pointer arithmetic, and no unchecked casts between
pointer types. Raw memory access exists only through the `intrinsics`
namespace (§10), which the std library wraps; application code does not need
it.

## 3. Memory model

### 3.1 Object layout

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

### 3.2 The counting rules

The compiler enforces a uniform convention — *each copy of a managed value
takes one retain, each destruction takes one release*:

- `let x: *T = expr;` — the expression transfers its +1 into `x`.
- Reassignment `x = expr;` — retain the new value, release the old.
- Passing an argument — caller retains; the callee releases at body exit.
- `return expr;` — the value transfers its +1 to the caller.
- Scope exit — every managed local is released, in reverse declaration order.
- Struct/enum copy — managed fields are retained; destruction releases them
  (this is the generated `drop` glue).
- Slice/string copies — the embedded `buf` is retained/released.

This is purely local, syntactic insertion — no whole-program analysis is
required for correctness. Redundant pairs are eliminated later (§11.2).

### 3.3 weak references

`weak.from(p: *T) -> weak[T]` creates a weak reference (bumps `wrc` only).
`w.get() -> *T` returns the object or `null` if it died. Weak references
never resurrect; use them on the child side of parent↔child cycles.
Dereferencing a dead weak pointer is impossible: `get` returns `null`.

### 3.4 Escape analysis (allocation placement)

A `new`/`make` whose result provably stays within the function — never
returned, never stored into another heap object, slice, static or closure
environment, never passed to a call — is allocated on the stack, its header
and count traffic elided. Semantics are unchanged; this is an optimization
the compiler may apply at any time after 0.0.5.

## 4. Expressions

### 4.1 Precedence (loosest to tightest)

| Level | Operators |
|-------|-----------|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `==` `!=` |
| 4 | `<` `>` `<=` `>=` |
| 5 | `\|` |
| 6 | `^` |
| 7 | `&` |
| 8 | `<<` `>>` |
| 9 | `+` `-` |
| 10 | `*` `/` `%` |
| 11 | unary `!` `~` `-` `*` (deref) `?` |
| 12 | postfix: call `f(x)`, index `a[i]`, field `a.b`, method `a.m(x)`, slice `a[i..j]`, `as` cast |

All binary operators are left-associative. `&&`/`||` short-circuit.
Shifts accept any integer type; the right operand is masked to the bit width
of the left (no UB).

### 4.2 Built-in operator semantics

- Integer arithmetic wraps (two's complement). Division of `MIN / -1`
  wraps to `MIN`; `x / 0` and `x % 0` panic.
- Signed shifts: `<<`/`>>` on signed values are arithmetic.
- Floats follow IEEE-754; `/ 0.0` gives infinities, not panics.
- `==`/`!=` on pointers compare identity (with `null`). On `string`,
  contents. On `bool`, ints, floats — values. On enums — variant and
  payload (compiler-generated structural equality: tags first, then the
  variant's fields; nested enums/structs recurse). Element-wise `==` on
  structs, arrays and slices arrives with the self-hosted compiler
  (0.1.x). Not defined on `fn` values.
- `as` casts: between integer types (wrap/truncate/extend as C would),
  float↔integer (`f as i32` truncates toward zero; out-of-range saturates
  to keep it defined), enum→its tag integer type. Nothing else.

### 4.3 Postfix expressions

- Call `f(a, b)`. Calling a function value of type `fn(..)..` — static or
  closure — uses the same syntax.
- Index `a[i]`: arrays and slices. Bounds-checked; panic message includes
  the index and length. Index type: any integer.
- Slice `a[i..j]`: `i <= j <= len`, bounds-checked; yields a new slice
  sharing the buffer.
- Field `a.b`: struct field, enum payload field (in match), module item
  (`mod.item`), associated function (`Type.make(..)`). Pointer auto-deref:
  `p.b` follows any number of pointers.
- Method call `obj.m(args)`: sugar for `Type::m(obj, args..)` — `m` must be
  a function whose first parameter is named `self`. Auto-deref applies.
- `?` postfix (§8).
- `new` (§4.4).

### 4.4 Allocation

```
let p: *Point = new Point { x: 1, y: 2 };   // heap struct, rc = 1
let s: []i32  = make([i32], 16);            // zeroed heap array, rc = 1
```

`new` takes a struct type; `make` takes an element type in `[]T` position
and a length. Both produce managed objects. `make` zero-initializes.
Resizing and growth are std functions (`std.append`).

### 4.5 Enum construction

`Name.Variant` denotes a variant; constructing: `Name.Variant(payload...)`
for tuple variants, `Name.Variant(field: v, ...)` with named arguments for
struct variants, `Name.Variant` for unit variants. Inside the defining
module, `Variant(...)` alone is accepted. Named arguments are legal only in
struct-variant construction.

### 4.6 Closures

```
let add: fn(i32, i32) -> i32 = fn(a: i32, b: i32) -> i32 { return a + b; };
let counter: fn() -> i32 = fn() -> i32 { count += 1; return count; };
```

A closure value is `{code: fn, env: *Env}`. Names used in the body but bound
outside are captured: `let`-bound immutables are copied into the environment;
`mut` locals are boxed — the local itself becomes a managed box shared by the
frame and the closure, so mutations are visible both ways. A static function
assigned to a `fn` type has `env = null`. The two-call-forms distinction is
invisible at call sites.

## 5. Statements

```
let x: T = expr;        // immutable binding; type optional when inferable
let mut y: T = expr;    // mutable binding
x = expr;  x += expr;   // assignment (statement, not expression)
expr;                   // expression statement
if ...                  // §6
while c { ... }         // loop while c
loop { ... }            // infinite loop; exit via break/return
break;  continue;       // innermost loop
return expr?;           // must be last statement of a block path
defer stmt_or_block;    // runs at end of enclosing block scope, LIFO
{ ... }                 // nested block scope
```

If/while bodies are blocks. `let` requires an initializer. Shadowing an
outer name is an error.

### 5.1 Expressions vs statements

`if` and `match` are expressions. A block's value is its final expression
(no trailing `;`). `let x = if c { 1 } else { 2 };` — both arms must have
the same type. Other control forms are statements. Assignment is a
statement; `a = b` has no value (this kills the `=`/`==` bug class).

## 6. Control flow

```
if c { } else if c2 { } else { }
```

Conditions are `bool` — no truthiness. The `if` expression form requires an
`else`.

`while`/`loop` are statements and evaluate to nothing. `break`/`continue`
target the innermost loop. `defer` runs on every exit path of its scope,
including `break`/`continue`/`return` and panics.

## 7. Match

```
match shape {
    Circle(r)      => draw(r),
    Rect { w, h }  => draw(w * h),
    Point          => 0,
    _              => panic("unknown"),
}
```

Arms are `pattern => expr,`. Patterns: unit variant, tuple variant with
bindings `Variant(a, b)` or `_` placeholders, struct variant with field
bindings `Variant { w, h }`, `_` wildcard, integer literals. Match is an
expression; all arms must produce the same type. **Exhaustiveness is
checked** — a match without `_` must cover every variant.

Bindings bind by copy (managed payloads are retained). An arm may open a
block `{ ... }` for multiple statements; its value is the block value.

Integer matches compare the discriminant literal-wise; `_` is the default.

## 8. Errors: Result, Option, `?`, panic

Defined in std, imported by the prelude:

```
enum Result[T, E] { Ok(T), Err(E) }
enum Option[T]    { Some(T), None }
```

`expr?` requires `expr: Result[T, E]` or `Option[T]`:

- `Ok(v)` / `Some(v)` — the arm's value is `v`.
- `Err(e)` — expand to `return Err(e)` where `E` must equal the enclosing
  function's error type.
- `None` — expand to `return None` (enclosing function returns an `Option`).

`panic(msg: string)` prints `msg` plus source location to stderr and exits
with code 101. On freestanding targets it writes to the target console.
Panic unwinds nothing — there is no unwinding; `defer`s do not run on panic
(this is deliberate: panic is for bugs, not control flow).

`assert(cond)` and `assert_eq(a, b)` live in std and panic on failure.

## 9. Functions, generics, modules

```
pub fn parse(src: string, opts: *Options) -> Result[Ast, string] { ... }
fn Point.dist(self: *Point, o: *Point) -> f64 { ... }   // method
fn Point.make(x: i32, y: i32) -> *Point { ... }         // associated fn
```

Parameters are typed, passed by value, and are mutable local copies. The
first parameter named `self` makes the function a method. Recursion is
allowed; order of declaration does not matter within a module.

### 9.1 Generics

`fn`, `struct`, `enum` may take type parameters: `fn id[T](x: T) -> T`.
Instantiation is implicit and monomorphizing: each distinct concrete use
produces a specialized copy whose body is re-typechecked. There are no
trait bounds; a generic body may only use operators on `T` that every
concrete instantiation happens to satisfy — verified per instantiation.

Recursive generic instantiation is detected and rejected.

### 9.2 Modules

One file = one module, named by its path relative to the root file's
directory: `use io/file;` loads `io/file.rho` and binds the namespace
`file`. Items are private unless marked `pub`. Cycles between modules are
allowed for functions, rejected for statics. The prelude (§10) is imported
into every module implicitly.

## 10. Builtins and the prelude

Compiler builtins (no import needed):

```
len(x)              // array/slice/string length -> usize
make([]T, n)        // new zeroed array -> []T   (§4.4)
new T { ... }       // heap struct
panic(msg)          // §8
size_of[T]()        // -> usize
```

`intrinsics` namespace (std-only by convention, not enforced): raw loads and
stores by width, `memcpy`, `mem_set`, `mem_move`, `grow_pages` (wasm) — the
minimal unsafe kernel the std library is built on.

The **prelude** is a per-target module the compiler embeds and imports
everywhere. It defines `Result`, `Option`, `panic`, `assert*`, the target's
allocator hooks (`__alloc`, `__free`), `rc_inc`/`rc_dec` (implemented in
rho itself via intrinsics), and console I/O (`print(s: string)`,
`eprint(s: string)`, `read_line()` on hosted targets).

## 11. Compilation model

### 11.1 Pipeline

```
lex → parse → resolve+monomorphize+typecheck → lower to SSA IR
    → insert rc ops → escape analysis → per-target codegen
```

The IR is in strict SSA form: every instruction produces a unique virtual
register; control-flow joins use φ nodes. Control flow in the IR is
structured (if/else diamonds, while loops, returns) — a consequence of the
source grammar, which keeps wasm lowering direct and the CFG reducible.

### 11.2 Backends

| Target | Codegen | Host tool used |
|--------|---------|----------------|
| `amd64-linux` | AT&T assembly → `cc -m64` (assemble + link) | system `cc` |
| `arm64-mac` | Apple assembly → `cc` (assemble + link, ad-hoc signed) | system `cc` |
| `wasm32-wasi` | binary module, WASI preview1 imports | none (runs on wasmtime/node/browser) |
| `esp32c3` (0.2) | RV32IM text assembly → in-tree two-pass assembler → flat load image; the in-tree simulator (boot compiler only) executes it: pc=0 entry, `a0`/`a1` = heap base/size, semihost exit via stores to `0x80000000` (stdout byte) / `0x80000004` (exit code) | none — no external toolchain, no linker |

Register allocation starts as spill-everything (every virtual register gets
a stack slot; each instruction reloads operands); a linear-scan allocator
replaces it after self-hosting. On `esp32c3`, pointers/`usize`/`isize` are
32-bit (8-byte slot footprint, low half carries the value), `i64`/`u64` run
as register pairs over fixed runtime stubs, and floating-point operations
trap (`ebreak`) until soft-float lands in 0.2.1 — same-size bit moves
(`usize`↔pointer, f32/f64 bit extraction) are plain copies and work. Structs and arrays are passed by reference
to a caller-made temporary (a documented deviation from the C ABI, which
only matters for `extern` functions — hosted externs in the prelude use
scalars and pointers only).

### 11.3 rc-pair elimination

After lowering, a peephole pass on the IR removes `rc_inc(x)` immediately
followed by `rc_dec(x)` (same value, no intervening call that could release
the last reference), and merges count traffic on slots proven local by
escape analysis. This pass must preserve observable behavior: the only
observable effect of rc traffic is death timing, which only weak references
and finalizers can observe — and rho has neither before 0.2.

### 11.4 Determinism

Monomorphization order, symbol emission order, and data layout are
canonically sorted. Compiling the same inputs twice yields byte-identical
artifacts. This is a tested invariant (`make test` includes a twice-compile
comparison).

## 12. Toolchain

```
rho build [file]  [-o out] [--target t]   compile & link (default ./main.rho)
rho run   [file] [-- args...]              build to temp & run
rho test  [file|dir]                       build & run all test_* functions
rho fmt    [-w] [file|dir]                 print/form canonical formatting
rho check  [file]                          type-check only
rho --version
```

A `test_*` function takes no parameters and returns nothing; the test runner
generated by `rho test` calls each in declaration order, prints
`ok NAME` / `FAIL NAME: message`, and exits nonzero on any failure.

Formatting is canonical: one true style, no configuration. `rho fmt`
rewrites source so that `fmt` is idempotent and `parse(fmt(parse(x))) ==
parse(x)`. Line comments are preserved: each comment rides the token that
follows it, and therefore the statement or declaration it precedes.

## 13. Version policy

- 0.0.x — boot compiler era (C). The language surface **only grows**.
- 0.0.5 is the freeze (declared with 0.0.5f): the 0.1.0 self-hosted
  compiler must be writable in exactly the language 0.0.5 defines.
  Escape analysis (§3.4) and element-wise aggregate `==` are explicitly
  deferred to the self-hosted compiler and are not part of the frozen
  surface.
- 0.1.0 — self-hosting. The C compiler is thereafter frozen forever; it
  exists only to seed new-host bootstraps. Language evolution continues in
  the self-hosted compiler only.
- Bootstrap invariant, tested continuously: `boot(self) == self(self)`
  byte for byte (stage2 == stage3), and `boot(corpus) == self(corpus)`.
