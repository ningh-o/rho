# The rho module system

How source files become one program — the current language law, carried by
the self-hosted compiler (`libs/compiler`). The frozen boot seed keeps its
era's slash spelling (`use a/b;`) forever; the current language retired it
in the dot round, and every law below is the mirror's word, pinned by
`tests/lang/modsys/` (fixtures carrying a `mirror` marker) and
`corpus/032_pkg`.

## One file, one module

A `.rho` file is a module. A `use` names it with a dot-separated path —
`use a.b.c;` — whose segments are plain identifiers, resolved without the
extension: `use io.file;` loads `io/file.rho` and binds the namespace
`file` — the last segment. Items inside are reached as `file.item`; an
alias renames the binding: `use io.file as f;` binds `f`.

Because segments must be identifiers, a module file's name may not contain
a dot: a file named `my.mod.rho` is not a module, and a `use` that finds
only that name is rejected with a diag that says so
(`module file names cannot contain dots`). The slash form is retired: the
parser accepts no `a/b` path and diagnoses it once
(`module paths use dots: write `a.b`, not `a/b``).

## Where paths are looked up

A `use` is resolved against exactly two bases, in order — never a search
path list:

1. the directory of the file that carries the `use`,
2. the directory of the entry file (the file named on the command line).

The first base that holds a real body wins, so a nested module reaches
its package root for its siblings and vendored sources keep nesting.

## Resolution: exactly one real body

Determinism is the law: every use path has exactly one real body, or the
program does not compile — nothing is ever picked silently.

For `use a.b.c;` the candidates under one base are, in order:

- `a/b/c/lib.rho` — a **package**; the use imports its facade,
- `a/b/c.rho` — a plain **file module**.

Both existing under the same base is an ambiguity and a compile error that
names both paths (`ambiguous use `a.b.c`: both … and … exist`). Neither
existing is `cannot read`. The single-item re-export form has a second
reading (below); when the module reading and the item reading both exist,
that too is an ambiguity, reported the same way.

## Packages: a directory behind its `lib.rho` facade

A directory holding a `lib.rho` is a **package**. `use vendor.json;`
loads `vendor/json/lib.rho` and binds `json` — the facade, the package's
only window from outside:

- The package's plain files are **package-private**: they import each
  other (and the facade) freely, but a `use` that names one from outside
  the package is refused (`… is package-private (use the package facade
  …)`). "Inside the package" means: the nearest enclosing directory with a
  `lib.rho` is the same directory for both files.
- A subdirectory with its own `lib.rho` is **itself a package** with its
  own facade: `use json.internal;` loads `json/internal/lib.rho`. A
  subpackage cannot reach its parent's interior either — packages are
  closed units behind facades.
- A directory without a `lib.rho` is a loose set of modules, each
  addressable.

`main.rho` marks an **executable**: the file the toolchain builds and
runs, holding `fn main`. `lib.rho` marks a library package, which has no
entry of its own.

## `pub use`: re-exports

`pub use` is a re-export: it makes something of another module part of
this module's own namespace, so importers see it here. Callers still
reach everything through a namespace, explicitly — re-exports never put
a bare name into anyone's scope.

- `pub use lexer;` — **full flatten**: every public item of `lexer`
  becomes reachable as `<here>.<item>`. The flattened module is not bound
  as a namespace; to name it internally, add a private `use lexer;`.
- `pub use lexer.parse;` — **single item**: the one public item becomes
  reachable as `<here>.parse`.
- `pub use lexer as lex;` — the module itself, re-exported as a
  namespace: importers write `<here>.lex.parse`.
- `pub use lexer.parse as p;` — the single item, renamed: `<here>.p`.

Aliases bind, never copy: a re-exported name is the original symbol seen
under another key, so a static is one static, a generic one generic, and
a symbol name is emitted once. Re-exported namespaces resolve one level
deep (`pkg.lex.parse`, `pkg.lex.Type`).

Errors: exporting a name already occupied is the boot law's
`duplicate name`; exporting a private item is `` `x` is not public ``;
naming something that is not a module item is rejected; and a cycle of
flattens never grounds — it is diagnosed (`circular re-export through …`).

## `main` is reserved

The root file — the file named on the command line — cannot bind a
module named `main`, under any spelling (`use main;`, `use d.other as
main;`, `pub use … as main;`): the executable's entry lives in the root
file, and a module by that name would shadow the law that says so. Other
files may use a module called `main` (a `main.rho` inside a package, for
instance); only the root file is affected.

