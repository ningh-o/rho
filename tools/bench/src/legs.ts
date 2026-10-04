// legs.ts — the timing families of docs/ecosystem.md section 1:
//
//   compile time — boot builds every corpus program (the `rho build`
//   invocation includes the wat2wasm assembly, so the number is the
//   full compile the way the repo runs it);
//
//   generated-code execution time — wasmtime runs every built program
//   a fixed number of rounds; the median is printed per program. A run
//   is a measurement when it ends at the program's designed exit code
//   — the `// exit:` header the corpus runner grades by
//   (tests/run-corpus-repo.sh): 0 for most programs, 42 for 002_arith,
//   the designed 101 for the six panic programs. Any other ending is
//   an ERR row — an undesigned 101 is a trap, and any code the text
//   does not promise is a run that did not finish the way it is
//   written, so it measures nothing. A program whose build did not
//   deliver in the same round is a starved DNF row — a stale artifact
//   from an earlier tree state is never timed;
//
//   the self-host chain — mirror build -> mirror run (the mirror
//   emits the child WAT) -> child assemble. Default OFF (--chain):
//   the chain is still moving (T3.1 leg 4 open), and numbers from a
//   moving compiler are not a trend line. The leg TIMES the chain;
//   grading it (byte-identity, behavior) stays gate.sh's job — bench
//   is a trend line, not a gate.
//
// Every invocation rides the injected Runner (spawn + hard cap). A cap
// hit is a DNF row and never drags down the round: the loop moves to
// the next program. A row reads "ok" only when the stage/program
// delivered ALL of its fixed samples clean — the median is then over
// exactly the fixed sample count, never a silent re-count.
//
// Every repo-relative input path is resolved against LegParams.root
// (the repo root the CLI computes from its own location), so the
// harness is equivalent from any cwd — `npm run bench` from
// tools/bench/ spawns exactly what the root invocation spawns.

import { readdirSync, readFileSync, statSync } from "node:fs";
import { isAbsolute, join } from "node:path";

import { median } from "./median.ts";
import { byName } from "./corpus.ts";
import type { CorpusProgram } from "./corpus.ts";
import type { Runner, RunOptions } from "./runner.ts";
import type { LegReport, Row, RowStatus } from "./report.ts";

export interface LegParams {
  readonly runner: Runner;
  // The repo root (cli.ts derives it from this file's location).
  // Repo-relative INPUT paths — the corpus programs, the compiler's
  // root — are resolved against it before spawning.
  readonly root: string;
  // The boot binary (default <root>/build/rho; RHO= overrides). Spawned,
  // never printed.
  readonly rho: string;
  // Tool names resolved on PATH — the pins were verified at startup.
  readonly wasmtime: string;
  readonly wat2wasm: string;
  // build/bench — the artifacts of this round live here.
  readonly artifactsDir: string;
  readonly capMs: number;
  readonly chainCapMs: number;
}

// The cell text a DNF row prints when it never ran because an
// upstream stage died — the exact sentence tools/bench/README.md
// promises. A cap-hit DNF carries reason "" and prints "> cap ms".
export const STARVED_REASON = "stage starved by a dead upstream stage";

interface Sample {
  readonly status: RowStatus;
  readonly ms: number;
  readonly exitCode: number;
  // "" except on a starved DNF (see STARVED_REASON).
  readonly reason: string;
}

function classify(samples: readonly Sample[], fixedCount: number): Sample {
  // A stage/program row is ok only when it finished its full fixed
  // sample count clean. DNF outranks ERR (a hang is the stronger
  // signal); a shortfall against the fixed count (upstream stages
  // starved this one) is itself a DNF — the fixed count was not met.
  const dnf = samples.find((s) => s.status === "dnf");
  if (dnf !== undefined) return dnf;
  const err = samples.find((s) => s.status === "err");
  if (err !== undefined) return err;
  if (samples.length < fixedCount) {
    return { status: "dnf", ms: 0, exitCode: -1, reason: STARVED_REASON };
  }
  const first = samples[0];
  if (first === undefined) {
    return { status: "dnf", ms: 0, exitCode: -1, reason: STARVED_REASON };
  }
  return {
    status: "ok",
    ms: median(samples.map((s) => s.ms)),
    exitCode: first.exitCode,
    reason: "",
  };
}

function row(name: string, s: Sample, capMs: number): Row {
  return {
    name,
    status: s.status,
    ms: s.ms,
    capMs,
    exitCode: s.exitCode,
    reason: s.reason,
  };
}

