# rho-lsp

A Language Server Protocol implementation for the
[rho language](https://github.com/ningh-o/rho), driven by the **real rho
compiler** — the boot toolchain compiled to `wasm32-wasi` and executed
in-process inside a worker thread. No approximated re-implementation: the
diagnostics you see are the bytes the native toolchain produces (the
language's determinism invariant).

## Capabilities

| Feature | Semantics | Notes |
| --- | --- | --- |
| Diagnostics | `rho check` (parse + type-check) | published on open/change, debounced, latest-wins |
| Formatting | `rho fmt` | full-document edit; `[]` (no edit) on parse errors |
| Hover | syntactic signatures | `fn`/method signatures, `struct`/`enum`/`trait`/`use`/variant declarations, typed `let`/`const`/`static` bindings, parameters |
| Go-to-definition | single file | file-level declarations, enum variants, parameters, body-local bindings |
| Lifecycle | initialize/shutdown/exit | full text sync; stdio transport |

## Boundaries (honest scope)

- **Single document.** Hover/definition come from a lightweight syntactic
  index of the file being edited — no cross-file name resolution, no type
  inference. Hover shows what the text itself declares.
- **Not indexed:** struct fields, match-pattern bindings, closures inside
  function bodies. Hovering such a name simply yields nothing.
- **Imports:** `use` bindings that point at other project files cannot be
  resolved in-memory; the compiler's `cannot open ...` note does not match
  the diagnostic line format and is filtered out (it never pollutes the
  editor). The embedded prelude is always present.
- Documents over 512 KiB skip validation (configurable) — stale results are
  cleared rather than shown.

## Reliability

The server is built so a crashing or wedged compiler can never take the
editor session down:

- the compiler runs in a dedicated **worker thread**; a compile that hangs
  is killed after a timeout (10 s) and the worker is **respawned** on the
  next request — the LSP loop never blocks on the compiler;
- every request handler and every compiler call is wrapped; failures
  degrade to "no diagnostics" / "no edits" and are logged once to the
  client's output channel, never thrown at the protocol;
- if no compiler can be found at all, the server runs inert: it still
  speaks LSP, answers requests, and formats nothing — the editor stays
  fully alive.

## Compiler discovery

Resolution order (first existing wins):

1. `RHO_LSP_WASM` — explicit `rho.wasm` override;
2. `build/rho.wasm` next to the package (the fresh in-tree artifact,
   `make build/rho.wasm` in the rho repo);
3. `site/assets/rho.wasm` next to the package (tracked in the rho
   repo, present after a clone);
4. a native `rho` binary (`RHO_LSP_BIN`, `build/rho-boot`, or `rho` on
   `PATH`) spawned with a temp file per invocation;
5. nothing found → inert mode (see Reliability).

`RHO_LSP_BACKEND` (`wasm` | `native` | `none`) forces a backend, mainly for
tests and debugging.

## Editor integration

### VS Code

The `vscode/` folder holds the extension skeleton: it contributes the `rho`
language (`.rho`) and launches the bundled server. Package it with:

```bash
npm install
npm run build:vscode   # builds the server, copies dist/ into vscode/server/
cd vscode
npm install
npm run compile        # out/extension.js
npx vsce package --skip-license
```

`rho.compilerWasm` (settings) maps to `RHO_LSP_WASM`.

### Other editors (stdio)

Any LSP client can launch the server directly:

```bash
rho-lsp            # if installed globally (npm install -g), or:
node <path>/rho-lsp/dist/server.js
```

Neovim (nvim-lspconfig style):

```lua
vim.lsp.start({
  name = 'rho-lsp',
  cmd = { 'node', vim.fn.expand('~/path/to/rho/tools/lsp/dist/server.js') },
  filetypes = { 'rho' },
})
```

## Environment variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `RHO_LSP_WASM` | — | compiler wasm override path |
| `RHO_LSP_BACKEND` | auto | force `wasm` / `native` / `none` |
| `RHO_LSP_DEBOUNCE_MS` | `150` | validation debounce after open/change |
| `RHO_LSP_MAX_DOC` | `524288` | documents above this many chars skip validation |

## Development

```bash
npm install
npm test        # builds, then runs the unit + protocol integration suites
```

Layout:

```
src/wasi.ts             minimal WASI preview1 shim (node), ported from
                        site/assets/wasi.js semantics
src/compiler-worker.ts  worker entry: compiles the compiler wasm once,
                        answers check/fmt requests
src/compiler.ts         supervisor: backend selection, worker lifecycle
                        (timeout, respawn), native-CLI fallback, inert mode
src/diag.ts             compiler stderr parsing, LSP ranges, LineIndex
src/symbols.ts          single-file symbol index (hover/definition)
src/server.ts           stdio LSP server
test/                   unit tests + protocol tests (spawn the real server,
                        speak LSP over stdio)
vscode/                 VS Code extension skeleton
```

The tests require a compiler artifact: `make build/rho.wasm` in the
rho repo root (or accept the tracked `site/assets/rho.wasm` fallback).
