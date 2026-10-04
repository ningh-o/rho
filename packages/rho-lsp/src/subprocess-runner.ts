// subprocess-runner.ts — the chosen Runner: the canonical two-step
// embedding face, byte-for-byte the shape tests/run-corpus-diff.sh
// (lines 56-61) and tests/run-selfhost.sh pin:
//
//   1. <rho> build libs/compiler/main.rho -o <tmp>/check.wasm
//        --set "SRC=<root text>" --set "MODS=<module blocks>"
//        [--set "FMT=1" for the fmt face]
//   2. <wasmtime> <tmp>/check.wasm      (bare — no runtime args; the
//      kernel imports only fd_write + proc_exit, so argv is unreadable)
//
// Faces, matching the corpus-diff labels exactly:
//   refused up front      -> the SRC/MODS --set values exceed the POSIX
//                            per-argv-element budget ("size"): a
//                            document property — the server's caller
//                            skips these without counting a failure;
//   step 1 fails          -> toolchain trouble ("build"): a RUNNER
//                            FAILURE, never document diagnostics —
//                            failure feeds the degradation counter;
//   step 2 rc 0           -> accepted (stdout WAT discarded; for fmt,
//                            stdout IS the canonical text);
//   step 2 rc 1           -> clean refusal: stderr carries the
//                            document's diagnostics;
//   step 2 rc > 1         -> the compiler itself died ("PANIC"): a
//                            RUNNER FAILURE — "worse than any
//                            behavioral diff" (corpus-diff.sh:69-72).
//
// Every spawn is time-capped: the child gets SIGTERM then SIGKILL at
// the deadline (this is what makes the cap honest — the server's outer
// race alone could not stop a wedge, only abandon it).
//
// Pins: test/runner.test.ts runs this class against fake toolchain
// scripts and pins the invocation, the faces, the cap, and cleanup.

import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, isAbsolute, join, relative } from "node:path";
import { parseCompilerDiags } from "./diag-parser.js";
import { MAX_ARG_BYTES, assembleMods, transportOverflow } from "./runner.js";
import type { CheckRequest, CheckResult, FormatResult, RawResult, Runner, RunnerInfo } from "./runner.js";

export interface SubprocessRunnerOptions {
  /** Absolute path to the boot binary (build/rho). */
  rhoBinary: string;
  /** Absolute path to the rho repo root (holds libs/compiler). */
  repoRoot: string;
  /** wasmtime executable; resolved from PATH when unset. */
  wasmtime?: string;
  /** Per-step cap in ms (each spawn gets its own). */
  deadlineMs?: number;
}

/** Default per-request cap. Generous by editor standards; the gates
 * allow minutes for the same builds, an editor must not. */
export const DEFAULT_RUNNER_CAP_MS = 5000;

interface SpawnOutcome {
  code: number | null;
  signal: NodeJS.Signals | null;
  stdout: string;
  stderr: string;
  timedOut: boolean;
}

function spawnCapped(cmd: string, args: string[], cwd: string, capMs: number): Promise<SpawnOutcome> {
  return new Promise((resolveSpawn) => {
    // detached on POSIX: the child leads its own process group, so the
    // cap can kill the whole tree — a wedged shell script must not
    // leave an orphan holding the stdio pipes open ('close' waits for
    // them, and a blocked 'close' is a cap that does not bite)
    const child = spawn(cmd, args, { cwd, windowsHide: true, detached: process.platform !== "win32" });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    let done = false;
    const killTree = (sig: NodeJS.Signals): void => {
      try {
        if (child.pid !== undefined && process.platform !== "win32") process.kill(-child.pid, sig);
        else child.kill(sig);
      } catch {
        child.kill(sig); // the group may already be gone
      }
    };
    const timer = setTimeout(() => {
      timedOut = true;
      killTree("SIGTERM");
      // A child that ignores SIGTERM dies at SIGKILL; either way the
      // outcome resolves below when the process exits.
      setTimeout(() => {
        if (!done) killTree("SIGKILL");
      }, 250).unref();
    }, capMs);
    timer.unref();
    child.stdout?.on("data", (d: Buffer) => {
      stdout += d.toString("utf8");
    });
    child.stderr?.on("data", (d: Buffer) => {
      stderr += d.toString("utf8");
    });
    const finish = (code: number | null, signal: NodeJS.Signals | null) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolveSpawn({ code, signal, stdout, stderr, timedOut });
    };
    child.on("close", finish);
    child.on("error", (err) => {
      // Spawn failure (binary missing): surface as code -1 with the
      // error on stderr — a runner failure, logged, never published.
      stderr += `rho-lsp: spawn ${cmd} failed: ${String(err)}\n`;
      finish(-1, null);
    });
  });
}

export class SubprocessRunner implements Runner {
  private readonly capMs: number;

