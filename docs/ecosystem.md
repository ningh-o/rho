# rho — the ecosystem workstream

The language law and the road to 0.1.0 live in [TODO.md](../TODO.md).
This file carries the four products that surround the toolchain — the
benchmarks, the vite plugin, the prettier plugin, the LSP — each with
its goal, its dependencies (the TODO items that must close first), its
shape, and its acceptance law. Nothing here blocks 0.1.0; nothing here
starts before its dependencies close. GitHub CI is repo infrastructure,
not a product: it lives in TODO.md as T3.0.

**Version policy.** The language and the toolchain freeze at 0.1.0 —
one version, no evolution (TODO.md's law). The npm-published artifacts
below are products *of* the toolchain, not the toolchain: each carries
its own independent semver, its own tags, its own changelog. A package
release never retags the compiler, and every package pins the exact
compiler generation it was built from.

## 1. bench

**Goal.** A trend line, not a gate: compile time (boot and the
self-hosted chain over the corpus) and generated-code execution time,
printed per run, watched over time.

**Depends on.** T2.x — the self-hosted chain must be stable enough for
the numbers to mean something — and T3.1, whose rebuilt gate harness
this rides. Meaningless while the compiler still grows weekly.

**Shape.** `tools/bench/`: deterministic inputs (the corpus itself),
pinned wasmtime and wabt versions, fixed iteration counts, medians
printed. The archive branch's `bench/` is reference — read it, never
copy it.

**Acceptance law.** Same tree, same machine, byte-identical numbers
(determinism is the language's law; the harness obeys it too). Every
leg time-capped. Numbers trend; nothing fails on them until the
plateau says they can.

## 2. vite-plugin-rho

**Goal.** The site and course build path (T5.1/T5.2): compile `.rho`
sources to wasm inside vite dev/build, retiring hand-pinned wasm
artifacts.

**Depends on.** T5.1's asset law (deploy asset = one generation past
the seed + `wasm-opt -Oz --enable-bulk-memory`) and T5.2 — the course
app is the first consumer.

**Shape.** An npm package: a vite transform hook for `.rho` → wasm,
build cache keyed by compiler generation, flags shared with the CLI.
One compiler generation, one plugin release.

**Acceptance law.** The plugin's wasm is byte-identical to the CLI's
for every corpus program (same compiler, same flags); the consuming
sites' own suites stay green.

## 3. prettier-plugin-rho

**Goal.** `prettier` support for `.rho`, so editors and build
pipelines format with the canonical form without shipping the CLI.

**Depends on.** T5.1's wasm asset pipeline (the embeddable fmt build —
one generation past the seed + `wasm-opt`) and a packaging workstream
to ship it to npm (T4.5's rho-pkg is the toolchain-side first cut).
The formatter itself is already complete: boot fmt and the self-hosted
fmt are byte-identical and fixpoint-stable (`tests/run-fmt-self.sh`).

**Shape.** A thin client, never a second formatter: the plugin embeds
the compiler's fmt wasm and hands prettier the canonical text
verbatim. The LSP (below) rides the same wasm fmt — one formatting
truth, two clients.

**Acceptance law.** Formatting every corpus program through the plugin
is byte-identical to `rho fmt`; the fixpoint law holds through the
plugin path; a plugin release pins the compiler generation it embeds.

## 4. LSP

**Goal.** Diagnostics, hover, go-to-definition, completion, format —
one language service, many editors — plus style hints: a `mut` view
that is never written through narrows (hint, never an error).

**Depends on.** **The 0.1.0 freeze.** The language surface must stop
moving first; an LSP built on a weekly-changing grammar is rework as a
service. Rides the T5 editor work (editor code flows one way:
repo → app).

**Shape.** A server wrapping the compiler's check layer over wasm —
the same embedding the playground uses, so diagnostics parity is
structural, not aspirational. The archive branch's `tools/lsp/` and
`docs/language-service.md` are required reading — read them, never
copy them.

**Acceptance law.** Diagnostics byte-identical to `rho check` on the
corpus (reusing T3.1's diagnostic-parity leg); every request
time-capped; a hung server degrades to no-LSP, never to wrong
diagnostics.
