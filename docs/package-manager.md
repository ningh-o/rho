# The rho package manager

Design and user guide for `rho-pkg` — the package tool for rho, written in
rho ([tools/pkg/rho-pkg.rho](../tools/pkg/rho-pkg.rho)). Status: implemented
for path + git dependencies. There is **no network registry** in this round;
a registry is future work (§10).

## 1. Scope and non-goals

`rho-pkg` manages dependency **resolution and pinning** for a package: a
manifest (`rho.toml`), a reproducible lockfile (`rho.lock`), and a verified
in-tree dependency layout (`vendor/`). It deliberately does **not** touch
the compiler: `rho build` / `rho run` / `rho test` work unchanged, because
dependency imports are ordinary module-system imports (§4). There is no
version solver: a dependency resolves to exactly one declared source, and
two different sources for the same name are an error (§6). No registries,
no network access from the tool itself, no semantic-version ranges (future
work, §10).

## 2. The capability envelope (why the tool looks like this)

`rho-pkg` is a real rho program, so it inherits the current sandbox of a
rho-program — every claim below was probed against `./build/rho-boot` this
round (see the fixture script for the living proof):

- **wasm32-wasi is the only usable target for a file-manipulating tool.**
  Native images (arm64-mac) link only the runtime blob's `_write`/`_exit` —
  every file-IO extern (`open`/`read`/`close`, `_NSGetArgc`) is an
  undefined symbol at image assembly time until the self-hosting mirror
  lands its RT-blob shims (`docs/todo.md`, 0.3.x item 8). Probed: `rho
  build io.rho --target arm64-mac` fails with `asm64: undefined symbol
  _open (fixup kind 3)`.
- On wasm32-wasi the prelude's per-target hooks work end to end:
  `__open`/`__read`/`__write`/`__close` over the preopened working
  directory (fd 3; wasmtime needs `--dir .`), stdin via `__read(0, ...)`,
  stdout via `printf`. Probed end to end with `wasmtime run --dir .`.
- **No argv** (`__program_args` currently traps on wasm), **no
  `fd_readdir`**, **no `path_create_directory`**, **no process spawn**:
  the emitter wires a fixed wasi import table (`boot/src/emit_wasm.c`,
  `wasi_imports[]`: fd_write, proc_exit, args_sizes_get, args_get,
  path_open, fd_read, fd_close) and unknown externs silently emit zero.
- **`use` paths are downward-only dot-separated identifiers**
  (`spec/module-system.md`; `boot/src/check.c` resolve pass,
  `boot/src/parse.c` `use` parsing): every segment is an identifier,
  resolution is relative to the importing file's directory (falling back
  to the entry file's directory), `..` is not expressible. Probed:
  `use ../x;` is a parse error.

Three consequences shape everything below:

1. The tool's **UI is stdin**, not argv: the subcommand line is piped in
   (`echo "install --frozen" | wasmtime run --dir . rho-pkg.wasm`). A thin
   wrapper can read real argv and pipe it through.
2. The tool **cannot enumerate or create directories**, so it never copies
   or materializes packages. Dependencies are consumed **in place**
   ("vendored by reference", §4); cloning is the one step the shell does
   (§7), because git needs process spawn.
3. Dependency imports must be **plain downward module paths**, which fixes
   the install layout (§4).

## 3. The manifest: `rho.toml`

Every package — app or library — has a `rho.toml` at its root:

```toml
# rho.toml
[package]
name = "mathx"
version = "0.1.0"

[dependencies.strs]
path = "vendor/strs"          # path dependency: a package root, relative
                              # to THIS package's root, descending only

[dependencies.jsonx]
git = "https://github.com/ningh-o/rho-jsonx"   # git dependency
rev = "9c1fa37b2e33d39e7d90a4b251d8f232c5b3d6aa"  # full sha1 (or tag)
```

