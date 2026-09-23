# The rho language service

Design for the compiler-side language-service API: the positional query
surface (completions, hover, definition, format edits), error recovery on
incomplete source, the incremental-recheck boundary, the position
conventions (rho's byte columns vs LSP's UTF-16 columns), the thin-shell
law, and the wasm export story.

Status: **design, not implemented.** The starting point is the adapter
built this round, [tools/lsp](../tools/lsp/) — a node LSP server that
drives the real compiler (`rho.wasm` as a wasm32-wasi program, or a
native `rho`) through its CLI semantics. Everything below is grounded in
that adapter's code and in probes run against the compiler; each probe
quotes its exact command and output.

## 1. Where we are: the tools/lsp adapter

The adapter today (all paths under `tools/lsp/src/`):

| Concern | Where | How |
| --- | --- | --- |
| LSP server, stdio | `server.ts:55` | `createConnection(..., process.stdin, process.stdout)` — LSP over stdio, no network |
| Compiler host | `compiler.ts` | three backends: wasm in a worker thread (supervised: 10 s timeout, kill + respawn, `compiler.ts:190-237`), native CLI via temp files (`compiler.ts:260-288`; when the resolved binary is the boot seed, `fmt` routes through the wasm mirror — the seed carries no formatter), inert (`compiler.ts:307-330`) |
| One compiler call | `compiler-worker.ts:39-48` | write `/main.rho` into an in-memory WASI fs, run `rho check` / `rho fmt` as argv, read stdout/stderr |
| Diagnostics | `diag.ts:27-43` | regex `^(.*?):(\d+):(\d+): error: (.*)$` over the compiler's stderr lines |
| Positions | `diag.ts:107-160` | `LineIndex`: JS string offsets ↔ LSP (line, character) |
| Hover / definition | `symbols.ts` | a 523-line single-file syntactic scanner (`indexSymbols`, `resolveName`) — the shell's own mini front end |
| Formatting | `server.ts:264-296` | one whole-document `TextEdit` from `rho fmt` stdout; `[]` on parse errors |
| Policy | `server.ts:43-44,73-104` | 150 ms debounce, 512 KiB doc cap, sequence-number latest-wins, full-text sync (`server.ts:158`) |

Tested end to end by speaking real stdio LSP against the spawned server
(`tools/lsp/test/protocol.test.mjs`) and by unit tests over the compiler
backends (`tools/lsp/test/compiler.test.mjs`).

This adapter works, and its reliability contract (a wedged compiler never
takes the editor down) is the right shape. But look at what the shell
currently *knows*:

- `symbols.ts` re-implements a slice of the front end — declarations,
  params, locals, shadowing ("innermost binding wins"), with documented
  blind spots: no struct fields, no match-pattern bindings, no closures,
  no types, no cross-file resolution (`symbols.ts:1-15`).
- `diag.ts:27` parses the compiler's *human* output format, which lives in
  the compiler (`boot/src/util.c:286,293`: `"%.*s:%d:%d: error: %s\n"`).
  The shell and the compiler are coupled through an unversioned string.
- `diag.ts:59-74` guesses the token extent under a diagnostic with a
  regex, because the compiler reports only a start point, never a range.
- `diag.ts:87-91` maps the compiler's column onto LSP's character field
  verbatim (`char0 = d.col - 1`) — exact for ASCII, wrong otherwise (§3).

Every one of those is language semantics leaking into the shell. The rest
of this document is the plan for pushing them back where they belong.

## 2. The thin-shell law

**The shell is a byte pump and a babysitter. All language semantics live
in the compiler core.**

Concretely, the shell owns exactly three kinds of things:

1. **Transport** — LSP framing over stdio (or postMessage in a browser),
   `Content-Length` headers, JSON-RPC.
2. **Host lifecycle** — finding a compiler artifact (env vars, well-known
   paths — `compiler.ts:64-106`), supervising it (worker threads,
   timeout/respawn, the native-CLI fallback, inert degradation), and
   document sync (didOpen/didChange/didClose, versioning).
3. **Editor policy** — debounce, document size caps, capability
   negotiation, when to publish, log routing.

The shell owns **none** of: parsing, name resolution, type inference,
symbol extents, completion candidate computation, format-edit
computation, or diagnostic text. If a shell needs to know what a rho
program means to answer an editor question, that is a bug in the
boundary — the question belongs in the compiler's service API.

