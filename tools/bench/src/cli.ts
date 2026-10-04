// cli.ts — the bench entry point.
//
// Refusal paths (usage error, pin mismatch, missing boot binary,
// missing artifacts, an uncreatable build/, empty filter match) print
// one line to stderr and exit 2 — the repo's usage-exit convention
// (boot/rho.h EXIT_USAGE). A completed round exits 0 no matter what
// its rows say: numbers trend, nothing fails on them
// (docs/ecosystem.md section 1).
//
// The report goes to stdout and is the only thing that does — even
// --verbose streams to stderr — so `cli.ts > report.txt` captures a
// byte-diffable artifact under every flag combination.

import { spawn } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import { accessSync, constants as fsConstants } from "node:fs";
import { dirname, isAbsolute, join } from "node:path";
import { fileURLToPath } from "node:url";

import { DEFAULTS, HELP, parseArgs } from "./args.ts";
import { listCorpus } from "./corpus.ts";
import {
  artifactPresent,
  assembleChainInput,
  chainLeg,
  compileLeg,
  execLeg,
  type LegParams,
} from "./legs.ts";
import { PINNED_WABT, PINNED_WASMTIME, verifyPins } from "./pins.ts";
import {
  renderReport,
  type BenchReport,
  type LegReport,
  type Row,
} from "./report.ts";
import { spawnRunner } from "./runner.ts";

function refuse(reason: string): never {
  process.stderr.write(`bench: refused: ${reason}\n`);
  process.exit(2);
}

async function main(): Promise<void> {
  const parsed = parseArgs(process.argv.slice(2));
  if (parsed.error !== null) {
    process.stderr.write(`bench: ${parsed.error}\n`);
    process.exit(2);
  }
  const args = parsed.args;
  if (args.help) {
    process.stdout.write(HELP + "\n");
    return;
  }

  // Repo root = four levels up from this FILE (…/rho/tools/bench/src/
  // /cli.ts — the dirname chain strips the filename first, so four
  // steps land at the rho root; three once escaped it to tools/).
  // Every path the harness touches is derived from here — the corpus
  // and compiler inputs, the artifacts, the default boot binary — so
  // the run is identical from any cwd (npm run bench from tools/bench/
  // spawns exactly what the root invocation spawns).
  const root = dirname(dirname(dirname(dirname(fileURLToPath(import.meta.url)))));
  // A relative RHO= resolves against the repo root too, so `RHO=build/rho`
  // means the same thing from wherever the harness is invoked.
  const rhoEnv = process.env.RHO;
  const rho =
    rhoEnv === undefined || rhoEnv === ""
      ? join(root, "build", "rho")
      : isAbsolute(rhoEnv)
        ? rhoEnv
        : join(root, rhoEnv);
  // The corpus dissolved at the freeze (T6.4): the programs tier IS its
  // successor — same flat *.rho listing, same `// exit:` grading
  // headers, module trees as sibling directories boot resolves itself.
  const corpusDir = join(root, "tests", "suites", "programs");
  const compilerDir = join(root, "libs", "compiler");
  const artifactsDir = join(root, "build", "bench");

  if (!existsSync(rho)) {
    refuse(
      `boot binary not found at ${rho} — build it with "make all" or point RHO= at it`,
    );
  }
  try {
    accessSync(rho, fsConstants.X_OK);
  } catch {
    refuse(`boot binary at ${rho} is not executable`);
  }

  // The pin self-check runs before anything is measured.
  const verification = await verifyPins(spawnToolVersionProbe());
  if (!verification.ok) refuse(verification.reason);

  const listed = listCorpus(corpusDir, args.filter);
  if (typeof listed === "string") refuse(listed);

  if (args.exec && !args.compile) {
    const missing = listed.filter(
      (p) => !artifactPresent(artifactsDir, p.name),
    );
    if (missing.length > 0) {
      refuse(
        `${missing.length} artifact(s) missing under build/bench/ ` +
          `(first: ${String(missing[0]?.name)}) — run with --compile, or ` +
          `produce the artifacts in a previous round of this tree state`,
      );
    }
  }

  try {
    mkdirSync(artifactsDir, { recursive: true });
  } catch {
    // An unwritable build/ (a permissions hole, a non-directory in the
    // way) is an environment refusal — exit 2 with a reason, never an
    // unhandled throw dying at exit 1 mid-round.
    refuse(
      `cannot create artifacts directory ${artifactsDir} — ` +
        `check what stands at build/ and its permissions`,
    );
  }

  const params: LegParams = {
    runner: spawnRunner(args.verbose),
    root,
    rho,
    wasmtime: "wasmtime",
    wat2wasm: "wat2wasm",
    artifactsDir,
    capMs: args.capMs,
    chainCapMs: args.chainCapMs,
  };

  // The exec leg rides the same round's compile rows: a program whose
  // build did not deliver is a starved DNF there — never timed against
  // stale build/bench/ bytes. With --no-compile there is no upstream
  // and existence was verified above; freshness is the operator's.
  const legs: LegReport[] = [];
  let compileRows: readonly Row[] | undefined;
  if (args.compile) {
    const leg = await compileLeg(listed, params, args.iters);
    legs.push(leg);
    compileRows = leg.rows;
  }
  if (args.exec) {
    legs.push(await execLeg(listed, params, args.execIters, compileRows));
  }
  if (args.chain) {
    const input = assembleChainInput(compilerDir);
    if (typeof input === "string") refuse(input);
    legs.push(await chainLeg(input, params, args.chainIters));
  } else {
    legs.push({
      title: `chain (mirror build -> mirror run -> child assemble, median of ${args.chainIters})`,
      rows: [],
      skipped: true,
      skipReason: "pass --chain (off until T3.1 leg 4, the self chain, is green)",
    });
  }

  const report: BenchReport = {
    banner:
      `bench: wasmtime ${PINNED_WASMTIME}, wat2wasm ${PINNED_WABT}, ` +
      `iters=${args.iters}, exec-iters=${args.execIters}, ` +
      `cap=${args.capMs}ms, chain-cap=${args.chainCapMs}ms`,
    legs,
  };
  process.stdout.write(renderReport(report) + "\n");
}

// Captures a tool's --version stdout for the pin self-check. Separate
// from the measuring runner: this one must return the output instead
// of discarding it (nothing here lands in the report either way).
function spawnToolVersionProbe() {
  return async (argv: readonly string[]): Promise<string | null> => {
    return new Promise<string | null>((resolve) => {
      const c = spawn(argv[0] ?? "", argv.slice(1), {
        stdio: ["ignore", "pipe", "ignore"],
      });
      let out = "";
      let failed = false;
      const timer = setTimeout(() => {
        c.kill("SIGKILL");
        failed = true;
      }, 10_000);
      c.stdout?.on("data", (chunk: Buffer) => {
        out += chunk.toString("utf8");
      });
      c.on("error", () => {
        clearTimeout(timer);
        resolve(null);
      });
      c.on("close", (code) => {
        clearTimeout(timer);
        if (failed || code !== 0) {
          resolve(null);
          return;
        }
        resolve(out);
      });
    });
  };
}

await main();