`[package]` requires `name` (an identifier: `[a-z0-9_-]+`, matching the
package directory's name — the last segment of the import path) and
`version` (a string, semver-shaped but not interpreted).
`[dependencies.<name>]` takes either `path = "..."` or the pair `git` +
`rev`. The dependency key must equal the dependency's own `[package].name`.

The parser accepts a **documented TOML subset**, because rho-pkg is a rho
program with no stdlib: line-based; full-line `#` comments; `[a]` and
`[a.b]` table headers; `key = "string"` pairs with `\\`, `\"`, `\n`
escapes. No inline tables, arrays, or trailing comments — out-of-subset
input is a parse error naming the line.

## 4. Where dependencies live: vendored by reference

Module imports are relative to the importing file and descend only (§2),
so a dependency must sit **inside the consumer's tree at a predictable
downward path**. The layout law:

```text
<package root>/
  rho.toml            # manifest
  rho.lock            # lockfile (generated, committed)
  lib.rho             # the facade: the package's only entry; namespace = package name
  <module>.rho        # the package's own files — package-private (optional)
  vendor/             # dependencies, one directory each
    <dep>/rho.toml
    <dep>/lib.rho     # the dependency's facade
    <dep>/vendor/     # ... and the dependency's own dependencies
```

- **Path dependencies are consumed in place** at the location their `path`
  names — nothing is copied. A path under `vendor/` is the recommended
  shape (it travels with the tree when the consumer itself is vendored);
  any descending path is legal.
- **Git dependencies are cloned** to `vendor/<name>` by the shell (§7) and
  then consumed in place at the pinned `rev`, exactly like path deps.
- **Transitivity is structural**: a dependency carries its own `vendor/`
  inside its own tree. If `mathx` depends on `strs`, then `mathx`'s tree
  contains `mathx/vendor/strs/`, and mathx's source imports
  `use vendor.strs;` — which keeps resolving at every nesting level,
  because every package's imports are spelled relative to its own root.
  Libraries that are meant to be consumed should therefore commit their
  `vendor/` (the Go 1.5 vendoring bargain, minus the copying).
- **Import spelling**: a package imports a dependency with
  `use vendor.<dep>;` — the facade, `vendor/<dep>/lib.rho` — and its own
  private modules by their relative dot path (`use util;` from the
  package root, `use net.http;` for `net/http.rho`). The namespace is the
  *last path segment* (`spec/module-system.md`), so `use vendor.mathx;`
  binds `mathx`: the dependency's directory name, which the manifest law
  makes equal to its `[package].name`. The consumer then writes
  `mathx.add(...)`, not `dep.something` — and because the facade is the
  only entry, a package's interior is invisible from outside:
  `use vendor.mathx.util;` is rejected while `mathx/lib.rho` exists.
- Inside a package, the facade composes; private modules import
  their siblings by bare name (`use util;` from a sibling file's
  directory, falling back to the package root) and never import past
  another package's facade — the same downward-only shape
  every rho program already follows.

## 5. The lockfile: `rho.lock`

`rho-pkg install` writes `rho.lock` — the reproducibility record:

```toml
# generated by rho-pkg; do not edit
[package]
name = "app"
version = "0.1.0"

[package.0]            # resolved dependencies, sorted by name
name = "jsonx"
version = "0.2.1"
kind = "git"
url = "https://github.com/ningh-o/rho-jsonx"
rev = "9c1fa37b2e33d39e7d90a4b251d8f232c5b3d6aa"
location = "vendor/jsonx"

[package.1]
name = "mathx"
version = "0.1.0"
kind = "path"
path = "vendor/mathx"
location = "vendor/mathx"
```

Determinism guarantees (pinned by the fixture test):

- entries are sorted by dependency **name** — two resolutions of the same
  manifest produce byte-identical locks;
- `location` is the dependency root **relative to the consuming package
  root** (the lock is stable across machines and check-out paths);
- `install` rewrites the lock only when content actually changes;
- `install --frozen` **never writes**: it fails if the lock is missing or
  does not match the current manifest graph — the CI gate.

Git deps pin exact `rev`s (full sha1 recommended; tags are resolved by the
clone step, the lock records what was actually checked out — resolution
after checkout verifies HEAD, §7). A package whose name appears twice with
different sources (kind/url/rev, or differing path) is a resolution error;
the same source appearing at several locations is allowed (each consumer
vendored its own copy) and each location is verified.

## 6. Resolution

`install` walks the dependency graph breadth-first from the root manifest:

1. load `rho.toml` from the current directory;
2. for each dependency, resolve its root: `(consumer root) + path` (path
   deps), or `vendor/<name>` under the consumer root (git deps, which must
   already be cloned — see §7);
3. read the dependency's own `rho.toml`, check its `name` matches the
   dependency key, and recurse into its dependencies;
4. dedupe by name; a name reached with two different sources is an error;
5. verify every resolved package: `<location>/lib.rho` must open; git
   deps must be checked out at the locked `rev` (`<location>/.git/HEAD`
   must be the detached sha1 — otherwise the exact repair command is
   printed);
6. write `rho.lock` (unless `--frozen`, which only compares).

Cycles in the graph close naturally (locations dedupe); nothing loops.

## 7. The git transport split

A rho program cannot spawn processes (§2), so `rho-pkg` does not run git.
Instead, when a git dependency is missing, at the wrong rev, or on a
branch instead of a detached checkout, `install` prints the exact commands
and exits nonzero:

```sh
git clone <url> vendor/<name>
git -C vendor/<name> checkout <rev>     # or: fetch + checkout when present
```

The operator (or any wrapper script — the fixture's `run.sh` is a worked
example with a local, network-free repository and `url.<base>.insteadOf`
rewriting) runs them and re-runs `install`. The split is deliberate and
documented: everything that is pure computation and file IO stays in the
rho tool; process spawning is the shell's job. When native targets grow
file IO and spawn (the self-hosting mirror's RT-blob work), rho-pkg can
absorb the clone step and compile native — the source is target-agnostic
modulo the file-IO hooks.

## 8. Commands

The subcommand line arrives on **stdin** (argv is unavailable to a wasm
rho program, §2); flags are `--flag value` pairs separated by single
spaces. Exit codes: `0` ok, `1` error, `2` usage.

```sh
RHO_PKG="wasmtime run --dir . rho-pkg.wasm"   # the wrapper line

printf 'init myapp\n'                  | $RHO_PKG   # scaffold rho.toml + myapp.rho
printf 'add mathx --path vendor/mathx\n' | $RHO_PKG # edit manifest + resolve + lock
printf 'add jsonx --git https://... --rev <sha>\n' | $RHO_PKG
printf 'install\n'                     | $RHO_PKG   # resolve + verify + write rho.lock
printf 'install --frozen\n'            | $RHO_PKG   # verify only (CI)
```

- `init <name>` — writes a minimal `rho.toml` and the executable entry
  module `main.rho` (`pub fn` skeleton). No directories are created (the
  tool cannot mkdir, §2); the layout law (§4) needs none.
- `add <name> (--path <dir> | --git <url> --rev <rev>)` — textually
  upserts the `[dependencies.<name>]` section into `rho.toml` (everything
  else in the file, comments included, survives byte-for-byte), then
  resolves and writes the lock.
- `install [--frozen]` — §6.

After a successful `install`, the package builds with the ordinary
toolchain — no flags, no config:

```sh
rho run main.rho        # dependency imports are plain module imports
rho check main.rho
```

## 9. Workflow and trade-offs

The recommended flow for an application:

```sh
printf 'init app\n' | wasmtime run --dir . rho-pkg.wasm
# put/copy/clone dependencies into vendor/ (§7 for git)
printf 'add mathx --path vendor/mathx\n' | wasmtime run --dir . rho-pkg.wasm
printf 'install --frozen\n' | wasmtime run --dir . rho-pkg.wasm   # CI gate
rho run main.rho
```

Trade-offs, stated plainly:

- **Vendored by reference, not by copy.** Dependencies live in the tree;
  there is no global cache and no content store. A fresh clone builds
  hermetically with zero network — the cost is tree size for git deps
  (commit them) or an install step (gitignore `vendor/`, clone after
  checkout). Both policies work; committed `vendor/` is the default the
  docs assume.
- **No copying also means no drift protection for path deps.** A path
  dependency is whatever sits at its path; the lock pins the *decision*,
  not the bytes (no hashing in v1). Content hashes are future work (§10).
- **The stdin UI is a constraint artifact**, not an aesthetic. The
  wrapper-script pattern restores a normal CLI for humans.
- **The tool is wasm-only today** because native images cannot do file IO
  yet (§2). The source uses only the per-target file-IO hooks, so the
  native port is a rebuild, not a rewrite — blocked on the mirror, not on
  this design.

## 10. Future work

- a registry (names → git URLs), resolution over version ranges, and a
  real solver;
- content hashes of resolved dependency trees recorded in `rho.lock`;
- `rho-pkg outdated` / `update`;
- absorbing the git clone step (and a native build) once rho programs can
  spawn processes and — on native targets — open files;
- `publish`/`pack`: a tarball format for registry upload;
- directory enumeration + mkdir on the wasi side (`fd_readdir`,
  `path_create_directory` are outside the emitter's fixed import table),
  which would allow `rho-pkg` to materialize `vendor/` itself.

## 11. The living proof

[tests/pkg-fixture/run.sh](../tests/pkg-fixture/run.sh) builds the tool
with the reference boot compiler, then drives an application package that
consumes a path dependency (which itself consumes a transitive path
dependency) and a git dependency (a deterministic local repository, no
network): init, add, install, `--frozen` rejection, tamper repair,
byte-stable locks, and the actual `rho run` of the consuming program. It
is the executable form of this document.