Why this law, stated as consequences rather than taste:

- **One semantic implementation.** VS Code, Neovim, the browser
  playground, and future integrations all get identical answers. The
  determinism pillar (`spec/spec.md:32` — same inputs, byte-identical
  outputs) extends naturally: same (files, query) → byte-identical
  answers, on every host and every target.
- **No drift.** A syntactic scanner in the shell rots with every language
  change (traits alone — 0.4.0 — would have been a rewrite; closures and
  struct fields are still missing). The compiler cannot rot relative to
  itself.
- **Honest degradation stays honest.** The adapter's inert mode answers
  *nothing* rather than something wrong (`server.ts:113-118`). When the
  shell has no semantics of its own, there is no temptation to serve
  stale approximations.

What this does **not** mean: it does not mean the compiler speaks LSP.
LSP is a shell concern (transport + editor policy). The compiler speaks a
small, target-independent query protocol (§5); a shell translates.

## 3. Positions: rho byte columns → LSP UTF-16 columns

This is the one place where the compiler's internal convention and the
wire convention genuinely differ, so it is specified here in full.

### 3.1 What the compiler produces

Measured, not assumed — probe run against the tracked compiler artifact:

```sh
$ cd /tmp/rho-ls-probe
$ printf 'fn main() -> i32 {\n  let a: string = "中文"; let b: i32 = a;\n  return 0;\n}\n' > main.rho
$ wasmtime run --dir . $RHO/site/assets/rho.wasm check main.rho
main.rho:2:42: error: initializer: expected `i32`, found `string`
```

On that line the error token `a` sits at 1-based **UTF-16 column 38** and
1-based **UTF-8 byte column 42**. The compiler reports 42.

The sources agree: the lexer counts columns in **bytes**. The boot lexer
advances `lx.col += (int)(lx.p - start)` over `char *` (`boot/src/lex.c:110`).
The self-hosted lexer advances `lx.t.col2 += (lx.t.pos - start) as i32`
over `u8` (`libs/compiler/lex.rho`, the `lex_file`/`ch_at` region). Positions are 1-based line,
1-based byte column. Tabs count as one byte; no expansion. Only `\n`
(0x0A) starts a new line; `\r` is an ordinary byte of whitespace that
counts toward the column (`boot/src/lex.c:82-92`) — a CRLF file's line
*contents* are unaffected because the CR sits between the last token and
the LF, but its byte length includes the CR.

The AST carries only these start points — there are no end positions and
no byte offsets on nodes (tokens store `line`/`col`; the self-hosted
token's `pos` field is set to 0 inside `emit_tok`,
`libs/compiler/lex.rho`, the `emit_tok` region). This is a real gap the service work must close
(§5.2): ranges need ends.

### 3.2 What LSP wants

