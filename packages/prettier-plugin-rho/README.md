# prettier-plugin-rho

`prettier` support for the [rho](https://github.com/ningh-o/rho) language: format
`.rho` sources with the rho toolchain's own formatter — one pinned compiler
generation, embedded as wasm — without shipping the CLI.

A thin client, never a second formatter: the plugin runs the compiler's `fmt`
face over your source and hands prettier the canonical text verbatim. Prettier's
layout machinery never re-formats a byte of it — there is exactly one
formatting truth, the same wasm the site's playground uses (and the LSP will).

## Install

```sh
npm install --save-dev prettier prettier-plugin-rho
```

## Use

`.prettierrc`:

```json
{
  "plugins": ["prettier-plugin-rho"]
}
```

Then `npx prettier --write .` formats `.rho` files alongside everything else,
and editors using prettier format rho sources in place. Syntax errors surface
as normal prettier parse errors, carrying the compiler's diagnostic text.

The package also exports the runtime face directly:

```js
import { fmtRho, loadGeneration, warmCompiler } from "prettier-plugin-rho/runtime";

loadGeneration(); // the pinned generation record (assets/generation.json)
warmCompiler();   // compile the embedded module once, upfront (optional)
fmtRho(source);   // { ok, text, stderr, exitCode } — `rho fmt`, in-process
```

Everything is synchronous: the compiler artifact imports no suspending WASI
calls, so the plugin runs on the synchronous WebAssembly APIs — no
`node:wasi` experimental flag, no async parser.

## How it fits together

- `src/index.js` — the prettier plugin: `parsers.rho` runs the fmt face and
  wraps the canonical text; `printers.rho` returns it verbatim.
- `src/runtime.js` — the fmt face: instantiates the pinned artifact over an
  in-memory filesystem (`/main.rho` + `/mode` = `fmt`, the app-face contract
  the site's playground uses), stdout is the canonical text, a refusal rides
  stderr with a nonzero exit exactly like `rho fmt`.
- `src/wasi.js` — the pure-JS WASI preview1 shim, adapted for Node from the
  site's playground shim (the `vite-plugin-rho` sibling ships the same
  lineage). No JSPI stdin provider (Node does not expose it); `path_open`,
  `path_rename`, the preopened fd 3, and the tmp-then-rename semantics are
  kept — the compiler's import surface needs them.
- `assets/rho-compiler.wasm` — the compiler artifact: the self-hosted mirror's
  app face with an empty `MODS_APP` (the §7 chain-law shape), shrunk with
  `wasm-opt -Oz --enable-bulk-memory --enable-multivalue` — the same
  shrinker step the site's deploy asset rides.
- `assets/generation.json` — the generation pin (see below).
- `tools/build-artifact.mjs` — the bake: rebuilds the artifact from the rho
  worktree, probes the fmt face against boot's `rho fmt` (raw AND optimized),
  asserts two chain builds are byte-identical, and writes the pin.
  `npm run check:artifact` re-bakes and verifies the committed artifact still
  matches the chain — byte for byte.

## The generation pin

One generation, one release (docs/ecosystem.md's version policy):
`assets/generation.json` is the package's ONLY generation pin. It records the
rho commit, sha256 over every `boot/` and `libs/compiler/` input, the bake
tools' versions (`wasm-opt`/binaryen, wasmtime, and the gate toolchain's
wat2wasm — recorded only, since the fmt face emits text and never assembles
WAT), the artifact's sha256 and byte count, and the exact bake commands.

## Formatter scope

The embedded formatter is the self-hosted `fmt` — the repo's subset-grammar
formatter, pinned against boot's `rho fmt` by `tests/run-fmt-self.sh` on the
same scope (constants, statics, functions, if/while, strings with escapes,
casts, comment replay including same-line tails). That is the formatter the
0.1.0 toolchain freeze ships; the plugin does not — and must not — be a second
formatter that papers over the difference.

Concretely, over the full programs tier (`tests/suites/programs/`, 96
programs):

- **Plugin fidelity** — prettier's output is byte-identical to the fmt face's
  stdout for every one of the 96 (the plugin-path law this package pins in
  `test/parity.test.js`).
- **Parity with `rho fmt`** — byte-identical on the fmt-self suite (pinned in
  `test/parity.test.js` against the CLI). Outside the subset grammar the
  frozen formatter diverges from boot's `rho fmt` (68 of the 96 tier programs
  today — enums, match expressions and other post-subset forms render
  differently), so formatting such programs through any client of this
  generation will not match the CLI byte for byte.
- **Fixpoint** — `fmt(fmt(x)) == fmt(x)` holds through the plugin for the 48
  tier programs whose face output is stable, and on the fmt-self suite in
  run-fmt-self's settle form. The other 48 drift at the formatter itself (the
  plugin cannot prove a law the embedded formatter does not satisfy);
  `test/fixpoint.test.js` pins the gap count, which may only shrink.

These gaps close the day `libs/compiler/fmt.rho` grows to the full language
(post-freeze toolchain work): re-bake, flip the pins, cut a release. Nothing
in the plugin changes.

## Known limitations

- **Line endings are prettier's law.** Prettier normalizes the SOURCE's line
  endings (`\r\n` and lone `\r` → `\n`) before any parser runs, for every
  language. A file whose comments or strings carry a raw carriage return
  (one programs-tier fixture does) is therefore formatted from normalized
  text; if the raw byte matters, format that file with `rho fmt`.
- **Range formatting and cursor mapping** follow prettier's generic machinery
  over a whole-file AST wrapper; the canonical form is a whole-file property,
  so per-range reformatting of rho sources is not meaningful and not
  supported.
- The embedded wasm is synchronous and single-file; formatting runs on the
  Node main thread. Large sources are fine — the face streams output in
  512-byte slices — but the plugin does not parallelize across workers.

## Releasing

- `npm run build:artifact` — re-bake the artifact + regenerate the pin
  (requires the worktree's `./build/rho` and `wasmtime`/`wasm-opt` on PATH).
- `npm run check:artifact` — verify the committed artifact against the chain.
- `npm test` — the acceptance suite (see above).
- Bump `CHANGELOG.md`, cut the release. A release never retags the compiler —
  it pins the generation it embeds.
