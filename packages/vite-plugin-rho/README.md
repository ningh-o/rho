# vite-plugin-rho

[vite](https://vite.dev) support for the [rho language](https://github.com/ningh-o/rho):
compile `.rho` sources to wasm32-wasi programs inside vite dev/build,
retiring hand-pinned wasm artifacts.

The plugin embeds the compiler itself — the self-hosted toolchain
baked from the rho repo into one wasm artifact (`assets/rho-compiler.wasm`,
the same app-face build the language site ships). Every compile runs
that artifact in-process on a pure-JS WASI shim and assembles the
emitted WAT with the same wabt generation the CLI chain uses. No
native toolchain, no external compiler, no network at build time.

## Usage

```bash
npm install --save-dev vite-plugin-rho
```

```js
// vite.config.js
import { defineConfig } from "vite";
import rho from "vite-plugin-rho";

export default defineConfig({
  plugins: [rho()],
});
```

```js
// src/main.js
import wasmUrl from "./prog.rho";

const { instance } = await WebAssembly.instantiateStreaming(fetch(wasmUrl));
instance.exports.main();
```

### Options

```js
rho({
  // the compiler verb this plugin drives — the same surface the CLI
  // flags share (docs/ecosystem.md §2):
  //   "build" (default) — compile to program wasm
  //   "check"           — typecheck only, no emit; a refusal fails
  //                       the build with the compiler's diagnostics
  //   "fmt"             — the module default-exports the canonical
  //                       formatting (the formatter is the compiler's)
  mode: "build",

  // how the wasm rides the bundle in build mode:
  //   false (default) — a rollup asset; the module default-exports
  //                     its URL (import.meta.ROLLUP_FILE_URL)
  //   true            — a data:application/wasm;base64 URL; works
  //                     everywhere (dev, SSR, no asset pipeline)
  inline: false,
});
```

The transform cache is keyed by (compiler generation, mode, source
bytes): a source imported twice compiles once, and the cache dies
with the artifact generation that fed it — a stale hit is impossible.

## The generation law

The plugin embeds ONE compiler generation, pinned exactly once —
`assets/generation.json`:

- the rho commit the artifact was baked from;
- sha256 over every input: boot's C sources, the compiler's own
  library (`libs/compiler/*.rho`), the baked std tree, and the
  vendored wabt assembler;
- the artifact's sha256 and size, plus the exact build command and
  the wat2wasm version the parity was graded against.

A package release never retags the compiler: one generation, one
release, its own semver (docs/ecosystem.md version policy).

### Byte-parity with the CLI

The acceptance law (docs/ecosystem.md §2): the plugin's wasm is
byte-identical to the CLI's for every programs-tier program (same
compiler, same flags). The CLI path is the repo differential's
self-hosted leg: `rho build libs/compiler/main.rho --set SRC=… --set
MODS=…`, run to WAT under wasmtime, assembled by wat2wasm. The
package's test (`test/parity.test.js`) proves it over the whole
programs tier (96 programs), plus behavior against the suite's
stdout/exit goldens.

Known boundary: programs whose modules live outside the std tree
(package users, e.g. `use geom;`) refuse cleanly with `mods:
unresolved module path(s)` — the app face reads `/main.rho` and a
`/mode` marker from its filesystem, but a module tree can only ride
the artifact at bake time. Compile package-using sources with the
CLI, or bake a custom artifact (see below).

## How the artifact is built

In the rho repo worktree (requires `./build/rho` — `make all` — plus
`wasmtime`, `wat2wasm`, `wasm-opt`, the repo's gate tools):

```bash
node tools/build-artifact.mjs          # bake assets/ + generation.json
node tools/build-artifact.mjs --check  # rebuild and verify the pin
```

The recipe is the site's own (`tools/build-app-artifact.sh` +
`tools/build-site.sh`): boot builds the self-hosted mirror with
`MODS_APP` = the std tree, a greet probe runs through the runtime
face, `wasm-opt -Oz --enable-bulk-memory --enable-multivalue`
shrinks it, and the optimized artifact is probed again. Two builds
must be byte-identical (the determinism law).

## Runtime notes

- Node >= 18. The WASI shim is pure JS (no `node:wasi`, no
  experimental flags) — the same shim the rho site runs in the
  browser, minus its JSPI stdin-provider lane.
- The compiler's WebAssembly.Module is compiled once per process;
  every compile pays only instantiation.
- The vendored `vendor/wabt.mjs` is a verbatim copy of the site's
  assembler (its sha256 rides the generation pin). Under Node it is
  evaluated on its UMD CJS branch (`src/wat.js`); the file itself is
  never patched.