LSP positions are 0-based line + 0-based offset in **UTF-16 code units**
from the line start (the LSP spec's `character` field). Editors on the
wire assume UTF-16; that is not renegotiable per-language.

### 3.3 The mapping rule

For a position `(L, C)` with `C` a 1-based byte column on line `L`:

1. Take line `L`'s bytes from the document's UTF-8 text (the document is
   stored as text; bytes come from the same buffer the shell sent the
   compiler, so they cannot disagree).
2. Walk the first `C - 1` bytes; decode UTF-8 incrementally.
3. Emit one UTF-16 code unit per code point below `U+10000`, two per code
   point at or above it. The resulting count is the 0-based LSP
   `character`.
4. Clamp: if the line has fewer than `C - 1` bytes (an out-of-sync
   diagnostic), stop at the line end. Never emit `end < start`.

The inverse (LSP position → byte column, needed when a request arrives:
hover, completion, definition) walks the same line accumulating bytes
per code point until `character` units are consumed.

Boundary placement: **the compiler speaks bytes end-to-end and never
knows UTF-16 exists; the conversion lives at the host boundary** — the
one place that holds both the bytes and the wire. This is not a
violation of §2: the rule is pure encoding, it is specified once (here),
and pinning it in the core would drag UTF-16 into a byte-oriented
compiler for no semantic gain. What §2 does forbid is *inventing*
positions shell-side — conversion, yes; heuristics, no.

The adapter today violates the rule by skipping the conversion
(`diag.ts:87-91`). The probe above shows the resulting error: the
diagnostic would land at character 41 instead of 37 — invisible on ASCII
fixtures (the current test pins an ASCII case,
`tools/lsp/test/protocol.test.mjs:130-132`) and visibly off by
`3 × (code points)` on any line with CJK before the error. The fix is
mechanical once the mapping function exists; it must land with non-ASCII
fixtures on both sides (compiler diag → LSP range, and LSP position →
byte column for hover).

### 3.4 Document-vs-compiler skew

The shell validates the document text it has; the compiler re-derives
line/column from the same bytes (the worker writes the exact text into
the MemFS, `compiler-worker.ts:42`). Because the round-trip is exact and
single-flight (latest-wins sequencing, `server.ts:73-104`), skew is
impossible by construction *provided* the conversion of §3.3 uses the
same text snapshot the compiler checked. Any future incremental scheme
must keep that invariant: positions are only ever interpreted against
the snapshot that produced them.

## 4. Incomplete source: the error-recovery strategy

An editor sends broken text constantly — mid-deletion, mid-typing. The
service's contract for broken source:

### 4.1 Today's behavior (measured)

The parser never aborts: `expect` reports and continues with the wrong
token (`boot/src/parse.c:26-33`), and lexing continues past lexical
errors (`boot/src/lex.c:144-148`). But the driver is phase-gated: any
parse diagnostic stops the pipeline before checking
(`boot/src/main.c:173-176`), so **type-level answers never arrive for
syntactically broken files**. And recovery is unfiltered — a probe:

```sh
$ printf 'fn main() -> i32 {\n  let x: i32 = ;\n  return 0;\n}\n' > main.rho
$ wasmtime run --dir . $RHO/site/assets/rho.wasm check main.rho
main.rho:2:17: error: expected an expression, found `;`
main.rho:3:3: error: expected `;` (`;`), found `return`
```

The second diagnostic is pure cascade noise: the missing-expression error
already explains line 2. Editors that show cascades train users to ignore
diagnostics, which is worse than showing none.

A missing module is reported outside the diagnostic format entirely —
`rho: cannot open missing/mod.rho` on **stderr** (probe: same harness,
stderr-only run reproduced it, stdout-only run printed nothing). The
adapter's regex drops it (`diag.ts:27-43`) and the README documents the
filter (`tools/lsp/README.md`, "Imports"). Silent dropping is the right
temporary call — a diagnostic with no position is unattachable — but it
must become a *positioned* diagnostic (anchored at the `use` token) in
the service API.

### 4.2 The design

Recovery has one goal: **maximize the queries answerable on the intact
parts of a broken file, at bounded noise.**

1. **Synchronization points.** The parser recovers at statement and
   declaration boundaries: on error, discard tokens up to the next
   `;`, `}`, or a declaration keyword (`fn` `struct` `enum` `trait`
   `use` `let` `const` `static` `pub`), then continue. Discard is
   recorded, not silent: one "skipped unexpected tokens" diagnostic
   maximum per recovery jump.
2. **Cascade suppression.** A diagnostic is suppressed when it points
   within the token span its predecessor skipped, or when the parser has
   already reported an error for the same construct without an
   intervening sync. Rule of thumb: **one primary error per
   construct**; the probe in §4.1 must yield one diagnostic, not two.
3. **Missing-token insertion.** Where the grammar wants a specific
   single token (`;` `)` `}` `,`), insert a virtual token, mark the node
   incomplete, and report once. Insertions never consume user tokens.
4. **Incomplete-node markers.** An AST node touched by recovery is
   marked; the checker skips *checking* it but keeps *resolving* around
   it. Concretely: completions, hover, and definition must work in a
   file whose middle function is garbage, as long as the surrounding
   declarations parse.
5. **Phase gating becomes per-query, not global.** Today `check` refuses
   to type-check anything once parse reported (§4.1). The service runs
   the checker on the recovered AST whenever recovery stayed inside
   function bodies (cases 3-4); it reports parse-only when structure was
   discarded (case 1). The driver's stop-at-first-phase behavior
   (`boot/src/main.c:173-176`) remains the *batch* CLI contract; the
   service API gets the finer verdict.
6. **fmt is exempt, permanently.** Canonical formatting of a broken file
   is undefined by design — `fmt` requires a full parse
   (`spec/spec.md:103-105`: `parse(fmt(parse(x))) == parse(x)`); the
   adapter already degrades to "no edits"
   (`tools/lsp/test/protocol.test.mjs:201-214`). Recovery never makes
   `fmt` guess.
7. **Determinism survives.** All recovery decisions are functions of the
   token stream only — no timing, no host state (`spec/spec.md:32`).
   Identical broken input yields identical diagnostics, byte for byte,
   on every target — asserted by the same twice-run gate the corpus
   uses.

## 5. The query surface

### 5.1 Shape

A **snapshot** per document version: parse + resolve + check results,
immutable, addressed by content. Queries are pure functions
`(snapshot, position, workspace?) → answer`. No query mutates; a new
document version produces a new snapshot. This is what makes the
supervision story trivial — a snapshot computation that dies is recomputed,
never partially trusted.

### 5.2 Prerequisite: ranges on every node

The compiler must carry byte spans (start and end byte offsets, plus the
derived line/byte-col for human output) on tokens, expressions,
statements, declarations, and — for diagnostics — on the *offending
token*, not just its start. `err_range` already exists but discards the
length (`boot/src/util.c:259-266`). Without ends there are no accurate
LSP ranges, no rename, no format-ranges; with them, the shell-side token
guess (`diag.ts:59-74`) is deleted.

### 5.3 The queries

Each query below names what it must see that the adapter cannot get
today — that delta is the reason the query lives in the core.

| Query | Input | Answer | What the core adds |
| --- | --- | --- | --- |
| `completions(pos)` | snapshot, byte pos | typed candidates with kinds + sort text | scope-aware name set (locals → module items → `use`d namespaces → prelude, in resolution order per `spec/module-system.md:21-31`), receiver-typed members and methods (`a.` / `Type.` contexts, primitive methods included), `use`-path candidates from the workspace file set; keyword/context snippets are shell-side sugar over the same list |
| `hover(pos)` | snapshot, byte pos | signature + type + owning module, with span | real types (the shell shows only declared syntax, `symbols.ts:27-29`); expression types at arbitrary positions, not just declarations |
| `definition(pos)` | snapshot + workspace, byte pos | file/span of the binding | cross-file resolution of `use` bindings and namespace members (`spec/module-system.md:5-31`); the shell resolves a single file only |
| `references(pos)` | snapshot + workspace | all reads/writes | same resolution machinery, reversed |
| `document_symbols(doc)` | snapshot | tree of decls with spans | the core's own decl list — replaces `indexSymbols` wholesale |
| `format(doc, range?)` | source | **edit list** (byte spans + replacement text), not raw text | computed by the real formatter; ranges enable format-selection; the whole-document case is the trivial edit list |
| `diagnostics(doc)` | snapshot | structured: start+end byte spans, severity, **stable code**, message | machine-readable channel replaces regex over human text; stable codes unlock code actions and editor filtering |

Deliberately **out** until there is demand measured in use: rename
(needs references + edit-atomicity), code actions (needs stable codes
first), semantic tokens (the highlighter in the site already covers the
browser need), inlay hints, call hierarchy. Listed so their absence is a
decision, not an oversight.

### 5.4 Diagnostics need codes

Every diagnostic gets a stable, documented code (`E0107`-style or
rho-idiomatic short names — the *stability* is the contract, the
spelling is not). The corpus's diag fixtures (`tests/lang/*/diag_*`,
`tests/lang/params`) are the natural home for
pinning codes alongside messages. Codes are what let an editor offer
"suppress this" or a quick-fix without string-matching English prose.

