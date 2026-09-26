# The conformance suites (T3.4) — spec §17 made executable

Run with the verb: `./build/rho test tests/suites [filter]` (this is
the `make test` leg; the verb is T3.5).

Every case is a `*_test.rho` file — a normal rho program whose
headers pin behavior — or a case directory holding a `main.rho` entry
that carries the headers (multi-file cases: packages, facades). Each
case compiles as its own program instance under a hard time cap; the
verb exits 1 on any failure, including when nothing matched.

## The headers

| header              | judges                                                                  |
| ------------------- | ----------------------------------------------------------------------- |
| `// out: LINE`      | pins one stdout line exactly (line + newline); none = empty stdout      |
| `// exit: N`        | pins the exit code (default 0; panics exit 101)                          |
| `// set: name=v`    | feeds `--set` build parameters (§7)                                      |
| `// expect: SUB`    | check must fail, diagnostics naming EVERY substring (one per line)       |
| `// err: SUB`       | run must exit nonzero, stderr containing EVERY substring (panic catalog) |
| `// pending: T3.x`  | expected-fail ledger for ratified law not yet implemented (see below)    |

Every file also names its law: `// spec: <doc> §<n>` — the §11
conformance map generates from these anchors; a rule whose test does
not exist is a rule not implemented.

## The pending ledger

`// pending: T3.x` marks a case for law already ratified but not yet
implemented (§18 mut view → T3.6, §19 match ergonomics → T3.9, item
imports → T3.7). A pending case must FAIL its own criteria today; the
verb counts it as `pending`, not `fail`. When the law lands, the case
passes, the verb prints `PROMOTE` — and counts the run as failed until
the marker comes off. Promotion = removing the marker in the commit
that lands the law. A stale ledger cannot hide under a green gate.

## Areas (the §11 map)

`lang` core runtime law · `modsys` modules and packages · `opt`
(build-parameter folding; the optimizer suite arrives with T2.x) ·
`eq` the comparability law · `params` build parameters · `multiline`
verbatim strings · `strops` the string law · `diag` diagnostics and
the panic catalog · `emit` behavioral emit fixtures (the old
tests/emit leg, now judged by the verb: they run and pin stdout) ·
`check` diagnostic and positive check fixtures (the old tests/check
leg — `// expect:` pins refusal diagnostics, positives now RUN with
`// out:` pins) · `set` the --set widened face (the old
run-set-tests.sh leg — one variant per override, the refusal rides
`// expect:`).

The verb is the main test framework (2026-09-27 migration): every
language-behavior test lives here. The shell legs that remain in
`make test` are cross-compiler infrastructure — the fmt roundtrip,
fmt-self parity, the selfhost loop, the boot-vs-mirror differential,
the corpus replay, and the robustness probes — none of them is
expressible as a single-program test.
