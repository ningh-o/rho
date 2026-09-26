# rho module system — the law

## 1. One file, one module

Every `.rho` file is a module. The module's name is the file stem.
Declarations inside a module are visible to each other in any order.

## 2. Lookup

A dotted `use a.b.c;` resolves against **two bases, in order**:

1. the directory of the file containing the `use` (the *user's* base);
2. the directory of the entry file (the *entry's* base).

Segment `a` resolves to a file `a.rho` or a directory `a/` under a
base; the next segment descends into that directory the same way. The
first base that matches wins; a name that resolves differently under
both bases is **not** an error (the user's base shadows the entry's),
but a facade ambiguity (§4) always is.

The first segment `std` is reserved: it resolves against the reserved
in-repo `std/` directory (see §7) and never against the two bases.

**Module or item.** The final segment resolves as a module first (file
or directory); only when no module matches may it resolve to a
**public item** of the module it lands in — the binding is then the
item itself, used unqualified, under its own name or the `as` name
(`use lex.a as b;` binds lex's item `a` as `b`). A final segment that
matches both a module and an item is an ambiguity error naming both
(§3's rule, across kinds). An import binding that collides with a
local declaration — or two import bindings of one name — is a compile
error naming both. Item bindings never cross a package boundary: a
package's items are reachable from outside only qualified through its
facade (§4); `use geom.norm;` from outside `geom/` is an error.
`use geom.{a, b as c};` is pure sugar: it expands to `use geom.a;`
and `use geom.b as c;`, each resolved by the rules above.

## 3. Exactly one real body

A module name must have exactly one real body. If both `lib.rho` and
`x.rho` could serve as the body of a name — concretely, a directory
`foo/` containing `lib.rho` *and* the same lookup scope offering
`foo.rho`, or a package whose facade and an identically-named file
module collide — the `use` is an **ambiguity error** naming both
paths.

## 4. Packages

A **package** is a directory behind a `lib.rho` facade.

- `use geom;` where `geom/` contains `lib.rho` imports the facade: the
  public items of `lib.rho` (its own `pub` items plus its `pub use`
  re-exports) are bound under the name `geom`.
- Interior files of a package (other `.rho` files in the directory)
  are **package-private**: importable by the facade and by sibling
  files inside the package, never from outside.
- Subpackages (`geom/units/` with its own `lib.rho`) are closed from
  the outside: `use geom.units;` is legal, `use geom.units.inner;` is
  not (the outside may name a package only through its facade, one
  level at a time).
- A plain file module inside a package directory is imported by its
  dotted path from within the package (`use web.strs;` — a file
  `strs.rho` in `web/`).
- Plain item-level imports (§2) stop at the facade: from outside, a
  package's items are reachable only qualified through the facade
  binding. This is what keeps the facade the package's whole public
  surface.

## 5. Visibility

Two tiers: **pub** (visible to importers, re-exportable) and
**private** (visible inside the module; and to the enclosing package
via its facade only if re-exported). Functions, structs, enums,
traits, consts, and impls may be `pub`. Statics are always module
private (global state never crosses modules). `main` is reserved in
the root module: a `fn main` must exist in the entry file; a `main`
anywhere else is an error.

## 6. `pub use` — the four forms

Legal only in a facade (`lib.rho`), these re-export items through the
package name:

| form                     | effect                                        |
| ------------------------ | --------------------------------------------- |
| `pub use sub;`           | bind submodule/package `sub` under the facade |
| `pub use sub.name;`      | bind one public item `name` from `sub`        |
| `pub use sub.name as n;` | bind one item, renamed                        |
| `pub use sub.*;`         | flatten: bind every public item of `sub`      |

Use bindings (plain `use`) are private: an importer of a module does
not see what that module imported. The facade keeps the re-export
monopoly: a plain `use` may import an item into the importer's own
scope (§2), but only `pub use` — here, in a facade — publishes
anything outward.

## 7. `std` — the reserved directory

`std` is the reserved in-repo directory of standard packages. `use
std.io;` resolves by the single rule: first segment `std` → the
reserved directory; remaining segments resolve inside it as §2–§4
(packages with facades, or file modules one level deep). Nothing
outside `std/` may be named `std`. The compiler's own source consumes
`std` packages like any user code; the bootstrap chain vendors
in-tree packages only.

## 8. Methods and impls anywhere

Methods and impl blocks may be defined in any module, regardless of
where the type or trait lives. Coherence is enforced at call sites by
exact-match-unique (`type-system.md` §5–§6); there is no module
ownership requirement. Same-signature collisions between two modules
in a caller's candidate set are a compile error naming both modules.

## 9. Root-file constants

Root-file `const`s (the entry module's) are visible in every module
without `use` — prelude status (see `spec.md` §6 for the build-
parameter law). Shadowing a root const (by a module-level const or a
local) is an error. Beyond root consts, modules see only what they
`use` — and that reach holds in const initializers too: a comptime
initializer may read another module's consts through its bindings
(qualified, item-imported, or facade re-exported); cycles and
unresolvable initializers are the comptime law's compile error
(`type-system.md` §2).