### 5.5 Where the API lives: three export shapes

**(a) `rho lsp` — a stdio query loop inside the compiler binary. The
recommendation for v1.**

The compiler gains a driver mode: it speaks a line-framed,
JSON-in/JSON-out request-response protocol on stdin/stdout (one request
per line, one response per line; responses reference the request id).
The protocol is *not* LSP — no capabilities, no document sync; the shell
keeps doing editor policy. The compiler process holds the session state
(snapshots, module cache) across requests, which is precisely what the
per-invocation CLI cannot do (`compiler.ts:260-288` spawns with a temp
file per call).

Why this shape wins v1:

- **Portable by construction.** The same binary serves node (spawned as
  a child), VS Code, and a browser page that drives it through the
  existing WASI shim (`tools/lsp/src/wasi.ts` is already a synchronous
  WASI preview1 host with args + fs + stdout/stderr — a stdio loop fits
  it unchanged). Native targets get the service for free; wasm gets it
  through the shim it already runs under.
- **It matches the toolchain's own story.** The compiler already drives
  itself over argv + stdio (`rho check`/`rho fmt`; the self-hosted
  compiler reads requests and writes artifacts through the per-target
  hooks — `boot/prelude/wasi.rho` implements exactly `__open/__read/
  __write/__program_args` for this). One more reader-loop driver is
  incremental; new link modes are not.
