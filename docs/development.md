# Development

Commands, layout, and pipelines for working on rho itself.

## Commands

```bash
make                # build boot (build/rho) — clang, C11, -Wall -Wextra -Werror
make test           # selftest + check tests + emit tests + fmt tests + corpus
make selftest       # unit tests (arena/lexer/parser/dump)

./build/rho build <file.rho> -o out.wasm [--set name=value] [-g]
./build/rho run   <file.rho> [--set name=value]
./build/rho check <file.rho> [--set name=value]
./build/rho fmt   <file.rho>
./build/rho dump-ast <file.rho>
```

`build` emits WAT beside the output and assembles with `wat2wasm`
(pinned wabt); executable outputs always go through a temp file and
rename. `run` executes under `wasmtime`, mapping traps to the panic
catalog (stack overflow = defined panic, exit 101); legitimate exit
codes pass through.

## Layout

| path             | what                                             |
| ---------------- | ------------------------------------------------ |
| `boot/`          | the C seed compiler (the reference compiler)     |
| `boot/wat/`      | kernel.wat — the runtime kernel, standalone-testable |
| `spec/`          | the language law: syntax, types, modules, spec   |
| `corpus/`        | behavioral corpus (programs + recorded goldens)  |
| `tests/`         | suites + the corpus/check/emit/fmt runners       |
| `tools/`         | embed_prelude.py, embed_kernel.py (deterministic) |

boot source map: `lex.c` `parse.c` `check.c` `check2.c` `check3.c`
(front half), `emit.c` `fmt.c` (back half), `kernel_wat.c`
`prelude.c` (generated embeds — regenerate with the tools, never edit).

The checker annotates the AST in place: `Node.sem` carries the
expression type, `sem2` the chosen overload / variant; the emitter
reads both. Generic instantiations clone their bodies (annotations are
never shared across instances).

## Testing law

- Every commit carries its tests, and they pass (`make test`).
- Corpus goldens are **regenerated from the seed**, never hand-written;
  `tests/run-corpus-repo.sh` judges stdout byte-exact + exit code.
- When adapting an archive program (tranche 2), the arbiter is the
  ARCHIVE golden unless the new law changed the behavior — then the
  adaptation note in `corpus/PLAN.md` records why.
- Never record a golden from a run you haven't diffed against the
  archive's expectation (self-recorded goldens once masked a printf
  assembly bug).

## Known open ends

See `TODO.md` — the living backlog with the phase plan. The archive
branch (`archive/pre-0.1.0`) is reference-only: read it, study its bug
ledgers, never copy code from it.
