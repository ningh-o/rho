# tests/suites/gap — the 2026-09-26 boot-vs-spec audit, as cases

Every file here pins a ratified law the boot does not hold yet
(`tests/GAPS.md` is the prose ledger behind them).

- `// pending: gap/<id>` — the case fails today by its own criteria
  (counted "pending"; the leg stays green). When boot starts holding
  the law, the case passes, the runner prints `PROMOTE`, and the leg
  goes red until the marker comes off. The promotion IS the audit
  closing — never delete a pending case without running it.
- `// expect: <file>:<line>:` — refusal pins whose future diagnostic
  wording is unknown: any compile error reported on that line of that
  file satisfies the pin.

Module-law cases are case directories (a `main.rho` entry plus its
private sibling modules); everything else is a single file.