- **Thin shells become trivial.** A conforming shell is: frame LSP in,
  translate to query calls, pump bytes, frame answers out. That is the
  whole job.

**(b) Extra wasm exports — direct function calls, considered and
deferred.** Today the compiler's emitted modules export exactly
`memory` and `_start` (`boot/src/emit_wasm.c:1453-1456`), and
`rho.wasm` is itself such a module — there is no ABI to call into.
A second link mode that exports `rho_check(buf, len) -> reply` would
remove the WASI round-trip and is the right end-state for the browser
(one compile, many queries). It is deferred because it is toolchain
work (a new emission path with its own gates) for a latency win the
measurements in §6 do not demand yet.

**(c) CLI per request — today's tier, kept forever as the fallback.**
`rho check`/`rho fmt` semantics remain the degradation path when no
persistent session is possible. Everything above tier (c) is additive.

Tiering, with gates:

- **Tier 1** — `rho lsp` (shape a): diagnostics (structured, coded,
  spanned), hover, definition, document symbols, completions
  (scope-level; workspace files supplied by the host through the same
  MemFS/preopen mechanism `check` already uses for modules).
  Gate: byte-identical answers native vs wasm32-wasi on a golden
  transcript corpus; the adapter's protocol tests rewritten against it
  with zero behavior change except the §3 fix.
- **Tier 2** — references, workspace-wide cross-file, recovery per §4,
  snapshot caching across requests. Gate: recovery corpus (broken
  inputs → pinned diagnostic sets, cascade counts bounded).
- **Tier 3** — rename, code actions, format ranges. Gate: demand.

## 6. The incremental-recheck boundary

Measurements first, because they bound the problem:

```sh
$ time wasmtime run --dir . $RHO/site/assets/rho.wasm check main.rho
# hello-world check: 0.010s total (includes wasmtime startup)

$ cp rho/libs/compiler/check.rho big.rho   # ~4,600 lines — the largest
                                          # real rho module
$ time wasmtime run --dir . $RHO/site/assets/rho.wasm check big.rho
# 0.043s total, cold, including VM startup and the full check
```

A full check of the entire self-hosted compiler costs **43 ms cold**.
The adapter's own warm path is faster still — module compiled once per
worker, instantiation ~1 ms per request (`compiler-worker.ts:39-47`),
behind a 150 ms debounce (`server.ts:43`). **Conclusion: incrementality
is not a latency necessity at today's scales, and this document does not
schedule it.** What is specified now is the *boundary*, so that a future
incremental core does not require protocol or shell changes:

1. **The wire stays whole-document.** Full-text sync
   (`server.ts:158`) remains the truth; the shell always sends complete
   file contents. No incremental-sync negotiation, no edit translation
   at the boundary.
2. **Incrementality is an inside-the-core cache.** The service accepts
   `(path, content)` upserts and answers queries; internally it may key
   parse/resolve/check work by content hash and reuse untouched
   modules. The module system already defines the invalidation unit:
   a program is the root plus transitively `use`d modules, compiled as
   one unit (`spec/module-system.md:76-82`) — the cache is per module
   file, and a changed file dirties exactly its transitive includers.
3. **What must never be incremental early:** monomorphization and
   emission order (determinism, `spec/spec.md:165-169`); per-query
   answers that depend on cache *state* rather than snapshot *content*.
   A cache hit and a cache miss must produce byte-identical replies.
