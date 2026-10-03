# Changelog

All notable changes to this package are documented here. The package is a
product of the rho toolchain, not the toolchain: it carries its own semver,
its own tags, and pins the exact compiler generation it embeds
(docs/ecosystem.md version policy).

## 0.1.0 — 2026-10-01

First release. Built against compiler generation `558a0bc96653`
(rho `86083a29fe5e`, the 0.1.0 freeze tree).

- The prettier plugin: `parsers.rho` runs the embedded compiler's fmt face;
  `printers.rho` returns the canonical text verbatim — a thin client, never a
  second formatter.
- The embedded artifact: the self-hosted mirror's app face with an empty
  `MODS_APP` (the chain-law shape), shrunk with the site's
  `wasm-opt -Oz --enable-bulk-memory --enable-multivalue` step; baked and
  pinned by `tools/build-artifact.mjs` (fmt-face probe against boot's
  `rho fmt` raw and optimized, two chain builds byte-identical, generation
  recorded in `assets/generation.json`).
- The Node runtime face: pure-JS WASI preview1 shim (Node adaptation of the
  site's playground shim — no JSPI, fully synchronous), exposed as
  `prettier-plugin-rho/runtime` (`fmtRho`, `warmCompiler`, `loadGeneration`).
- Acceptance suite (`npm test`, node --test):
  - plugin output byte-identical to the fmt face's stdout for every
    programs-tier program (96, the corpus differential's PINNED base);
  - plugin output byte-identical to `rho fmt` on the repo's fmt-law suite
    (tests/fmt-self, the subset grammar run-fmt-self.sh pins);
  - fixpoint law through the plugin path on the fmt-self suite (settle form)
    and on every programs-tier program whose face output is stable (48;
    the other 48 are the frozen subset formatter's gap, pinned and documented
    in README "Formatter scope" — the count may only shrink).

Known gaps are documented in README "Formatter scope" and
"Known limitations"; they all close with post-freeze formatter growth, not
with plugin changes.