## Visibility

Items are private unless marked `pub`. A private item is visible only
inside its own module (and, for package interior files, inside their
package). A plain `use` binding is itself private — a namespace bound by
`use` cannot be reached through this module by an importer; only `pub
use` bindings cross. The prelude is imported into every module
implicitly and all of its items are visible everywhere — that is how
`printf`, `Option`, `panic` need no import.

## Resolution of names

Inside a module, a name resolves in order:

1. local scopes (parameters, `let` bindings, match-arm bindings),
2. the current module's items,
3. the prelude.

A `use`d namespace must be named explicitly (`file.parse`) — imported
items do not enter the caller's unqualified scope. Shadowing an outer
name is an error, not a warning. Binding a `use` onto an occupied name
is the boot law's `duplicate name`.

## Cycles and statics

Cycles between modules are allowed for functions and rejected for
statics: a static's initializer must be evaluatable without a cyclic
dependency (const-eval is memoized per declaration; a cycle is diagnosed,
not left to overflow).

## The prelude

The prelude is a per-target module the compiler embeds and imports
everywhere. It is composed of a pure, target-independent **core**
(`boot/prelude/core.rho`) and a small per-target **tail**:

- the core defines `Result`, `Option`, `panic`, `assert*`, the `to_str`
  family on every primitive, the concat glue the string `+` operator
  lowers to, and the variadic format sinks
  (`__fmt_print` / `__fmt_eprint`) that `printf`/`eprintf` desugar into;
- the tail defines `__alloc`/`__free`, the rc count helpers' hooks,
  `__print_str` / `__eprint_str`, `__exit`, and the host hooks (file IO
  over `__open`/`__read`/`__write`/`__close`, argv over
  `__program_args`). Boot embeds a single tail — the wasi one
  (`boot/prelude/wasi.rho`, the only target it emits); the mirror embeds
  all three (wasi, freestanding `mac`, libc `hosted`) from
  `libs/compiler/prelude_src.rho`. There is no `read_line` yet — it
  rides the stdlib roadmap's fs item.

The split keeps the core byte-identical across targets: every
target-specific fact lives in the tail.

## Root build parameters

The entry (root) file's `const` declarations are **build parameters** and
are implicitly visible in every module — the same status the prelude
carries, no `use` needed:

```
// main.rho (the root)
const native: bool = false;
// any module, no import:
if native { ... } else { ... }
```

Lookup order: lexical scopes, then the current module, then the root's
build parameters, then the prelude. A module-level item named like a
parameter is a shadowing error. `rho build --set name=value` overrides a
parameter's declared default before anything is checked; the value
grammar follows the declared annotation (`bool`: `true`/`false`; `i32`:
decimal digits with optional `-`; `string`: raw text). The full law lives
in `spec/syntax.md` § "Build parameters"; the folding semantics in
`spec/syntax.md` § "Comptime-folded conditions".

## Compiler builtins

Recognized by name at call sites, no import needed, and reserved — user
code cannot shadow them:

```
len(x)              // array/slice/string length -> usize
make([]T, n)        // new zeroed array -> []T
new T { ... }       // heap struct
panic(msg)          // print + exit(101)
printf(fmt, ...)    // formatted stdout (type-system.md § printing)
eprintf(fmt, ...)   // formatted stderr
size_of[T]()        // -> usize
```

`intrinsics` namespace (std-only by convention, not enforced): raw loads
and stores by width, `memcpy`, `mem_set`, `mem_move`, `f64_bits` /
`f32_bits`, `slice_string`, `grow_pages` (wasm) — the minimal unsafe
kernel the std library is built on.

## Program shape and entry

A program is the root module plus every transitively `use`d module,
compiled as one unit: monomorphization and reachability run over the
whole graph (spec §11), so unused prelude machinery costs nothing. The
entry point is `fn main() -> i32` in the root module (`main.rho`); its
return value is the exit code.

The formatter is part of the contract: `use` declarations print canonically
(`pub use a.b.c as x;`) and roundtrip byte-for-byte — `fmt(fmt(src)) ==
fmt(src)` — pinned by the mirror fixtures' fmt step.

## Impl ownership

An `impl Trait for Type` block must live in the module that owns the
trait or the type (the prelude counts as the owner of its traits and
primitives). A module cannot implement a foreign trait for a foreign
type — with satisfaction computed from method tables, the rule keeps
two modules from racing to satisfy the same requirement differently.
