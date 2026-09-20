# Traits and `dyn` (0.4.0)

Design: **name-satisfied traits, `dyn` as a fat pointer riding the closure
ABI, and the RC header's drop slot as the release path.** The first cut
includes dynamic dispatch — that was the point.

## The rule: a type implements a trait when its methods say so

```
trait Show {
  fn to_str(self) -> string;        // a requirement: name + signature
}

impl Show for Point {               // sugar: define the methods, and
  fn to_str(self: *Point) -> string {   // check eagerly that they satisfy
    return format("({}, {})", self.x, self.y);
  }
}
```

A type implements `Show` when a method `to_str` exists on it (struct
method, enum method, or primitive method) whose signature matches the
requirement. `impl` blocks are method definitions plus an eager
compile-time check — the satisfaction relation itself is computed from
the method tables, never stored apart from them. Consequences:

- The prelude needs **no impl blocks**: `i32.to_str` & co. already exist,
  so every primitive satisfies `Show` the moment the trait is declared.
- A hand-written `fn Point.to_str(...)` (the 0.3.0 print protocol)
  satisfies `Show` with zero migration. printf's name-based lookup and
  the trait system are one world, not two.
- Duplicate satisfaction is impossible by construction — there is one
  method table per type, one method per name.

Multi-method traits work the same way: satisfied when every requirement
has a matching method. A missing method names the trait and the missing
signature in one error.

Deferred (v2 candidates): default method bodies, generic impls
(`impl[T] Show for Box[T]`), supertraits, multi-bounds (`[T: Show+Eq]`).

## Bounds: `[T: Show]`

```
fn show_all[T: Show](xs: []*T) { ... }
```

rho generics are checked per instantiation and monomorphized; a bound is
therefore mostly a **contract you can read and the checker can enforce
with a good error** ("Point does not implement Show — missing to_str"),
anchored at the instantiation site. Bounds do not change codegen: static
calls specialize as today.

## `dyn Show`: the fat pointer

```
let d: dyn Show = p;                // implicit coercion from *Point
printf("{}\n", d);                  // dispatches at runtime
```

- `dyn Show` is a two-word value: `{vtable: *u8, obj: *void}` — the same
  aggregate shape as a closure pair. Coercion is implicit wherever an
  expected type is known (let with annotation, arguments, returns, field
  initializers), and only from pointer types `*T` whose `T` satisfies the
  trait. Enums and primitives are not dyn-able in this cut — they are
  values, not heap objects (honest limit; a `Box[T]` story can lift it
  later).
- **Method call on a dyn value loads the vtable slot and calls through
  the existing closure ABI**: `code = load(vt + i*8)`, indirect call with
  `obj` as the trailing hidden env argument. Each vtable entry is a
  per-(trait, method, type) shim with signature `(args..., env)` that
  forwards `env` as `self` — a variant of the existing `shim_for`
  generator, which already handles aggregate returns (to_str returns
  string, a 24-byte aggregate).
- **Ownership needs no vtable slot.** Every heap object's RC header (24
  bytes before the data) already carries a drop function pointer at +16
  that the release path calls with the data pointer when the count hits
  zero. So the value walker for `dyn Show` is exactly the closure env's:
  `rc_inc`/`rc_dec` the object — typed cleanup rides the header.
- **Vtables are materialized at runtime, once per (trait, type), into an
  immortal heap block** (refcount top bit set — the RC runtime skips
  counting it): `__alloc(n*8 + 24)`, drop slot NULL, method slots filled
  with `ADDRC` of the shims — the very stores `new` uses for its header
  drop glue. A `.bss` global caches the pointer; the coercion site
  lazily initializes it under a null check. No backend changes: the same
  stores, loads, indirect calls, and globals closures and statics use
  today, on all four targets.
- `==` on two dyn values compares the object pointers (identity).

## What the checker builds

- `trait` declares a RecType-shaped carrier with an ordered method list
  (name, params-minus-self, ret). The order is the vtable layout.
- `impl X for T { ... }` desugars to method registrations on T plus an
  eager satisfaction check. It lives in the module owning the trait or
  the type; foreign × foreign is an error.
- `TY_DYN` is an aggregate, managed, 16-byte type whose `rec` points at
  the trait carrier. Method resolution on a TY_DYN receiver looks the
  name up in the trait's method list and lowers to a virtual call.
- Instantiation re-checks bounds: a clone whose T lacks a required
  method fails anchored at the call site that demanded it.

## Boot vs self

Boot-only this round, like 0.3.3/0.3.4; `docs/todo.md` carries the
mirror set.