4. **Cancellation is superseded-by-version at the shell**
   (`server.ts:95-104`), and kill-and-respawn at the host
   (`compiler.ts:190-210`). The core never sees a "cancel" verb; stale
   replies are dropped by id. This stays.

The honest trigger for revisiting: an editor session where p95
check-latency exceeds the debounce budget on a real file. Nothing in the
tree today is that file.

## 7. What sinks from tools/lsp into the core

The ask's core question, answered component by component:

| Adapter component | Verdict | Rationale |
| --- | --- | --- |
| `symbols.ts` (523 lines: `indexSymbols`, `resolveName`, `identAt`) | **Sink; delete.** | It is a second-class front end with documented blind spots (fields, pattern bindings, closures, types, cross-file — `symbols.ts:1-15`). Tier 1's `document_symbols`/`hover`/`definition` queries replace it with the real model. It does not survive as a fallback: inert mode answers nothing (§2), so there is no tier where a wrong-ish shell index is preferred. |
| `diag.ts` `parseCompilerDiags` regex (`diag.ts:27-43`) | **Sink; delete.** | Structured, coded, spanned diagnostics from the core (§5.3-5.4). The regex is a string-level coupling to `boot/src/util.c:293` that breaks the first time the human format evolves. |
| `diag.ts` `tokenRangeAt` heuristic (`diag.ts:59-74`) | **Sink; delete.** | Real spans from the core (§5.2); the guess exists only because starts are all the compiler emits. |
| `diag.ts` byte→UTF-16 conversion (currently *missing*, §3.3) | **Keep in shell; implement correctly.** | Wire-concern, encoding-only; specified once here, pinned by non-ASCII fixtures both directions. |
| `diag.ts` `LineIndex` (`diag.ts:107-160`) | **Keep.** | It *is* the §3.3 conversion machinery (offset↔position) plus line surgery; pure encoding. |
| `compiler.ts` backends/supervision (worker, timeout, respawn, native fallback, inert) | **Keep in shell.** | Host lifecycle, not semantics. Under `rho lsp` this shrinks to "keep the child process alive, restart on hang" — simpler than today's worker protocol, same guarantees. |
| `compiler.ts` artifact discovery (env, well-known paths) | **Keep.** | Host concern. |
| `server.ts` policy (debounce, doc cap, latest-wins, sync kind, publish/versioning) | **Keep.** | Editor policy. |
| Whole-document format edit (`server.ts:279-291`) | **Reshape, keep behavior.** | The core returns an edit list (§5.3); the whole-document case is its first element. Shell applies verbatim. |
| The WASI shim `wasi.ts` | **Keep; grows a stdio pump.** | It is the byte pump that lets the *same* compiler binary serve the browser and node (§5.5a). |

Net effect: the shell loses every line that knows what rho *is* —
roughly the whole of `symbols.ts` and half of `diag.ts` — and keeps the
bytes, the babysitting, and the policy.

## 8. Testing law

- **Golden transcripts.** Tier 1's gate is a corpus of (files, request
  sequences, replies) where replies are compared byte-for-byte; run on
  native and wasm32-wasi, both must match (determinism pillar,
  `spec/spec.md:32`). The existing adapter tests already model the
  style — real stdio, real server (`protocol.test.mjs`) — and migrate
  rather than multiply.
- **Position fixtures.** Every position-touching answer is pinned on a
  CJK/emoji/combining-mark fixture, both directions (§3.3). The probe
  from §3.1 becomes a test case verbatim.
- **Recovery corpus.** Broken inputs with pinned diagnostic sets and a
  maximum-cascade assertion (§4.2); the probe from §4.1 pins the first
  case: one diagnostic, not two.
- **Degradation is tested, not hoped for.** The adapter's crash/respawn
  and inert-mode tests (`compiler.test.mjs:71-115`) carry over
  unchanged — they test the shell, which survives this redesign.

## 9. Non-goals

- The compiler never speaks LSP; shells translate (§2).
- No daemon, no semantic database, no background indexing service: one
  request loop per session, state bounded by open documents (§5.5a).
- No formatting configuration, ever — one true style
  (`spec/spec.md:103-105`); the service exposes no format options even
  where LSP requests carry them.
- No incremental *wire* protocol (§6.1) and no scheduled incremental
  core (§6) — the boundary is specified; the machinery waits for
  measurements.
