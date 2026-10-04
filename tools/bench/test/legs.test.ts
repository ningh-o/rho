// Pins: the DNF law — docs/ecosystem.md section 1 "Every leg
// time-capped" + the ask's "a single-program timeout records DNF and
// does not drag down the round" — and the legs' classification
// contract: ok only over the full fixed sample count; a run is ERR
// when its exit misses the stage's expectation — 0 for boot build,
// artifact load, and the chain stages, the program's designed
// `// exit:` code (the corpus runner's own grading) for exec runs, so
// an undesigned trap is ERR while a designed nonzero exit (002_arith's
// 42, a panic program's 101) is a measurement; a program the compile
// leg did not deliver is a starved DNF, never timed against stale
// artifacts; every repo-relative input path is spawned against the
// repo root; and the chain leg is staged (a dead upstream stage
// starves the stages after it — no fabricated samples, and the
// starvation cell is the sentence README.md promises, never a cap the
// row did not hit).

import test from "node:test";
import assert from "node:assert/strict";
import {
  chmodSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import {
  STARVED_REASON,
  assembleChainInput,
  chainLeg,
  compileLeg,
  execLeg,
  type ChainInput,
  type LegParams,
} from "../src/legs.ts";
import type { CorpusProgram } from "../src/corpus.ts";
import type { RunResult, Runner, RunOptions } from "../src/runner.ts";
import type { Row } from "../src/report.ts";

function res(partial: Partial<RunResult>): RunResult {
  return {
    timedOut: false,
    code: 0,
    spawnFailed: false,
    ms: 1,
    ...partial,
  };
}

// A scripted runner: one RunResult per call, in order; any call past
// the script throws, so a leg that "silently skips" a program (or
// execs one it should have starved) fails the test instead of passing
// dishonestly.
function scriptedRunner(script: RunResult[]): Runner {
  let calls = 0;
  return {
    async run(
      _argv: readonly string[],
      _capMs: number,
      _opts?: RunOptions,
    ): Promise<RunResult> {
      const r = script[calls];
      if (r === undefined) {
        throw new Error(`unexpected invocation #${calls + 1}`);
      }
      calls += 1;
      return r;
    },
  };
}

// The fixtures mirror the real corpus headers (001_hello -> exit 0,
// 002_arith -> exit 42, 015_panic_oob -> exit 101) so the pins read
// against the same grading the corpus runner applies.
const progs: CorpusProgram[] = [
  { name: "001_hello", relPath: "corpus/001_hello.rho", sets: [], exitCode: 0 },
  { name: "002_arith", relPath: "corpus/002_arith.rho", sets: [], exitCode: 42 },
];

function params(runner: Runner): LegParams {
  return {
    runner,
    root: "/repo",
    rho: "./build/rho",
    wasmtime: "wasmtime",
    wat2wasm: "wat2wasm",
    artifactsDir: "build/bench",
    capMs: 30000,
    chainCapMs: 600000,
  };
}

function rowOf(name: string, status: Row["status"], exitCode: number): Row {
  return { name, status, ms: 0, capMs: 30000, exitCode, reason: "" };
}

test("compile leg: median over the fixed sample count, per program", async () => {
  const runner = scriptedRunner([
    res({ ms: 10 }),
    res({ ms: 12 }),
    // second program, two samples
    res({ ms: 20 }),
    res({ ms: 30 }),
  ]);
  const leg = await compileLeg(progs, params(runner), 2);
  assert.equal(leg.rows.length, 2);
  assert.equal(leg.rows[0]?.status, "ok");
  assert.equal(leg.rows[0]?.ms, 11);
  assert.equal(leg.rows[1]?.status, "ok");
  assert.equal(leg.rows[1]?.ms, 25);
  assert.equal(leg.title, "compile (boot build, median of 2)");
});

test("compile leg: a cap hit is DNF and the round moves on", async () => {
  const runner = scriptedRunner([
    res({ ms: 10 }),
    res({ timedOut: true, code: -1, ms: 30000 }), // 001_hello: DNF on sample 2
    // 002_arith must still be sampled — a DNF never drags down the round
    res({ ms: 20 }),
    res({ ms: 21 }),
  ]);
  const leg = await compileLeg(progs, params(runner), 2);
  assert.equal(leg.rows[0]?.status, "dnf");
  assert.equal(leg.rows[0]?.capMs, 30000);
  // reason "" — a cap cell ("> cap ms"), not a starvation sentence
  assert.equal(leg.rows[0]?.reason, "");
  assert.equal(leg.rows[1]?.status, "ok");
  assert.equal(leg.rows[1]?.ms, 20.5);
});

test("compile leg: a compile error is ERR, named with the exit code", async () => {
  const runner = scriptedRunner([
    res({ code: 1, ms: 5 }),
    res({ ms: 20 }),
    res({ ms: 21 }),
  ]);
  const leg = await compileLeg(progs, params(runner), 2);
  assert.equal(leg.rows[0]?.status, "err");
  assert.equal(leg.rows[0]?.exitCode, 1);
  assert.equal(leg.rows[1]?.status, "ok");
});

test("compile leg: `// set:` headers ride the build as --set arguments", async () => {
  let seen: readonly string[] = [];
  const runner: Runner = {
    async run(argv, _capMs) {
      seen = argv;
      return res({ ms: 1 });
    },
  };
  const prog: CorpusProgram = {
    name: "t06_params_widen",
    relPath: "corpus/t06_params_widen.rho",
    sets: ["WIDTH=8", "NAME=wide"],
    exitCode: 0,
  };
  await compileLeg([prog], params(runner), 1);
  const i = seen.indexOf("--set");
  assert.notEqual(i, -1);
  assert.deepEqual(seen.slice(i, i + 4), [
    "--set",
    "WIDTH=8",
    "--set",
    "NAME=wide",
  ]);
});

test("compile leg: input and -o paths are spawned against the repo root, never the cwd", async () => {
  // The corpus path is repo-relative and boot fopens it as given, so
  // the leg must resolve it against LegParams.root — this is the
  // contract that makes `npm run bench` (cwd tools/bench/) identical
  // to a root invocation.
  let seen: readonly string[] = [];
  const runner: Runner = {
    async run(argv, _capMs) {
      seen = argv;
      return res({ ms: 1 });
    },
  };
  const p: LegParams = {
    ...params(runner),
    root: "/repo",
    artifactsDir: "/repo/build/bench",
  };
  await compileLeg([progs[0] as CorpusProgram], p, 1);
  assert.equal(seen[2], join("/repo", "corpus/001_hello.rho"));
  assert.equal(seen[4], "/repo/build/bench/001_hello.wasm");
});

test("exec leg: an exit that misses the design is ERR — a trap is not a finished run", async () => {
  // 001_hello designs exit 0; the run's 134 (a trap) measures nothing.
  const runner = scriptedRunner([
    res({ code: 134, ms: 3 }), // wasmtime maps a trap to a nonzero exit
    // 002_arith must still be sampled — an ERR never drags down the round
    res({ code: 42, ms: 6 }),
    res({ code: 42, ms: 7 }),
  ]);
  const leg = await execLeg(progs, params(runner), 2);
  assert.equal(leg.rows[0]?.status, "err");
  assert.equal(leg.rows[0]?.exitCode, 134);
  assert.equal(leg.rows[1]?.status, "ok");
  assert.equal(leg.rows[1]?.ms, 6.5);
  assert.equal(leg.title, "exec (wasmtime run, median of 2)");
});

test("exec leg: a run that lands on the designed `// exit:` code is a measurement", async () => {
  // 002_arith designs exit 42 (corpus/002_arith.rho:1) — the corpus
  // runner passes such a run, so bench measures it instead of calling
  // it an ERR.
  const runner = scriptedRunner([
    res({ code: 42, ms: 6 }),
    res({ code: 42, ms: 8 }),
  ]);
  const leg = await execLeg([progs[1] as CorpusProgram], params(runner), 2);
  assert.equal(leg.rows[0]?.status, "ok");
  assert.equal(leg.rows[0]?.ms, 7);
  // The row names the designed code, not a hardcoded 0.
  assert.equal(leg.rows[0]?.exitCode, 42);
});

test("exec leg: a designed nonzero code still rejects every other ending", async () => {
  // 002_arith (design 42) run against an undesigned 101 — a trap — is
  // ERR: only the designed code is a measurement.
  const runner = scriptedRunner([
    res({ code: 0, ms: 2 }), // 001_hello: designs 0, lands 0
    res({ code: 0, ms: 3 }),
    res({ code: 101, ms: 4 }), // 002_arith: designs 42, lands 101
  ]);
  const leg = await execLeg(progs, params(runner), 2);
  assert.equal(leg.rows[0]?.status, "ok");
  assert.equal(leg.rows[1]?.status, "err");
  assert.equal(leg.rows[1]?.exitCode, 101);
});

test("exec leg: the designed 101 of a panic program is a measurement", async () => {
  // 015_panic_oob designs exit 101 (corpus/015_panic_oob.rho:1) — the
  // corpus runner's own six trap programs; a run that lands there is
  // graded like any other designed exit.
  const panic: CorpusProgram = {
    name: "015_panic_oob",
    relPath: "corpus/015_panic_oob.rho",
    sets: [],
    exitCode: 101,
  };
  const runner = scriptedRunner([res({ code: 101, ms: 5 })]);
  const leg = await execLeg([panic], params(runner), 1);
  assert.equal(leg.rows[0]?.status, "ok");
  assert.equal(leg.rows[0]?.ms, 5);
  assert.equal(leg.rows[0]?.exitCode, 101);
});

test("exec leg: a cap hit is DNF; the next program still runs", async () => {
  const runner = scriptedRunner([
    res({ timedOut: true, code: -1, ms: 30000 }),
    res({ code: 42, ms: 6 }),
    res({ code: 42, ms: 7 }),
  ]);
  const leg = await execLeg(progs, params(runner), 2);
  assert.equal(leg.rows[0]?.status, "dnf");
  assert.equal(leg.rows[0]?.reason, ""); // a cap cell, not starvation
  assert.equal(leg.rows[1]?.status, "ok");
  assert.equal(leg.rows[1]?.ms, 6.5);
});

test("exec leg: the artifact path is artifactsDir/<name>.wasm", async () => {
  let seen: readonly string[] = [];
  const runner: Runner = {
    async run(argv, _capMs) {
      seen = argv;
      return res({ ms: 1 });
    },
  };
  await execLeg([progs[0] as CorpusProgram], params(runner), 1);
  assert.deepEqual(seen, ["wasmtime", "run", "build/bench/001_hello.wasm"]);
});

test("exec leg: a program the compile leg did not deliver is a starved DNF, never timed", async () => {
  // This round's compile rows: 001_hello ERR'd, 002_arith delivered.
  const upstream: Row[] = [
    rowOf("001_hello", "err", 1),
    rowOf("002_arith", "ok", 0),
  ];
  // The script serves ONLY 002_arith's two samples: any attempt to
  // exec the starved 001_hello (its stale artifact included) would
  // over-run the script and throw.
  const runner = scriptedRunner([res({ code: 42, ms: 6 }), res({ code: 42, ms: 8 })]);
  const leg = await execLeg(progs, params(runner), 2, upstream);
  assert.equal(leg.rows[0]?.status, "dnf");
  assert.equal(leg.rows[0]?.reason, STARVED_REASON);
  assert.equal(leg.rows[1]?.status, "ok");
  assert.equal(leg.rows[1]?.ms, 7);
});

test("exec leg: a cap-hit build starves its exec row too", async () => {
  const upstream: Row[] = [rowOf("001_hello", "dnf", -1)];
  const runner = scriptedRunner([]); // zero legal invocations
  const leg = await execLeg([progs[0] as CorpusProgram], params(runner), 2, upstream);
  assert.equal(leg.rows[0]?.status, "dnf");
  assert.equal(leg.rows[0]?.reason, STARVED_REASON);
});

test("exec leg: without a compile leg (artifact reuse) every program is sampled", async () => {
  const runner = scriptedRunner([res({ ms: 6 }), res({ code: 42, ms: 7 })]);
  const leg = await execLeg(progs, params(runner), 1);
  assert.deepEqual(
    leg.rows.map((r) => r.status),
    ["ok", "ok"],
  );
});

const chain: ChainInput = {
  rootRel: "libs/compiler/main.rho",
  rootSrc: "fn main() -> i32 { return 0; }",
  mods: "@MOD@ lex.rho\ncontent\n",
};

test("chain leg: three stages, all green, medians over the fixed count", async () => {
  const runner = scriptedRunner([
    res({ ms: 100 }),
    res({ ms: 140 }),
    res({ ms: 5 }),
    res({ ms: 110 }),
    res({ ms: 130 }),
    res({ ms: 6 }),
  ]);
  const leg = await chainLeg(chain, params(runner), 2);
  assert.equal(leg.rows.length, 3);
  assert.deepEqual(
    leg.rows.map((r) => r.name),
    ["chain/mirror-build", "chain/mirror-run", "chain/child-assemble"],
  );
  assert.deepEqual(
    leg.rows.map((r) => r.status),
    ["ok", "ok", "ok"],
  );
  assert.deepEqual(
    leg.rows.map((r) => r.ms),
    [105, 135, 5.5],
  );
  assert.deepEqual(
    leg.rows.map((r) => r.reason),
    ["", "", ""],
  );
});

test("chain leg: a dead upstream stage starves the stages after it", async () => {
  // Iteration 1: mirror build ERRs. Iteration 2 never happens (fixed
  // count 1). The build row is ERR; the two downstream stages ran zero
  // times — their rows are DNF (did not finish) carrying the starvation
  // sentence, never fabricated oks and never a cap they did not hit.
  const runner = scriptedRunner([res({ code: 1, ms: 100 })]);
  const leg = await chainLeg(chain, params(runner), 1);
  assert.equal(leg.rows[0]?.status, "err");
  assert.equal(leg.rows[0]?.reason, "");
  assert.equal(leg.rows[1]?.status, "dnf");
  assert.equal(leg.rows[1]?.reason, STARVED_REASON);
  assert.equal(leg.rows[2]?.status, "dnf");
  assert.equal(leg.rows[2]?.reason, STARVED_REASON);
});

test("chain leg: a cap hit starves the stages after it too", async () => {
  // The timed-out stage keeps the cap cell (reason ""); the stages
  // that never ran carry the starvation sentence.
  const runner = scriptedRunner([
    res({ timedOut: true, code: -1, ms: 600000 }),
  ]);
  const leg = await chainLeg(chain, params(runner), 1);
  assert.equal(leg.rows[0]?.status, "dnf");
  assert.equal(leg.rows[0]?.reason, "");
  assert.equal(leg.rows[1]?.status, "dnf");
  assert.equal(leg.rows[1]?.reason, STARVED_REASON);
  assert.equal(leg.rows[2]?.status, "dnf");
  assert.equal(leg.rows[2]?.reason, STARVED_REASON);
});

test("chain leg: stages carry the chain cap, not the corpus cap", async () => {
  const seenCaps: number[] = [];
  const runner: Runner = {
    async run(_argv, capMs) {
      seenCaps.push(capMs);
      return res({ ms: 1 });
    },
  };
  await chainLeg(chain, params(runner), 1);
  assert.deepEqual(seenCaps, [600000, 600000, 600000]);
});

test("chain leg: the mirror build's input is spawned against the repo root, never the cwd", async () => {
  // three stages run per iteration; the capture keeps every argv and
  // the assertions read the BUILD stage's (the later stages would
  // otherwise overwrite the capture)
  const seen: (readonly string[])[] = [];
  const runner: Runner = {
    async run(argv, _capMs) {
      seen.push(argv);
      return res({ ms: 1 });
    },
  };
  const p: LegParams = {
    ...params(runner),
    root: "/repo",
    artifactsDir: "/repo/build/bench",
  };
  await chainLeg(chain, p, 1);
  const buildArgv = seen[0] ?? [];
  assert.equal(buildArgv[2], join("/repo", "libs/compiler/main.rho"));
  assert.equal(buildArgv[4], "/repo/build/bench/chain-mirror.wasm");
});

test("chain input: a missing compiler directory is an honest refusal string", () => {
  const refused = assembleChainInput("/repo/libs/nowhere");
  assert.equal(typeof refused, "string");
  assert.match(String(refused), /libs\/compiler\/main\.rho not found/);
});

test("chain input: SRC/MODS assembly mirrors gate leg 4 (sorted, @MOD@, stripped)", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-chain-"));
  try {
    writeFileSync(join(dir, "main.rho"), "fn main() {}\n\n\n");
    writeFileSync(join(dir, "lex.rho"), "module lex\n");
    writeFileSync(join(dir, "emit.rho"), "module emit\n\n");
    const input = assembleChainInput(dir);
    if (typeof input === "string") assert.fail(input);
    assert.equal(input.rootRel, "libs/compiler/main.rho");
    // $(cat ...) semantics: trailing newlines stripped from the SRC.
    assert.equal(input.rootSrc, "fn main() {}");
    // Modules name-sorted (readdir order is OS-defined), one @MOD@
    // block each, trailing newlines stripped.
    assert.equal(
      input.mods,
      "@MOD@ emit.rho\nmodule emit\n@MOD@ lex.rho\nmodule lex\n",
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("chain input: a module that is not a readable file is a refusal string", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-chain-"));
  try {
    writeFileSync(join(dir, "main.rho"), "fn main() {}\n");
    mkdirSync(join(dir, "bad.rho")); // a directory where a module must be
    const refused = assembleChainInput(dir);
    assert.equal(typeof refused, "string");
    assert.match(String(refused), /module bad\.rho is not readable/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("chain input: a directory that cannot be listed is a refusal string, not a throw", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-chain-"));
  try {
    writeFileSync(join(dir, "main.rho"), "fn main() {}\n");
    chmodSync(dir, 0o000); // no read permission on the directory itself
    const input = assembleChainInput(dir);
    // The contract under fix: the environment edge reaches the CLI's
    // exit-2 refusal path, never an unhandled readdirSync throw. An
    // unprivileged run (the ordinary case) gets the refusal string;
    // root reads the directory anyway and gets the assembled input —
    // both outcomes are asserted honestly, per identity.
    if (typeof input === "string") {
      assert.match(input, /not readable as a directory/);
    } else {
      assert.equal(input.rootRel, "libs/compiler/main.rho");
    }
  } finally {
    chmodSync(dir, 0o700);
    rmSync(dir, { recursive: true, force: true });
  }
});
