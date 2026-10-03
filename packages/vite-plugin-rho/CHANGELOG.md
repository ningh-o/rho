# Changelog

All notable changes to this package are documented here. The package
carries its own semver, independent of the compiler's freeze
(docs/ecosystem.md version policy): a package release never retags
the compiler, and each release pins the exact compiler generation it
embeds (assets/generation.json — the package's only generation pin).

## 0.1.0 — 2026-10-01

First release.

- The vite transform hook for `.rho` sources: compile to wasm32-wasi
  inside vite dev/build, in three verbs shared with the CLI's flag
  surface (build / check / fmt through the app face's `/mode`).
- Embeds one compiler generation: the self-hosted mirror in its app
  face, baked by boot from rho @ 6b4f3f5b2a89 with `MODS_APP` = the
  std tree, shrunk by `wasm-opt -Oz --enable-bulk-memory
  --enable-multivalue` (421,576 bytes, generation `e383088b22e7`) —
  byte-identical to the site's deploy asset of the same generation.
- Node runtime face: the site's pure-JS WASI shim (no experimental
  flags) + the site's vendored wabt assembler (verbatim, CJS-eval'd
  under Node).
- Acceptance: byte parity with the CLI's mirror path over the whole
  programs tier (93 of 96 programs byte-identical; the 3
  package-module users refuse cleanly — the app face has no runtime
  module channel), behavior goldens green for all 93 compilable
  programs through the shim and an 8-program wasmtime sample, and a
  real vite build graded in both emit modes.
