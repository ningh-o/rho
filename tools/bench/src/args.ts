// args.ts — the command-line surface, parsed by a pure function so the
// tests can pin it without spawning anything.
//
// Law: docs/ecosystem.md section 1 — "fixed iteration counts". Every
// default is a constant here; nothing is read from the environment
// except RHO (the boot binary path). Invalid input is a refusal, not a
// fallback: the CLI prints the reason and exits 2 (the repo's
// usage-exit convention, boot/rho.h EXIT_USAGE).

import { PINNED_WABT, PINNED_WASMTIME } from "./pins.ts";

export interface BenchArgs {
  readonly help: boolean;
  readonly verbose: boolean;
  // The two corpus timing families; both on by default.
  readonly compile: boolean; // boot builds each corpus program
  readonly exec: boolean; // wasmtime runs each built program
  // The self-host chain leg: default OFF until T3.1 leg 4 (the self
  // chain) is green — the numbers only mean something once the chain
  // is stable (docs/ecosystem.md section 1, "Depends on").
  readonly chain: boolean;
  readonly iters: number; // compile-leg samples per program
  readonly execIters: number; // exec-leg samples per program
  readonly chainIters: number; // chain-leg samples
  readonly capMs: number; // per-invocation cap, corpus legs
  readonly chainCapMs: number; // per-invocation cap, chain legs
  readonly filter: string; // corpus-name substring filter; "" = all
}

export const DEFAULTS: BenchArgs = {
  help: false,
  verbose: false,
  compile: true,
  exec: true,
  chain: false,
  iters: 5,
  execIters: 5,
  chainIters: 1,
  capMs: 30_000,
  chainCapMs: 600_000,
  filter: "",
};

export interface ParsedArgs {
  readonly args: BenchArgs;
  // null = parsed clean; a string = the refusal reason (exit 2).
  readonly error: string | null;
}

export const HELP = `rho bench - compile-time and execution-time trend lines over the corpus
(docs/ecosystem.md section 1). A trend line, not a gate: same tree,
same machine, byte-stable report furniture; only the measured time
cells move, and nothing fails on them.

usage: node tools/bench/src/cli.ts [flags]

Runs the same from any cwd: every input path (corpus, compiler
sources, artifacts, the default boot binary) is resolved against the
repo root, so "npm run bench" from tools/bench/ and this invocation
from the root are the same run.

legs:
  --compile / --no-compile
      the compile-time family: boot builds each corpus program,
      median of --iters samples per program (default: on)
  --exec / --no-exec
      the execution-time family: wasmtime runs each built program,
      median of --exec-iters samples per program (default: on).
      Needs the artifacts of a compile leg: either run --compile in
      the same round (default) or have build/bench/<name>.wasm from a
      previous round of the same tree state. With the default
      --compile, a program whose build failed this round is a DNF row
      here ("stage starved by a dead upstream stage") - stale
      artifacts are never timed. A run is measured when it ends at
      the program's designed exit code (the "// exit:" header the
      corpus runner grades by); any other ending - an undesigned
      trap included - is an ERR row
  --chain
      the self-host chain: boot builds the compiler (mirror), the
      mirror runs (emitting the child WAT), wat2wasm assembles the
      child - median of --chain-iters. Default: OFF. Turn it on once
      T3.1 leg 4 (the self chain) is green; until then the numbers
      would measure a moving compiler

sampling and caps (the time-cap law: every invocation is hard-capped;
a cap hit is a DNF row and never drags down the round):
  --iters <n>          compile-leg samples per program    (default: 5)
  --exec-iters <n>     exec-leg samples per program       (default: 5)
  --chain-iters <n>    chain-leg samples                  (default: 1)
  --cap-ms <ms>        per-invocation cap, corpus legs    (default: 30000)
  --chain-cap-ms <ms>  per-invocation cap, chain legs     (default: 600000)
  --filter <substr>    only corpus names containing substr (default: all;
                       a filter that matches nothing is a refusal)
  --verbose            stream each invocation's raw output to stderr.
                       stderr is NOT part of the deterministic report;
                       stdout (the report) stays byte-stable either way
  --help               this text

environment:
  RHO=<path>           the boot binary (default ./build/rho, resolved
                       against the repo root - a relative RHO= value
                       resolves the same way, so the harness runs
                       from any cwd). The harness never builds boot -
                       run "make all" first

tools, verified at startup (mismatch or missing tool = refusal, exit 2):
  wasmtime ${PINNED_WASMTIME} and wat2wasm (wabt) ${PINNED_WABT} - the pins of
  .github/workflows/ci.yml

determinism (docs/ecosystem.md section 1 + design section 5):
  the report on stdout is name-sorted, fixed-width, pure ASCII, and
  carries no timestamps, no hostnames, and no absolute paths. Two runs
  on one tree and machine are byte-identical except the measured time
  cells (acceptance command: tools/bench/README.md)`;

export function parseArgs(argv: readonly string[]): ParsedArgs {
  // The working copy is mutable while parsing; the returned view is
  // the readonly BenchArgs (defaults never mutate — the error path
  // returns DEFAULTS untouched).
  const args: { -readonly [K in keyof BenchArgs]: BenchArgs[K] } = {
    ...DEFAULTS,
  };
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i];
    if (flag === undefined) break;
    // Consumes the flag's value; null = the value is missing.
    const needValue = (): string | null => {
      const v = argv[i + 1];
      if (v === undefined) return null;
      i += 1;
      return v;
    };
    const missing = (): string => `${flag}: missing value (see --help)`;
    const needInt = (min: number): number | string => {
      const raw = needValue();
      if (raw === null) return missing();
      if (!/^\d+$/.test(raw)) {
        return `${flag}: want a non-negative integer, got '${raw}' (see --help)`;
      }
      const n = Number(raw);
      if (!Number.isSafeInteger(n) || n < min) {
        return `${flag}: want an integer >= ${min}, got ${raw} (see --help)`;
      }
      return n;
    };
    switch (flag) {
      case "--help":
        args.help = true;
        break;
      case "--verbose":
        args.verbose = true;
        break;
      case "--compile":
        args.compile = true;
        break;
      case "--no-compile":
        args.compile = false;
        break;
      case "--exec":
        args.exec = true;
        break;
      case "--no-exec":
        args.exec = false;
        break;
      case "--chain":
        args.chain = true;
        break;
      case "--iters": {
        const r = needInt(1);
        if (typeof r === "string") return { args: DEFAULTS, error: r };
        args.iters = r;
        break;
      }
      case "--exec-iters": {
        const r = needInt(1);
        if (typeof r === "string") return { args: DEFAULTS, error: r };
        args.execIters = r;
        break;
      }
      case "--chain-iters": {
        const r = needInt(1);
        if (typeof r === "string") return { args: DEFAULTS, error: r };
        args.chainIters = r;
        break;
      }
      case "--cap-ms": {
        const r = needInt(1);
        if (typeof r === "string") return { args: DEFAULTS, error: r };
        args.capMs = r;
        break;
      }
      case "--chain-cap-ms": {
        const r = needInt(1);
        if (typeof r === "string") return { args: DEFAULTS, error: r };
        args.chainCapMs = r;
        break;
      }
      case "--filter": {
        const v = needValue();
        if (v === null) return { args: DEFAULTS, error: missing() };
        args.filter = v;
        break;
      }
      default:
        return { args: DEFAULTS, error: `unknown flag '${flag}' (see --help)` };
    }
  }
  return { args, error: null };
}
