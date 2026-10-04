// runner.ts — the time-cap law in its Node form.
//
// Law: TODO.md working protocol — "Every run is time-capped (perl
// alarm / poll pattern; never an unbounded wait)". Every invocation
// this harness spawns is hard-capped: at the cap the child is killed
// (SIGKILL) and the invocation resolves as timedOut. Nothing here can
// wait forever. docs/ecosystem.md section 1 — "Every leg time-capped"
// — is met by construction: a leg is a fixed number of capped
// invocations, so each leg's total time is bounded without a second,
// output-truncating budget (a leg-level cut-off would land at a
// run-dependent row and break the byte-stability law).
//
// The clock is process.hrtime.bigint() — monotonic. No wall-clock
// time is ever read: the report carries elapsed milliseconds of
// measured cells, never a timestamp.
//
// Child output is captured and discarded (or streamed to stderr under
// --verbose, which is documented as not part of the deterministic
// report): no tool output can leak machine-local text — paths, host
// names — into stdout.

import { spawn } from "node:child_process";
import { openSync, closeSync } from "node:fs";

export interface RunResult {
  // The cap was hit and the child was killed.
  readonly timedOut: boolean;
  // Exit code; -1 when killed.
  readonly code: number;
  // The binary could not be spawned at all (missing / not executable)
  // — classified as ERR by the legs, never as a measured sample (the
  // CLI's startup checks make this near-unreachable).
  readonly spawnFailed: boolean;
  // Elapsed milliseconds on the monotonic clock.
  readonly ms: number;
}

export interface RunOptions {
  // When set, the child's stdout goes to this file at the fd level
  // (the chain leg captures the mirror's emitted WAT this way; the
  // child is tens of MB — piping it through the event loop would slow
  // the very thing being measured). Never printed; the report carries
  // no tool output.
  readonly stdoutFile?: string;
}

export interface Runner {
  run(
    argv: readonly string[],
    capMs: number,
    opts?: RunOptions,
  ): Promise<RunResult>;
}

export function spawnRunner(verbose: boolean): Runner {
  return {
    run(argv, capMs, opts) {
      return new Promise((resolve) => {
        const stdoutFd =
          opts?.stdoutFile !== undefined
            ? openSync(opts.stdoutFile, "w")
            : undefined;
        const child = spawn(argv[0] ?? "", argv.slice(1), {
          stdio: ["ignore", stdoutFd ?? "pipe", "pipe"],
        });
        const start = process.hrtime.bigint();
        let done = false;
        const finish = (r: RunResult) => {
          if (done) return;
          done = true;
          clearTimeout(timer);
          if (stdoutFd !== undefined) closeSync(stdoutFd);
          resolve(r);
        };
        const timer = setTimeout(() => {
          child.kill("SIGKILL");
          finish({
            timedOut: true,
            code: -1,
            spawnFailed: false,
            ms: Number(process.hrtime.bigint() - start) / 1e6,
          });
        }, capMs);
        child.stdout?.on("data", (chunk: Buffer) => {
          if (verbose) process.stderr.write(chunk);
        });
        child.stderr?.on("data", (chunk: Buffer) => {
          if (verbose) process.stderr.write(chunk);
        });
        child.on("error", () => {
          // Spawn failure (binary missing / not executable).
          finish({ timedOut: false, code: 127, spawnFailed: true, ms: 0 });
        });
        child.on("close", (code) => {
          finish({
            timedOut: false,
            code: code ?? -1,
            spawnFailed: false,
            ms: Number(process.hrtime.bigint() - start) / 1e6,
          });
        });
      });
    },
  };
}
