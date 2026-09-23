# The rho module system

How source files become one program — aligned with the boot compiler
(`boot/src/check.c` resolve pass, `boot/src/main.c` driver).

## One file, one module

A `.rho` file is a module. A `use` names it with a dot-separated path —
`use a.b.c;` — whose segments are plain identifiers resolved relative to
the using file's directory (falling back to the entry file's directory,
so a nested module reaches its package root), without the extension:
`use io.file;` loads `io/file.rho` and binds the namespace `file` — the
last segment. Items inside are reached as `file.item`; an alias renames
the binding: `use io.file as f;` binds `f`. The root file (the one named
on the command line, `main.rho`) is a module like any other; `main` must
live in it.

## Packages: a directory behind its `lib.rho` facade

A directory holding a `lib.rho` is a **package**. `use vendor.json;`
loads `vendor/json/lib.rho` and binds `json` — the facade, the package's
only entry from outside:

- The package's other files are **package-private**: they import each
  other (and the facade) freely, but no file outside the package can
  reach past the facade — with `vendor/json/lib.rho` present,
  `use vendor.json.reader;` is rejected. A directory *without* a
  `lib.rho` is a loose set of modules, each addressable.
- A subdirectory with its own `lib.rho` is **itself a package** with its
  own facade: `use json.internal;` loads `json/internal/lib.rho`.
- `pub use` **re-exports**: inside `lib.rho`, `pub use reader;` makes the
  used module's public items reachable as `json.<item>` from outside, so
  a package presents one namespace no matter how its internals are split
  (without `pub`, the `use` stays package-internal).

`main.rho` marks an **executable**: the file the toolchain builds and
runs, holding `fn main`. `lib.rho` marks a library package, which has no
entry of its own.

## Visibility

Items are private unless marked `pub`. A private item is visible only
inside its own module (and, for package interior files, inside their
package). The prelude is imported into every module
implicitly and all of its items are visible everywhere — that is how
`printf`, `Option`, `panic` need no import.

## Resolution

Inside a module, a name resolves in order:

1. local scopes (parameters, `let` bindings, match-arm bindings),
2. the current module's items,
3. the prelude.

A `use`d namespace must be named explicitly (`file.parse`) — imported
items do not enter the caller's unqualified scope. Shadowing an outer
name is an error, not a warning.

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
- the tail (`boot/prelude/wasi.rho`, `hosted.rho`, `mac.rho`) defines
  `__alloc`/`__free`, the rc count helpers' hooks, `__print_str` /
  `__eprint_str`, `__exit`, and `read_line()` on hosted targets.

The split keeps the core byte-identical across targets: every
target-specific fact lives in the tail.

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

## Impl ownership

An `impl Trait for Type` block must live in the module that owns the
trait or the type (the prelude counts as the owner of its traits and
primitives). A module cannot implement a foreign trait for a foreign
type — with satisfaction computed from method tables, the rule keeps
two modules from racing to satisfy the same requirement differently.
