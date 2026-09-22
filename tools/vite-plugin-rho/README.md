# vite-plugin-rho

The official Vite integration for the [rho language](https://github.com/ningh-o/rho):
import `.rho` modules from your frontend code like any TypeScript module —
compiled to WebAssembly at build time, instantiated with a minimal WASI shim,
and re-exported as real, typed JS functions.

```ts
import { fib, concat } from './math.rho';

fib(10); // 55
concat('hello, ', 'rho'); // 'hello, rho'
```

## How it works

The rho compiler (as a wasm32-wasi module, `rho.wasm`) runs in-process
during the build. For each imported `.rho` module the plugin:

1. runs `rho fmt` over the module — the compiler's canonical typed surface —
   and parses the top-level `pub fn` signatures from it;
2. generates a **bridge program** that `use`s your module and wraps every
   supported export in a fingerprint function (a unique signature the plugin
   can find in the wasm type section — no name guessing); the compiler itself
   type-checks every wrapper;
3. compiles the bridge to `wasm32-wasi`, locates the wrappers by fingerprint,
   and splices one export per wrapper into the export section;
4. emits the patched wasm as an asset and a companion ESM module that
   instantiates it (with a minimal WASI shim: stdout/stderr, clock, random)
   and re-exports your functions with the right calling convention;
5. writes `index.d.rho.ts` next to your source so `tsc` (with
   `allowArbitraryExtensions`) and editors see real signatures.

Functions whose signatures contain only primitives, `string`, slices, or
pointers are exported. Generics and struct/enum/function-typed signatures are
reported as comments in the `.d.rho.ts` instead.

## Install

```bash
npm install --save-dev vite-plugin-rho
```

Build the compiler once from the rho repo:

```bash
make build/rho.wasm
```

## Use

```ts
// vite.config.ts
import { defineConfig } from 'vite';
import { rhoPlugin } from 'vite-plugin-rho';

export default defineConfig({
  plugins: [
    rhoPlugin({
      compiler: path.resolve(import.meta.dirname, '../../rho/build/rho.wasm'),
    }),
  ],
  build: { target: 'es2022' }, // companions use top-level await
});
```

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

Generate the declaration sidecars (also generated automatically on first
dev/build, but run it explicitly so `tsc` sees them on a fresh checkout):

```bash
rho-dts --compiler path/to/rho.wasm src
```

```html
<!-- index.html -->
<script type="module" src="/src/main.ts"></script>
```

```bash
vite build && vite preview
```

## Options

| Option    | Type                     | Default | Meaning                                                     |
| --------- | ------------------------ | ------- | ----------------------------------------------------------- |
| `compiler`| path / bytes / `{ url }` | —       | The rho compiler as a wasm32-wasi module (required).        |
| `sources` | `string[]`               | —       | Extra files/directories visible to `use` resolution, on top of each compiled module's own directory. |
| `dts`     | `boolean`                | `true`  | Write `<name>.d.rho.ts` sidecars next to compiled modules.  |

## Notes

- **Dev** serves the wasm as a data-URL module inside Vite's module graph, so
  editing a `.rho` file invalidates, recompiles, and hot-swaps it like any
  other module.
- **Build** emits `index.rho-<hash>.wasm` as a regular asset the bundle
  fetches — caching and CDN rules work as usual.
- Types: primitives map to `number`/`bigint` (`i64`/`u64`/`usize` are
  `bigint`), `bool` to `boolean`, `string` marshals through a borrowed record
  with an immortal rc header. Pointer/slice parameters are typed `number` and
  take raw addresses — an advanced escape hatch, not a suggestion.
- rho's `main` is not run on import; the module is used as a library.

See [the frontend guide](../../docs/frontend-guide.md) for the full story,
including size characteristics and package-manager workflows.

## Development

```bash
npm install
npm test        # vitest: unit + real-compiler end-to-end + vite build
npm run typecheck
```

The integration tests require the compiler wasm at `../../build/rho.wasm`
relative to this package (built from the rho checkout they live in); they
skip with a clear message otherwise. The `example/` directory is a complete
vite + TypeScript app:

```bash
cd example
npm install
npm run build   # rho-dts src && tsc --noEmit && vite build
npm run preview
```
