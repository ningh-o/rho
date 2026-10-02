# site/ — the rho language home

Four pages, no build system, no framework:

- `index.html` — the hero: what rho is, the tour program (its output is
  pinned by `make test`), why the pillars hold, the quickstart.
- `tutorial.html` — seventeen chapters; every example renders as a live
  editor and runs in the page through the real compiler.
- `playground.html` — the editor: examples picker, compile + run, live
  diagnostics (the checker squiggles), `fmt`, stdin with the terminal
  row, shareable `#code=` hashes.
- `spec.html` — the specification reader over `spec/*.md` (copies of the
  repo's law, refreshed by `tools/build-site.sh`).

## The compiler in the page

`assets/rho.wasm` is the REAL self-hosted compiler — `libs/compiler`
built by boot in its app configuration (the std tree baked in as
`MODS_APP`, built by `tools/build-app-artifact.sh`), shrunk with
`wasm-opt -Oz --enable-bulk-memory --enable-multivalue`. It reads
`/main.rho` (plus a `/mode` marker for `check` and `fmt`) from an
in-memory filesystem and writes canonical WAT to stdout; `wat.js`
assembles that WAT with wabt (`vendor/wabt.mjs`) — the same text the
repo's own chain feeds wat2wasm. Compilation happens on the page's main
thread (a V8 worker-context miscompile once produced invalid artifacts
for byte-identical source), execution in a worker, so a runaway program
can never freeze the page.

## Rebuilding the assets

```
make site            # compiler artifact + spec copies, smoke-probed
```

Run before committing site changes — `site/` is committed whole (the
Pages workflow uploads the directory as-is; there is no CI build step).

## The copy cannot rot

Every example in `assets/examples.js` — the hero's tour, all seventeen
tutorial chapters, the playground picker — is compiled and executed by
`tools/verify-site-examples.mjs` in `make test`, and its `expect` is
matched byte-for-byte against the real compiler's stdout. Editing an
example into non-compiling or dishonest shape fails the same `make
test` the compiler itself must pass.
