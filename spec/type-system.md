# rho type system — the law

The ratified design's type rules, made concrete. §1–§11 number the
design §3's rules; §12–§15 make the mechanisms the design names
elsewhere (`?`, operator typing, the mut view, the operator traits)
law of their own.

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
error (never a silent wrap). For float literals, **fit** means finite
and representable: a literal that rounds to ±inf, or that rounds to
zero from a nonzero literal, is out of range for its consumer — a
compile error, never a silent `inf` (ratified 2026-09-26; subnormal
but nonzero values fit).

## 2. `const` infers; annotation pins a builtin

`const N = 4;` infers from the initializer (an integer literal without
other consumer → `i32`). `const N: i64 = 4;` pins the type; the
initializer must comptime-fit it. The annotation may be any builtin
type only (not user types): a const is a folded value. Initializers
must be comptime-evaluable: literals, other consts, builtin operators
on comptime values, `as` casts of comptime values. "Other consts"
reaches across modules: a comptime initializer may read a const of
another module through a use binding — the qualified `mod.K` form, an
item import, or a facade re-export; the fold runs after the module
graph binds. An initializer that cannot fold — a cycle, a non-comptime
form, an unresolvable name, a value that does not fit the annotation —
is a **compile error**, never a silent zero (ratified 2026-09-26).
Statics fold in the same fixpoint as consts and are held to the same
law.

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
when a method `to_str` with a matching signature exists for it
(including the receiver's `mut` form, §14). Impl
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
signature must match the trait's signature exactly. Receiver
`mut`-ness (§14) is not a resolution axis: two candidates differing
only in `mut self` are an ambiguity.

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
comparable. Builtins compare directly — no trait lookup:

| type            | compares by            |
| --------------- | ---------------------- |
| bool, integers, floats | value (`NaN != NaN`) |
| string          | content (bytes)        |
| `*T`            | identity (same object) |
| enums           | tag, then payload element-wise (the Eq default) |
| structs         | element-wise (the Eq default) |
| `Option[T]`     | constructor, then payload |

On user structs and enums the element-wise default holds until an
`impl Eq for T` exists — then the trait's `eq` **replaces** the
default entirely (the operator resolves through the operator traits,
§15). Cyclic data (structs whose transitive fields reach a cycle,
through user `eq` impls as much as through the defaults) recurses and
ends in a **stack-overflow panic** — documented, not detected.

Never comparable — **locked, no impl can unlock them**: `fn` types,
`dyn`, slices `[]T`, `Result[T, E]` (err-payloads never compare).

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

The comparison operators on user types resolve through the operator
traits (§15): `==`/`!=` through `Eq` (slotwise default, impl
overrides), `<`/`<=`/`>`/`>=` through `Ord` (explicit impl only).

Integer semantics (wrap, `MIN / -1 = MIN`, `/0 %0` panic, shifts mask
by the left width) and float semantics (IEEE-754, `/0.0` = inf) are
specified in `spec.md` §4.

## 14. The mut view

Assignment through a handle binding — field stores, slice-element
stores, `mut`-receiver method calls — requires the binding to be
`mut` (TODO.md §18); a call-site argument marker pairs with a `mut`
parameter in both directions. `mut` changes permission, never
representation: copying, retain/release, and layout are §2's law
unchanged. The view is shallow: a handle copied out of a non-`mut`
binding is governed by its own binding's `mut` — the language never
tracks read-only-ness transitively.

## 15. The operator traits — Eq, Ord, Hash

Operator overloading with one shape and no magic (design §11,
ratified 2026-09-26). Three prelude traits anchor the comparison and
hash laws:

```
trait Eq   { fn eq(self, other: Self) -> bool }
trait Ord  { fn lt(self, other: Self) -> bool }
trait Hash { fn hash(self) -> u64 }
```

**Self** is a reserved word. Inside a trait declaration it names the
type satisfying the trait; inside an impl block (and its methods) it
names the impl's target type. It is a type expression and is valid
nowhere else — a `Self` outside trait/impl context is an unknown type.

**Resolution.** Builtins never consult the traits: bool, integers,
floats, `string`, and `*T` emit directly, exactly as before the
traits existed. On user structs and enums:

- `==`/`!=` resolve to the slotwise element-wise default until an
  `impl Eq for T` exists; the impl's `eq` then replaces the default
  entirely (never merges with it).
- `<`/`<=`/`>`/`>=` exist on a user type **only** through
  `impl Ord for T` — there is no lexicographic auto-derive and no
  default ordering. `a < b` calls `lt`; the other three derive:
  `a <= b` is `!(b < a)`, `a > b` is `b < a`, `a >= b` is `!(a < b)`.
  One method, one meaning — `lt` is the only implementable ordering.
- `hash(...)` (the default) and an explicit `impl Hash for T` follow
  the same replace-don't-merge law. The default folds the same values
  the `==` law compares: 64-bit FNV-1a (offset basis
  `0xcbf29ce484222325`, prime `0x100000001b3`) over the slots in
  declaration order — string = its content bytes; `*T` = the 32-bit
  address, little-endian; integers and floats = their little-endian
  bit patterns (`f32` 4 bytes, `f64` 8; a float hashes its bit
  pattern, so `NaN != NaN` may still hash equal — equal implies
  equal hash, never the converse); bool = one byte; enums = the tag
  (`i32`, little-endian) then payload slots; `Option[T]` = constructor
  then payload. Byte law pinned because hash values are behavior, and
  behavior must match across compilers (spec.md §8).

**The never-list is locked**: `fn` types, `dyn`, slices `[]T`, and
`Result[T, E]` are never comparable and never hashable, and no impl
can unlock them. The `==` operator itself never applies to `dyn` —
though a `dyn Eq` value dispatches `.eq(...)` virtually, exactly like
any trait method (§9); there is no separate `dyn` mechanism for
operators.

**Coherence and bounds** are the existing laws unchanged: impls live
in any module (§4), satisfaction is exact-match-unique (§5 — two
satisfying `eq` methods for one trait/type pair is the ordinary
ambiguity error), and `[T: Eq]` / `[T: Ord]` / `[T: Hash]` are
ordinary §8 bounds — verified per instantiation, dispatching
statically to the satisfying method. The traits live in the prelude
(kernel-anchored, spec.md §9) and are addressed by identity: a user
trait named `Eq` in another module does not hijack the operators.
