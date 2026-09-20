# The rho module system

How source files become one program — aligned with the boot compiler
(`boot/src/check.c` resolve pass, `boot/src/main.c` driver).

## One file, one module

A `.rho` file is a module. Its name is its path relative to the root
file's directory, without the extension: `use io/file;` loads
`io/file.rho` and binds the namespace `file`. Items inside are reached as
`file.item`. The root file (the one named on the command line) is a module
like any other; `main` must live in it.

## Visibility

Items are private unless marked `pub`. A private item is visible only
inside its own module. The prelude is imported into every module
implicitly and all of its items are visible everywhere — that is how
`printf`, `cat`, `Option`, `panic` need no import.

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
  family on every primitive, `cat`, and the variadic format sinks
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
entry point is `fn main() -> i32` in the root module; its return value is
the exit code.