  constructor(private readonly opts: SubprocessRunnerOptions) {
    this.capMs = opts.deadlineMs ?? DEFAULT_RUNNER_CAP_MS;
  }

  info(): RunnerInfo {
    return {
      kind: "subprocess",
      detail: `rho build (${this.opts.repoRoot}) -> wasmtime`,
    };
  }

  async dispose(): Promise<void> {
    /* nothing to reap: every child is bounded by its own cap */
  }

  /**
   * Same-directory sibling modules, name-sorted, paths relative to the
   * root document's directory. Subpackages and facades are NOT walked
   * (v1 scope, documented): the mirror loads what MODS carries.
   */
  static modulesFor(rootPath: string | undefined, siblingTexts: Map<string, string>): { path: string; text: string }[] {
    if (!rootPath || !isAbsolute(rootPath)) return [];
    const dir = dirname(rootPath);
    const out: { path: string; text: string }[] = [];
    for (const [absPath, text] of siblingTexts) {
      if (absPath === rootPath) continue;
      if (dirname(absPath) !== dir) continue;
      if (!absPath.endsWith(".rho")) continue;
      const rel = relative(dir, absPath);
      out.push({ path: rel, text });
    }
    out.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
    return out;
  }

  private async runFace(req: CheckRequest, fmt: boolean, deadlineMs: number): Promise<RawResult> {
    const started = Date.now();
    const tmp = await mkdtemp(join(tmpdir(), "rho-lsp-"));
    try {
      const compilerRoot = join(this.opts.repoRoot, "libs", "compiler", "main.rho");
      const outWasm = join(tmp, "face.wasm");
      const mods = assembleMods(req.modules);
      const sets = [`SRC=${req.rootText}`, `MODS=${mods}`];
      if (fmt) sets.push("FMT=1");
      // defense in depth: the server skips over-budget requests before
      // it ever calls the runner; a direct caller that does not gets an
      // honest "size" face here instead of a Linux exec dying E2BIG
      // inside the child spawn
      const overflow = transportOverflow(req.rootText, mods);
      if (overflow !== undefined) {
        const err = new Error(
          `--set ${overflow} exceeds the ${MAX_ARG_BYTES}-byte argv budget (POSIX MAX_ARG_STRLEN); the face refuses rather than exec into E2BIG`,
        );
        (err as RunnerFaceError).face = "size";
        throw err;
      }
      const args = ["build", compilerRoot, "-o", outWasm];
      for (const s of sets) args.push("--set", s);

      // step 1: boot bakes the document into a fresh compiler wasm
      const build = await spawnCapped(this.opts.rhoBinary, args, this.opts.repoRoot, Math.min(deadlineMs, this.capMs));
      if (build.timedOut) return this.timed(started);
      if (build.code !== 0) {
        // "build" face: the toolchain could not produce the checker —
        // a runner failure. Boot's own noise goes on the error for the
        // server's log; the server counts a failure, never document
        // diagnostics.
        const err = new Error(`toolchain build failed (rc=${build.code})\n${build.stderr}`);
        (err as RunnerFaceError).face = "build";
        throw err;
      }

      // step 2: wasmtime runs the fresh wasm bare
      const run = await spawnCapped(this.opts.wasmtime ?? "wasmtime", [outWasm], tmp, Math.min(deadlineMs, this.capMs));
      if (run.timedOut) return this.timed(started);
      if (run.code !== null && run.code > 1) {
        // "PANIC" face (corpus-diff.sh:69-72): the compiler itself died
        // mid-run — a runner failure, never a document diagnostic.
        const err = new Error(`compiler died mid-run (rc=${run.code})\n${run.stderr}`);
        (err as RunnerFaceError).face = "panic";
        throw err;
      }
      return {
        ok: run.code === 0,
        exitCode: run.code,
        stderr: run.stderr,
        stdout: run.stdout,
        timedOut: false,
        durationMs: Date.now() - started,
      };
    } finally {
      // the per-request temp tree (face.wasm + face.wasm.wat beside it)
      await rm(tmp, { recursive: true, force: true }).catch(() => {});
    }
  }

  private timed(started: number): RawResult {
    return {
      ok: false,
      exitCode: null,
      stderr: "",
      stdout: "",
      timedOut: true,
      durationMs: Date.now() - started,
    };
  }

  async check(req: CheckRequest, deadlineMs: number): Promise<CheckResult> {
    const raw = await this.runFace(req, false, deadlineMs);
    return { ...raw, parsed: parseCompilerDiags(raw.stderr) };
  }

  async format(req: CheckRequest, deadlineMs: number): Promise<FormatResult> {
    const raw = await this.runFace(req, true, deadlineMs);
    const result: FormatResult = { ...raw };
    if (raw.ok) result.text = raw.stdout;
    return result;
  }
}

/** A runner failure carries which face failed ("build" | "panic" |
 * "size"). */
export interface RunnerFaceError extends Error {
  face?: string;
}
