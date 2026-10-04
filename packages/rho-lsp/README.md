# rho-lsp

A Language Server Protocol server for the [rho language](https://github.com/ningh-o/rho).
Diagnostics come from the real compiler — not a re-implementation: every
published diagnostic is the compiler wasm's own refusal, produced
through the same boot → self-hosted-compiler → wasmtime face the repo's
gates run. Message parity with `rho check` is structural (same
toolchain, same invocation shape the corpus differential grades), and
honest about today's shape: the wasm face's diagnostics are
**positionless** (it prints `check: ...` refusal lines, no
`file:line:col`), so they attach verbatim as file-level diagnostics.
Boot's positioned face already converts through the byte-column law
(and a positioned line naming a *different* file degrades to
file-level rather than land on the wrong text); in-text ranges land
the day the checker reports positions.

## Capabilities

| Feature | Semantics | Truth source |
| --- | --- | --- |
| Diagnostics | full check on open/change, debounced, latest-wins | the compiler (wasm face) |
| Formatting | one whole-document edit, canonical form, or nothing | the compiler's fmt face (same truth as `rho fmt` and the prettier plugin) |
| Hover | the declaration line of the name under the cursor | a minimal local scanner |
| Go-to-definition | same-document declaration | a minimal local scanner |
| Completion | declarations above the cursor + the prelude + keywords | a minimal local scanner |
| Style hints | a `mut` binding never written through narrows — a Hint, never an error (design §18) | a conservative local scan |

Honest scope: hover/definition/completion and the mut scan are lexical
services with documented blind spots (struct fields, match-pattern
binders, closure params, cross-file resolution, alias tracking). The
mut scan is conservative in both directions: any `x.m(...)` method call
counts as a possible write (receiver mut-ness is not visible lexically,
so a read-only receiver can hide the hint), and "…" string literals or
comments never create declarations or writes (known limitation: the
triple-quoted verbatim form is not modeled — one containing a `"` can
leak its middle and hint on a ghost declaration; a hint, never an
error). The compiler exports no
query surface today (the wasm module exports `memory` and `_start`);
when it grows one, these retire. They answer *nothing* rather than
guess.

## The embedding face

Per request the runner performs the canonical two-step the repo's own
suites pin (`tests/run-corpus-diff.sh`, `tests/run-selfhost.sh`):

1. `rho build libs/compiler/main.rho -o <tmp>/face.wasm --set SRC=<the
   document> --set MODS=<open sibling modules> [--set FMT=1]`
2. `wasmtime <tmp>/face.wasm` — bare; the kernel imports only
   `fd_write` and `proc_exit`, so argv is unreadable.

Faces: the build failing is toolchain trouble (never your
diagnostics); exit 0 accepts; exit 1 refuses with the diagnostics on
stderr; anything else is a crashed compiler — a failure, never a
diagnostic. The document's module tree rides MODS as
`@MOD@ <path>\n<text>` blocks (same-directory open siblings today;
subpackages are future work).

**Transport budget.** Each `--set` value is a single argv element, and
POSIX caps one element at `MAX_ARG_STRLEN` = 128 KiB (Linux; execve
fails with E2BIG above it — macOS's ceiling is higher, so the Linux
limit is the portable budget here). A request whose `SRC=` or `MODS=`
value exceeds 128 KiB is **skipped with cleared diagnostics on every
platform** (uniform behavior, driven by the lowest common ceiling) —
skipped, never half-served, and never counted as a toolchain failure:
a large document must not latch the server degraded. Boot reads
`--set` values from argv only, so a file/stdin transport is not
available without changing the compiler face.

`FMT=1` produces the canonical text — the same formatting truth the
prettier plugin serves. Broken source formats to *no edits*, never a
guess.

## Version self-validation

`tools/build-check.mjs` builds the stock check/fmt wasm pair, runs a
double-build determinism leg, and writes `artifacts/generation.json`:
the rho commit, per-source SHA-256s, artifact SHA-256s, and pinned tool
versions — byte-stable, no timestamps. At startup the server verifies
the pin (sources and artifacts re-hash) and goes **inert** on any
mismatch: it still speaks LSP and answers capabilities, but serves
nothing. A wrong-generation toolchain must not produce confident
garbage.

## Reliability (the degradation law)

- Every runner call is time-capped twice: the runner kills its own
  children at the cap; the server abandons anything that never settles.
- A failed or timed-out check publishes **no diagnostics** for the
  document — never stale bytes, never invented ones.
- Consecutive failures latch **degraded mode**: the server keeps
  speaking LSP, publishes nothing, answers null, and logs once.
  Restart to retry. no-LSP, never wrong-LSP.

## Running

```sh
npm install
npm run build:generation   # boot -> check/fmt wasm + generation.json (needs build/rho)
npm run build              # dist/
node dist/main.js          # LSP over stdio
npm test                   # vitest
```

Environment: `RHO_LSP_REPO` (rho root; defaults to the package's tree
home), `RHO_LSP_RHO` (boot binary; default `<repo>/build/rho`),
`RHO_LSP_WASMTIME`, `RHO_LSP_ARTIFACTS`. Missing configuration makes
the server inert — never wrong.

`initializationOptions`: `mutHints` (bool, default true — the §18 hint
switch), `requestTimeoutMs` (number, default 5000).
