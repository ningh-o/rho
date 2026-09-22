# rho language support (VS Code)

VS Code extension skeleton for the [rho language](../../..). It launches the
`rho-lsp` server (bundled as `server/server.js` by the package root's
`npm run build:vscode`) and gives `.rho` files:

- compiler diagnostics (`rho check` semantics),
- canonical document formatting (`rho fmt` semantics),
- hover signatures and single-file go-to-definition.

## Build and package

```bash
# from rho/tools/lsp (the package root)
npm install
npm run build:vscode

# inside vscode/
npm install
npm run compile          # out/extension.js
npx vsce package --skip-license
```

The produced `.vsix` carries `out/extension.js`, the bundled server
(`server/*.js`) and the extension's production `node_modules`
(`vscode-languageclient`).

## Settings

- `rho.compilerWasm` — absolute path to a `rho.wasm` override, passed to
  the server as `RHO_LSP_WASM`. When empty, the server discovers the compiler
  on its own (see the [package README](../README.md#compiler-discovery)).
