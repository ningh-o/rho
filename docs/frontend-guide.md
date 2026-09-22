# Using rho in a production frontend

rho programs are WebAssembly modules. That makes the browser a first-class
home for them — and this guide is the recipe for shipping rho code inside a
real frontend project: a Vite application with TypeScript, code-splitting,
hashed assets, and a type checker watching your back.

The official integration is [`vite-plugin-rho`](../tools/vite-plugin-rho/)
(README in that directory is the tool's own manual). This guide walks the
whole path: installation, writing modules, importing them, what the types
mean, what the developer experience feels like, and what to watch when it
comes to size.

## 1. Installation

The plugin needs two things: the plugin package, and the rho compiler itself
as a wasm32-wasi module (`rho.wasm`).

```bash
npm install --save-dev vite-plugin-rho
```

Build the compiler once from the rho repo:

```bash
make build/rho.wasm
```

Then point the plugin at it:

```ts
// vite.config.ts
import { defineConfig } from 'vite';
import { rhoPlugin } from 'vite-plugin-rho';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const compiler = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '../rho/build/rho.wasm',
);

export default defineConfig({
  plugins: [rhoPlugin({ compiler })],
  build: { target: 'es2022' }, // generated companions use top-level await
});
```

The `compiler` option accepts a filesystem path (shown here), raw bytes, or
any fetchable URL — however your project prefers to acquire it. Where the
bytes come from, and how they are versioned for your app, is the package
manager workflow described in [docs/package-manager.md](package-manager.md).

TypeScript needs one flag so `.rho` imports pick up their generated
declarations:

```jsonc
// tsconfig.json
{
  "compilerOptions": {
    "allowArbitraryExtensions": true,
    "target": "ES2022",
    "module": "ES2022",
    "moduleResolution": "bundler"
  }
}
```

## 2. Writing a module

A `.rho` file is a module. Everything a frontend should call is a top-level
`pub fn`; everything else stays private to the file.

```rho
// src/math.rho
pub fn fib(n: i32) -> i32 {
  if (n < 2) {
    return n;
  }
  return fib(n - 1) + fib(n - 2);
}

pub fn concat(a: string, b: string) -> string {
  return cat(a, b);
}

fn main() -> i32 {
  return 0;
}
```

Notes on shape:

- **`main` is optional.** The plugin treats the file as a library — `main` is
  never executed on import, so keep it (returning 0) or leave it out.
- **`use` works as usual.** `use helper;` resolves against the compiled
  module's own directory (sibling files are visible automatically); the
  plugin's `sources` option can widen that view to other directories.
- **What can be exported:** functions over primitives (`i8`–`i64`,
  `u8`–`u64`, `usize`, `isize`, `f32`, `f64`, `bool`), `string`, slices
  (`[]T`), and raw pointers (`*T`) — with the last two treated as raw
  addresses (see §4). Generic functions and signatures involving structs,
  enums, function values, or trait objects are not exported today; the
  generated declarations say so in a comment rather than failing the build.

## 3. Importing and calling

```ts
// src/main.ts
import { fib, concat } from './math.rho';

document.querySelector('#out')!.textContent =
  concat('fib(10) = ', String(fib(10)));
```

Run `vite build` (or `vite dev`) and it just works:

- **Build** compiles the module with the real toolchain, patches the wasm's
  export section, and emits `math.rho-<hash>.wasm` as a regular asset next to
  your JS. Your hosting, caching, and integrity story is the one you already
  have for assets.
- **Dev** keeps the compiled wasm inside Vite's module graph as a data URL.
  Editing `math.rho` invalidates the module, recompiles, and hot-swaps it —
  the same inner loop every other file in the project enjoys.

If a rho source file has a type error, the build fails with the compiler's
own diagnostic, pointing at the offending line, exactly like a TS error does.

## 4. Types

The plugin generates `math.d.rho.ts` next to the source (on first dev/build,
or explicitly via `rho-dts src`) and TypeScript reads it through
`allowArbitraryExtensions`. The mapping:

| rho                  | TypeScript | Notes                                            |
| -------------------- | ---------- | ------------------------------------------------ |
| `i8`–`i32`, `u8`–`u32` | `number` |                                                  |
| `i64`, `u64`, `usize`  | `bigint` | 64-bit integers cross the wasm boundary as BigInt |
| `bool`               | `boolean`  |                                                  |
| `f32`, `f64`         | `number`   |                                                  |
| `string`             | `string`   | marshaled both ways (see below)                  |
| `[]T`, `*T`          | `number`   | raw address — an escape hatch, documented as such |

Strings are the one non-trivial marshal. A JS string handed to rho becomes a
borrowed string record inside rho memory whose ownership header is marked
immortal, so the callee's reference-count traffic skips it — nothing to free,
no copies outliving the call. A string returned by rho is read from the
record the call leaves behind and copied into a fresh JS string. UTF-8 all
the way through.

Everything the type checker sees is real: `fib("ten")` is a compile error,
and `fib(10)` has type `number` — generated from the compiler's own view of
your module (`rho fmt` output), not a hand-written shim.

## 5. Developer experience

- Edit a `.rho` file, save: the module recompiles in place, the browser gets
  the new wasm. No manual step, no dev server restart.
- Compiler diagnostics surface in the build output where you already look.
- `rho fmt` canonical formatting and `rho check` remain useful on the
  command line; the plugin drives the same binary for its own work.
- A worked example lives in
  [`tools/vite-plugin-rho/example/`](../tools/vite-plugin-rho/example/):
  an `index.rho` exporting `fib`, `add`, `exclaim` (string concat), and
  `is_even`, imported and rendered by a small TypeScript page. Its
  `npm run build` runs `rho-dts` + `tsc --noEmit` + `vite build` — the whole
  pipeline as one gate.

## 6. Size notes

Real numbers from the example app (`npm run build` there prints them):

- the compiled `index.rho` module — five exported functions plus the prelude
  machinery they reach — weighs **8.3 kB**, **3.4 kB gzipped**;
- dead-code elimination runs from the bridge's call graph, so a module costs
  only what its exported functions actually pull in; the prelude is not a
  fixed tax;
- the wasm ships as a separate asset (not inlined), so it compresses well and
  caches independently of your app code;
- base64-in-the-module-graph is used in dev only, where size is irrelevant.

One memory note: the runtime instantiates the module with a minimal WASI shim
(stdout/stderr to the console, clock, random) and reserves scratch space in
pages grown at the top of the wasm heap. rho's own allocator bumps upward
from below; a program that allocates unboundedly could walk into that
scratch space — treat exported functions as bounded, pure computations, as
frontend functions should be anyway.

## 7. Package-manager workflow

How to acquire and version the compiler artifact (`rho.wasm`) in a
project — pinning, caching in CI, and what to commit — is described in
[docs/package-manager.md](package-manager.md).