// The one sampler, shared by both corpus legs and the chain stages:
// a run whose exit code misses `expectedExit` — and a spawn failure —
// is ERR; a cap hit is DNF and stops the sampling — the remaining
// iterations would measure nothing but the same hang. `expectedExit`
// is 0 for every stage that must simply succeed (boot's build,
// wasmtime's artifact load, the chain stages) and the program's
// designed `// exit:` code for the exec leg's generated-code runs —
// exactly the grading the corpus runner applies to the same program.
async function sampleClean(
  runner: Runner,
  argv: readonly string[],
  capMs: number,
  iters: number,
  expectedExit = 0,
  opts?: RunOptions,
): Promise<Sample[]> {
  const samples: Sample[] = [];
  for (let i = 0; i < iters; i++) {
    const r = await runner.run(argv, capMs, opts);
    if (r.timedOut) {
      samples.push({ status: "dnf", ms: r.ms, exitCode: -1, reason: "" });
      break;
    }
    if (r.spawnFailed) {
      samples.push({ status: "err", ms: r.ms, exitCode: r.code, reason: "" });
      break;
    }
    if (r.code !== expectedExit) {
      samples.push({ status: "err", ms: r.ms, exitCode: r.code, reason: "" });
      break;
    }
    samples.push({ status: "ok", ms: r.ms, exitCode: expectedExit, reason: "" });
  }
  return samples;
}

// --- leg 1: compile time (boot build per corpus program) -------------

export async function compileLeg(
  programs: readonly CorpusProgram[],
  p: LegParams,
  iters: number,
): Promise<LegReport> {
  const rows: Row[] = [];
  for (const prog of programs) {
    const argv = [
      p.rho,
      "build",
      // The corpus path may be repo-relative or absolute (listCorpus
      // spells the directory it listed); boot fopens it as given, so
      // it is always spawned against the repo root, never the cwd.
      isAbsolute(prog.relPath) ? prog.relPath : join(p.root, prog.relPath),
      "-o",
      `${p.artifactsDir}/${prog.name}.wasm`,
      ...prog.sets.flatMap((s) => ["--set", s] as const),
    ];
    const samples = await sampleClean(p.runner, argv, p.capMs, iters);
    rows.push(row(prog.name, classify(samples, iters), p.capMs));
  }
  return {
    title: `compile (boot build, median of ${iters})`,
    rows,
    skipped: false,
    skipReason: "",
  };
}

// --- leg 2: execution time (wasmtime per built program) --------------

// `upstream` is the same round's compile leg. A program it did not
// deliver (build ERR, build DNF) gets a starved DNF row here and is
// never exec'd: with no fresh artifact there is nothing to measure,
// and exec'ing anyway would time build/bench/<name>.wasm's stale
// bytes from an earlier tree state with no provenance. Pass nothing
// (--no-compile reuse path) and every program is exec'd — the CLI has
// then verified mere existence at startup; freshness is the
// operator's trend hygiene.
export async function execLeg(
  programs: readonly CorpusProgram[],
  p: LegParams,
  iters: number,
  upstream?: readonly Row[],
): Promise<LegReport> {
  const delivered = new Map<string, RowStatus>(
    (upstream ?? []).map((r) => [r.name, r.status]),
  );
  const rows: Row[] = [];
  for (const prog of programs) {
    if (upstream !== undefined && delivered.get(prog.name) !== "ok") {
      rows.push({
        name: prog.name,
        status: "dnf",
        ms: 0,
        capMs: p.capMs,
        exitCode: -1,
        reason: STARVED_REASON,
      });
      continue;
    }
    const argv = [p.wasmtime, "run", `${p.artifactsDir}/${prog.name}.wasm`];
    // Graded by the program's designed exit code — the same `// exit:`
    // header the corpus runner scores the program by; a run that lands
    // on it (0, 42, the designed 101 of a panic program) is the
    // measurement, anything else is ERR.
    const samples = await sampleClean(
      p.runner,
      argv,
      p.capMs,
      iters,
      prog.exitCode,
    );
    rows.push(row(prog.name, classify(samples, iters), p.capMs));
  }
  return {
    title: `exec (wasmtime run, median of ${iters})`,
    rows,
    skipped: false,
    skipReason: "",
  };
}

// True when the exec leg's artifact for this program is present (the
// --no-compile --exec reuse path).
export function artifactPresent(artifactsDir: string, name: string): boolean {
  try {
    return statSync(join(artifactsDir, `${name}.wasm`)).isFile();
  } catch {
    return false;
  }
}

// --- leg 3: the self-host chain (default off) ------------------------

export interface ChainInput {
  // "libs/compiler/main.rho" — the compiler's root.
  readonly rootRel: string;
  // SRC build parameter: the root source, exactly as gate.sh leg 4
  // feeds it ($(cat ...) — trailing newlines stripped).
  readonly rootSrc: string;
  // MODS build parameter: "@MOD@ <basename>\n<content>\n" per module
  // file, name-sorted, exactly as gate.sh assembles it.
  readonly mods: string;
}

