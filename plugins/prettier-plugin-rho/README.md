# prettier-plugin-rho

`prettier` support for the [rho language](https://github.com/ningh-o/rho):
editors and build pipelines format `.rho` sources into the canonical
form without shipping the CLI.

Thin client by law — the plugin is **never a second formatter**. It
embeds the compiler's own fmt wasm (`assets/fmt.wasm`), feeds it the
source, and hands prettier the canonical text verbatim. One formatting
truth, two clients (this plugin and the LSP ride the same pipeline).

## Usage

```bash
npm install --save-dev prettier prettier-plugin-rho
```

```json
{ "plugins": ["prettier-plugin-rho"] }
```

```bash
prettier --write program.rho
```

Or programmatically:

```js
import prettier from "prettier";
import rho from "prettier-plugin-rho";

const canonical = await prettier.format(src, {
  parser: "rho",
  plugins: [rho],
});
```

## The generation law

The plugin embeds one **compiler generation**, pinned three ways:

- `assets/generation.json` — the full record: sha256 over the boot
  sources and the vendored `libs/compiler` fmt pipeline, the input
  slot's offset and capacity, and the exact build command;
- `package.json` — the generation short hash rides the description;
- the artifact itself — `node tools/build-fmt.mjs --check` rebuilds
  through the chain and verifies the committed bytes.

A package release never retags the compiler: one generation, one
release, its own semver (docs/ecosystem.md version policy).

## How the wasm is built

In the rho repo worktree (requires `./build/rho` — `make build/rho`):

```bash
./build/rho build plugins/prettier-plugin-rho/rho/fmtmain.rho -o fmt.wasm
node tools/build-fmt.mjs
```

`rho/fmtmain.rho` is the fmt wasm's entry; `rho/lex.rho`, `rho/parse.rho`,
`rho/fmt.rho` are verbatim copies of the compiler's own modules
(hash-verified against `libs/compiler/` at build time — a drifted copy
fails the build). The kernel imports only `fd_write` + `proc_exit`, so
the source rides linear memory: the host writes the source bytes plus a
NUL terminator at the pinned input slot (`generation.json`), then calls
`_start`; stdout is the canonical text.

## Honest limits of this generation

- **Subset grammar, enforced by a two-pass gate.** The embedded fmt
  pipeline canonicalizes the subset of rho it retains today; on the
  pinned subset (`test/subset.js` — a corpus slice plus the repo's fmt
  fixtures) formatting is byte-identical to `rho fmt` and
  fixpoint-stable. The pipeline still *accepts* some shapes it does not
  retain yet (structs, enums/match, floats, closures, ... — the
  compiler's parse-retention gaps, TODO.md T2.2+) and would mangle them
  silently: declarations dropped, expressions collapsed to `( + )`.
  The plugin therefore never trusts a single pass. It runs the fmt
  twice and **refuses the source** unless the formatted form re-parses
  and formats back to the same bytes; refusals throw an error carrying
  the compiler's parse diagnosis, so `prettier --write` fails loudly
  instead of corrupting the file. Expected behavior this generation:
  retained-subset sources format; known-lost shapes refuse (the wave-1
  census measured their mangled forms failing their own re-parse); and
  anything whose two passes disagree refuses — the gate is the
  guarantee, not the subset list. When the mirror grows, the artifact
  is regenerated, not patched.
- **Input capacity.** 224,402 usable source bytes per format — the raw
  slot literal is 224,403 bytes and embeds no NUL; its last byte is
  reserved, and the host writes the NUL there at run time (both
  numbers pinned in `assets/generation.json`).
  Larger sources refuse cleanly.
- **Blank input.** Empty or whitespace-only input returns `""` — what
  prettier core returns for empty input — without running the wasm (the
  compiler's fmt would synthesize a `fn main` skeleton for an empty
  program, and a formatter must not invent code). A comments-only file
  is *not* blank: its comments are dropped (see below) and the
  synthesized skeleton is what remains — the embedded pipeline's
  canonical law this generation (outside the pinned subset, so boot
  parity for token-less files is unmeasured).
- **Comments are dropped** — `rho fmt`'s canonical form itself drops
  them this generation (both boot and the embedded pipeline agree;
  this is the formatter's law, not the plugin's).
- **Single-file sources.** Multi-module programs (`use` trees) are not
  carried by the runtime bridge yet.

## Development

```bash
npm install
npm test          # vitest: real-prettier-API, byte-identity, fixpoint
npm run check:fmt # artifact ↔ chain consistency
```

The byte-identity suite compares the plugin path against
`build/rho fmt` from the worktree — build boot first
(`make build/rho` at the worktree root).
