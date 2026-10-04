# rho bench

Compile-time and execution-time **trend lines** over the programs
tier (the corpus dissolved at the freeze; tests/suites/programs IS its
successor — same flat listing, same `// exit:` grading) —
[docs/ecosystem.md §1](../../docs/ecosystem.md). A trend line, not a
gate: nothing here fails on its numbers; they are watched over time.

```
tools/bench/
  src/cli.ts       entry point (refusals exit 2; a round exits 0)
  src/pins.ts      pinned wasmtime/wabt + the startup self-check
  src/args.ts      flag parsing (pure) + --help text
  src/corpus.ts    corpus discovery (sorted) + `// set:`/`// exit:` headers
  src/runner.ts    spawn + hard per-invocation time cap (DNF law)
  src/legs.ts      the timing families: compile, exec, chain
  src/report.ts    the deterministic report renderer
  src/median.ts    the median of a fixed sample
  test/*.test.ts   the contracts above, pinned (node:test, zero deps)
```

## Prerequisites

1. The boot binary: `make all` from the repo root (the harness never
   builds boot; `RHO=` overrides the default `./build/rho` — a
   relative `RHO=` value resolves against the repo root too).
2. The pinned tools on PATH — **wasmtime 40.0.0** and **wabt 1.0.39**
   (`wat2wasm`), the same pins as `.github/workflows/ci.yml`. The
   harness verifies both at startup and **refuses to run (exit 2)** on
   any mismatch: a trend line measured across tool generations is not
   a trend line.
3. Node >= 22.18 (the tests and the harness are plain TypeScript that
   Node runs directly via type stripping; no install step exists).

## Running

```bash
node tools/bench/src/cli.ts            # compile + exec legs over the corpus
node tools/bench/src/cli.ts --help     # every flag, the pins, the law
node --test "tools/bench/test/*.test.ts"   # or: npm test (from tools/bench/)
```

The harness runs the same from any cwd: every input path (the corpus,
the compiler sources, `build/bench/`, the default boot binary) is
resolved against the repo root, so `node tools/bench/src/cli.ts` from
the root and `npm run bench` from `tools/bench/` are the same run.

Legs:

| leg       | what                                                        | default |
| --------- | ----------------------------------------------------------- | ------- |
| `compile` | boot builds each corpus program; median of `--iters` (5)    | on      |
| `exec`    | wasmtime runs each built program, graded by its `// exit:` header; median of `--exec-iters` (5) | on |
| `chain`   | mirror build -> mirror run -> child assemble, `--chain-iters` (1) | **off** |

The chain leg stays off until T3.1 leg 4 (the self chain) is green —
numbers from a moving compiler are not a trend line. Turn it on with
`--chain` when the gate says so.

Every invocation of every leg is hard-capped (`--cap-ms`, default
30 s; chain legs `--chain-cap-ms`, default 600 s — the gate's own leg
budgets). A cap hit is a **DNF row**; the round moves on. A row reads
`OK` only when it delivered all of its fixed samples clean.

With the default `--compile`, a program whose build did not deliver in
the same round gets a **starved DNF** row in the exec leg — the leg
never times `build/bench/<name>.wasm` bytes it did not just produce
(stale artifacts carry no provenance). With `--no-compile`, artifact
existence is verified at startup; freshness is the operator's trend
hygiene.

## Expected output shape

stdout carries the report and nothing else (even under `--verbose`,
tool output goes to stderr). Shape (values illustrative):

```
bench: wasmtime 40.0.0, wat2wasm 1.0.39, iters=5, exec-iters=5, cap=30000ms, chain-cap=600000ms
== leg: compile (boot build, median of 5)
  001_hello          OK        12.3 ms
  002_arith          OK        11.8 ms
  ...
  t06_params_widen   DNF  > 30000.0 ms
== leg: exec (wasmtime run, median of 5)
  001_hello          OK         3.2 ms
  t07_build_failed   DNF  stage starved by a dead upstream stage
  ...
== leg: chain (mirror build -> mirror run -> child assemble, median of 1) -- skipped: pass --chain (off until T3.1 leg 4, the self chain, is green)
```

Row statuses: `OK` (median of the full fixed sample count), `DNF` —
either a cap hit (the governing cap printed as `> cap ms`) or a stage
starved by a dead upstream stage (it prints exactly that sentence: the
stage never ran, so no cap was exceeded and `> cap ms` would
misattribute a hang), `ERR` (the run ended where its stage demands
success — boot build, artifact load, and the chain stages demand exit
0; an exec-leg run is graded by the program's own `// exit:` header,
the same first header the suite runner scores by
(`tests/run-corpus-repo.sh`): a run that lands on the designed code —
0, or 42 for `002_arith`, or the designed 101 of the six panic
programs — is the measurement, and anything else is an ERR row with
the exit code printed: an undesigned 101 is a trap, a wrong code a run
that did not finish the way its text promises). A program without a
header designs exit 0.

## The determinism acceptance

docs/ecosystem.md §1: "Same tree, same machine, byte-identical
numbers." The harness obeys the law where a measurement can: the
report furniture is byte-stable — results **sorted** (plain code-unit
order, never `localeCompare`), **fixed-width** columns (a DNF cell is
exactly as wide as a measured cell), **no timestamps**, no hostnames,
**no machine-local absolute paths**, pure ASCII, and no tool output
on stdout. The measured time cells are the one thing that moves —
they are measurements, not furniture. Accept both facts with the
scrubbed diff:

```bash
node tools/bench/src/cli.ts > /tmp/bench-a.txt
node tools/bench/src/cli.ts > /tmp/bench-b.txt
diff <(sed -E 's/[0-9]+(\.[0-9]+)? ms/… ms/' /tmp/bench-a.txt) \
     <(sed -E 's/[0-9]+(\.[0-9]+)? ms/… ms/' /tmp/bench-b.txt)
# silence = the furniture is byte-identical; only the measured cells moved
```

`diff /tmp/bench-a.txt /tmp/bench-b.txt` raw shows exactly the
measured cells and nothing else — that is the expected shape of the
raw diff on a quiet machine (small numeric drift; every structural
line identical).

Refusals (never a report, always stderr + exit 2): tool missing or
version mismatch; boot binary missing; `--filter` matching nothing;
`--exec` without `--compile` when artifacts are missing under
`build/bench/`; the corpus directory missing or unreadable; a `*.rho`
corpus entry that is not a readable file; the chain leg without
`libs/compiler/`, with a directory that cannot be listed, or with an
unreadable module in it; an artifacts directory that cannot be created
under `build/`.

## Honest limits

Where bench's header reading deliberately differs from the corpus
runner's, recorded rather than hidden (both latent today — the corpus
carries no `// set:` headers and no malformed `// exit:` headers as of
2026-09-27):

- `// set:` values: bench reads the **whole rest of the line** as one
  `--set` value (the section-17 line law, one header = one build
  parameter). The runner word-splits header lines instead (unquoted
  `$(sed -n 's/^\/\/ set: //p' …)` under zsh's IFS), so a value
  containing a space would reach the compiler as several arguments
  there and as one here. Bench keeps the whole-line reading; the
  corpus does not exercise the fork.
- `// exit:` values: bench grades with a strict
  `^// exit: <digits>$` match and treats a header that is not a plain
  non-negative integer as absent (designed exit 0). The runner's
  `[ "$rc" -eq "$want_exit" ]` errors and fails the program on such a
  value instead — a grading divergence, not a measurement one.

## Tests

```bash
cd tools/bench && npm test        # node --test "test/*.test.ts", zero deps
cd tools/bench && npm run typecheck   # tsc --noEmit (needs the devDependencies)
```

The tests pin the contracts without spawning any tool: the median's
exact reduction, the report golden (byte-for-byte, hand-computed),
the four determinism properties, flag parsing and its refusals, the
version pins against `ci.yml`, the refusal paths of the startup check,
and the legs' DNF / starvation / skip semantics (cap cells vs the
starvation sentence; exec never timing a program the compile leg did
not deliver; nonzero exits as ERR; repo-root-resolved spawn paths) —
all against injected fake runners and throwaway temp dirs.
