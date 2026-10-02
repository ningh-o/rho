# programs/ — the whole-program interaction pins

The differential's base (T6.4): every case here is compiled and
executed by BOTH compilers — boot and the self-hosted one — on every
`make test` (tests/run-corpus-diff.sh), and stdout + exit code must
agree. The pin is the case count (94). This is the layer that never
retired from the corpus: whole-program behavior, held forever.

Two golden forms:

- **Textual** (`*_test.rho`): the golden is `// out:` headers judged
  by the suite verb itself (`rho test tests/suites` — this directory
  is part of the suite tree), `// exit:` pins the code, `// set:`
  feeds build parameters. One case may carry `// rawout:` — the
  expected stdout tail WITHOUT the trailing newline, for a program
  whose last printf deliberately ends mid-line.
- **Byte** (no `_test` suffix, a sibling `.out`): goldens that carry
  non-UTF8 bytes cannot ride a text header; `tests/run-corpus-repo.sh`
  compares them byte-for-byte through `rho run`.

The `geom/`, `pk/` and `web/` directories are the module-system
packages the package-using cases import (`use geom;` resolves against
the entry's directory); the differential bakes them into the
self-hosted side under the same module paths.

Provenance: dissolved from `corpus/` at the freeze — fourteen smoke
shapes died under confirmed suite holders, fourteen teaching programs
promoted to `examples/`, and the ninety-four pins here re-anchored
from the old bare `.rho` + `.out` form into these two.