// Reads the self-hosted compiler's sources from libs/compiler and
// assembles the chain build's input, mirroring tools/gate.sh leg 4
// (the bench must measure the same build the gate grades).
export function assembleChainInput(compilerDir: string): ChainInput | string {
  const rootRel = "libs/compiler/main.rho";
  let rootSrc: string;
  try {
    rootSrc = readFileSync(join(compilerDir, "main.rho"), "utf8");
  } catch (e) {
    // EACCES/EPERM on the read (a directory the runner cannot enter)
    // is a different environment edge from a missing tree — both are
    // refusal strings, but they must not share a sentence.
    const code = (e as NodeJS.ErrnoException | null)?.code;
    if (code === "EACCES" || code === "EPERM") {
      return `${rootRel}: the compiler directory is not readable as a directory`;
    }
    return `${rootRel} not found — the chain leg needs the self-hosted compiler`;
  }
  let entries: string[];
  try {
    entries = readdirSync(compilerDir);
  } catch {
    // The directory gone or unreadable between the main.rho read and
    // the listing (EACCES, ENOENT) — a refusal string for the CLI's
    // exit-2 path, never an unhandled throw.
    return `${rootRel}: the compiler directory is not readable — ` +
      `the chain leg needs the self-hosted compiler`;
  }
  const files = entries
    .filter((f) => f.endsWith(".rho") && f !== "main.rho")
    .sort(byName);
  const pieces: string[] = [];
  for (const f of files) {
    let text: string;
    try {
      text = readFileSync(join(compilerDir, f), "utf8");
    } catch {
      // A *.rho directory (EISDIR), a permission hole, a file gone
      // between readdir and read — a refusal string for the CLI's
      // exit-2 path, never an unhandled throw.
      return `${rootRel}: module ${f} is not readable as a file`;
    }
    pieces.push(`@MOD@ ${f}\n${stripTrailingNewlines(text)}\n`);
  }
  return {
    rootRel,
    rootSrc: stripTrailingNewlines(rootSrc),
    mods: pieces.join(""),
  };
}

// $(cat file) semantics: every trailing newline is stripped.
function stripTrailingNewlines(text: string): string {
  return text.replace(/\n+$/, "");
}

const CHAIN_STAGES = [
  "chain/mirror-build",
  "chain/mirror-run",
  "chain/child-assemble",
] as const;

export async function chainLeg(
  input: ChainInput,
  p: LegParams,
  iters: number,
): Promise<LegReport> {
  const mirrorWasm = `${p.artifactsDir}/chain-mirror.wasm`;
  const childWat = `${p.artifactsDir}/chain-child.wat`;
  const childWasm = `${p.artifactsDir}/chain-child.wasm`;

  // Per stage: only iterations whose turn actually ran the stage
  // record a sample; an upstream failure starves the stages after it
  // (no fabricated samples). classify() then rules a starved or
  // short-changed stage DNF — its fixed count was not met.
  const logs: Map<string, Sample[]> = new Map(
    CHAIN_STAGES.map((n) => [n, [] as Sample[]] as const),
  );
  for (let i = 0; i < iters; i++) {
    const buildLog = logs.get("chain/mirror-build");
    if (buildLog === undefined) continue;
    const build = await sampleClean(
      p.runner,
      [
        p.rho,
        "build",
        // Repo-relative, like the corpus paths — spawned against the
        // repo root, never the cwd.
        join(p.root, input.rootRel),
        "-o",
        mirrorWasm,
        "--set",
        `SRC=${input.rootSrc}`,
        "--set",
        `MODS=${input.mods}`,
      ],
      p.chainCapMs,
      1,
    );
    buildLog.push(...build);
    if (classify(build, 1).status !== "ok") continue;

    const runLog = logs.get("chain/mirror-run");
    if (runLog === undefined) continue;
    const run = await sampleClean(
      p.runner,
      [p.wasmtime, "run", mirrorWasm],
      p.chainCapMs,
      1,
      0,
      { stdoutFile: childWat },
    );
    runLog.push(...run);
    if (classify(run, 1).status !== "ok") continue;

    const asmLog = logs.get("chain/child-assemble");
    if (asmLog === undefined) continue;
    const asm = await sampleClean(
      p.runner,
      [p.wat2wasm, childWat, "-o", childWasm],
      p.chainCapMs,
      1,
    );
    asmLog.push(...asm);
  }

  const rows: Row[] = CHAIN_STAGES.map((n) => {
    const samples = logs.get(n) ?? [];
    return row(n, classify(samples, iters), p.chainCapMs);
  });
  return {
    title: `chain (mirror build -> mirror run -> child assemble, median of ${iters})`,
    rows,
    skipped: false,
    skipReason: "",
  };
}
