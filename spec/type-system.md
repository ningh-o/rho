# rho type system — the law

The eleven rules of the ratified design, made concrete. Numbering
follows the design section §3.

## 1. Literals adapt to their consumer

An integer or float literal has no type of its own: it takes the type
its context demands. **Consumers** are, exhaustively:

- the annotated type of a `let`/`const`/`static`/field/parameter/
  return type the literal flows into directly,
- the element type of a slice literal or `new` constructor field,
- the operand position of a binary operator whose other side has a
  determined type (same type must result),
- a function argument whose parameter type is determined,
- the subject of an `as` cast (`3 as u8` — the cast consumer),
- an arm of a match/if whose other arms determine the type.

With no consumer: integers are `i32`, floats are `f64`, and the value
must fit. `bool` and `string` literals always have their own type; they
do not adapt. A literal that cannot fit its consumer is a compile
error (never a silent wrap).

## 2. `const` infers; annotation pins a builtin

`const N = 4;` infers from the initializer (an integer literal without
other consumer → `i32`). `const N: i64 = 4;` pins the type; the
initializer must comptime-fit it. The annotation may be any builtin
type only (not user types): a const is a folded value. Initializers
must be comptime-evaluable: literals, other consts, builtin operators
on comptime values, `as` casts of comptime values.

## 3. `as` is the only conversion

One conversion operator, one semantics — **constants truncate exactly
like variables**; there is no "constant-only precision" special case.

| from \ to | integers                          | f32/f64                 |
| --------- | --------------------------------- | ----------------------- |
| integers  | wrap/truncate/extend two's complement | exact (round to nearest for narrowing) |
| floats    | truncate toward zero, saturate out of range (NaN→0) | convert |
| enum      | its tag (the declared order, `i32`) | — |

`enum_value as i32` yields the tag. There is no reverse (integer →
enum) conversion. `bool` does not participate in `as` in either
direction. `string` does not convert via `as` (no coercions, ever).
Pointer types do not convert via `as`.

## 4. traits: impl blocks and bare methods coexist

A trait is a set of method signatures. Satisfaction is **name +
signature match**: a type satisfies `trait Show { fn to_str(self) -> string }`
when a method `to_str` with a matching signature exists for it. Impl
blocks (`impl Show for Pt { … }`) and bare methods
(`fn Pt.to_str(self: *Pt) -> string`) both provide methods; multiple
impl blocks may target the same trait; methods and impls may be
defined in any module, including one that has neither the trait nor
the type. Coherence is enforced at call sites (§5, §6), not by
ownership.

## 5. Function overloading — exact match unique

Several functions may share a name. A call resolves by **exact
match**: parameter types + count + receiver (for methods) equal to the
call's argument types. Exactly one match → the call resolves. Zero
matches or more than one match → compile error **naming the
candidates** (module + name + signature each). Literal arguments use
the consumer rule against each candidate's parameter type: an overload
set may not contain two signatures a literal argument set can satisfy
simultaneously with different interpretations — if two distinct
signatures both exact-match after literal adaptation, it is ambiguous.

Trait satisfaction uses the same rule: the satisfying method's
signature must match the trait's signature exactly.

## 6. Method visibility is import-scoped

The candidate set for `x.m(args)`:

- **native methods** — declared in the module that declares `x`'s type
  (or, for generic instantiations, the declaring module of the
  generic type). These travel with the type: visible wherever the type
  is.
- **extension methods** — declared in any other module. These
  participate only if that module is in the caller's use closure
  (transitively via `use`/`pub use`).

Same-signature collisions (two modules each contributing a method with
one identical signature) are a compile error naming both modules —
detected at the call site that needs them.

## 7. `use` has one form

Dotted `use a.b.c;` — the final segment binds a module or a public
item, with the brace sugar and the collision law defined in
`module-system.md` §2. (Kept here as rule 7 to preserve the design's
numbering.)

## 8. Trait bounds, verified per instantiation

Generic functions and types may carry bounds: `fn show[T: Show](x: T)`.
At every instantiation the bound is verified: the type argument must
satisfy the trait (name + signature, §4). Bounds stack
(`[T: Show, Eq]`); a struct's bounds are checked at each
instantiation site of the struct. Calls through a bound dispatch
statically to the satisfying method for that instantiation (bounds are
not dyn — see §9 for `dyn`).

## 9. Pointers non-null; `?T` is Option sugar

`*T` is a non-null pointer; there is no `null` value. Absence is
expressed once: `?T` parses as `Option[T]`. `weak.get()` returns
`?*T` — dead-or-alive. There are **no smart casts**: an `if` that
checked an Option does not refine the type in the branch; unwrap via
`match` or methods (`Option.get_or`, `Result.unwrap_or` … per the
prelude). `Option[T]` and `Result[T, E]` are ordinary generic enums
from the prelude; user code may match on them, and `?` (§12) is the
propagation sugar.

`dyn Trait` is the existential: a `dyn Show` value is a pointer to a
value of some type satisfying `Show`, plus its vtable. Coercion
`*T → dyn Trait` happens at expected-type sites (annotation,
argument, field initializer, return) when `T` satisfies the trait.
Method calls through `dyn` dispatch virtually. `dyn` values are not
comparable (§10) and not printable.

## 10. The `==` comparability law

`==`/`!=` require both sides the same type, and that type must be
comparable:

| type            | compares by            |
| --------------- | ---------------------- |
| bool, integers, floats | value (`NaN != NaN`) |
| string          | content (bytes)        |
| `*T`            | identity (same object) |
| enums           | tag, then payload element-wise |
| structs         | element-wise           |
| `Option[T]`     | constructor, then payload |

Never comparable: `fn` types, `dyn`, slices `[]T`, `Result[T, E]`
(err-payloads never compare). Comparing cyclic data (structs whose
transitive fields reach a cycle) recurses and ends in a
**stack-overflow panic** — documented, not detected.

## 11. Everything is a value type

No moves, no borrows, no address-of (`&x` does not exist; `new` is the
only heap operator). Assignment and argument passing copy the value;
copies of managed values (pointers, strings, slices, dyn) retain per
the memory law (`spec.md` §2). Structs and enums are values (inline
storage); `*T` boxes them.

## 12. `?` on Result/Option

`e?` requires `e: Option[T]` (propagates `None`) or `e: Result[T, E]`
(propagates `Err(e)`). The expression yields `T`. The enclosing
function must return `Option[U]` (any `U` — the propagated `None`
fits) or `Result[U, F]` with `F` = the error type being propagated
(exact match). Mixing — `?` on a `Result` inside a function returning
`Option` — is a compile error, in both directions.

## 13. Operator typing

Binary arithmetic/bitwise/shift/comparison operators require both
sides the **same** type; the result is that type (comparisons yield
`bool`). There is no mixed-width arithmetic: widen explicitly with
`as`. `&&`/`||` take and return `bool`. Unary `-` takes the operand's
type (integer or float); `!` is bool-not; `~` is integer bitwise-not —
one operator, one meaning (`!` never meets an integer, `~` never meets
a `bool`, neither converts).

Integer semantics (wrap, `MIN / -1 = MIN`, `/0 %0` panic, shifts mask
by the left width) and float semantics (IEEE-754, `/0.0` = inf) are
specified in `spec.md` §4.
