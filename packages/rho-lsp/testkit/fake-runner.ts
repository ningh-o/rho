// testkit/fake-runner.ts — scripted Runner doubles for the server
// tests. Each scripted behavior is popped per call: a canned result, a
// thrown failure, or a promise that NEVER settles (the DNF case). All
// calls are recorded so tests can assert what the server asked for.

import { parseCompilerDiags } from "../src/diag-parser.js";
import { RUNNER_TIMEOUT, raceDeadline } from "../src/runner.js";
import type { CheckRequest, CheckResult, FormatResult, Runner, RunnerInfo } from "../src/runner.js";

export type Behavior =
  | { kind: "ok"; stderr?: string; stdout?: string; durationMs?: number }
  | { kind: "okFmt"; text: string; durationMs?: number }
  | { kind: "refused"; stderr: string; durationMs?: number }
  | { kind: "timeout"; durationMs?: number }
  | { kind: "throw"; message: string };

export interface RecordedCall {
  req: CheckRequest;
  deadlineMs: number;
}

export class FakeRunner implements Runner {
  readonly checkCalls: RecordedCall[] = [];
  readonly formatCalls: RecordedCall[] = [];
  private queue: Behavior[] = [];

  /** Script behaviors, consumed FIFO; the last one repeats forever. */
  script(...behaviors: Behavior[]): void {
    this.queue = [...behaviors];
  }

  info(): RunnerInfo {
    return { kind: "fake", detail: "scripted double" };
  }

  async dispose(): Promise<void> {
    /* nothing */
  }

  private next(): Behavior {
    if (this.queue.length === 0) throw new Error("FakeRunner: no behavior scripted");
    if (this.queue.length === 1) return this.queue[0] as Behavior;
    return this.queue.shift() as Behavior;
  }

  async check(req: CheckRequest, deadlineMs: number): Promise<CheckResult> {
    this.checkCalls.push({ req, deadlineMs });
    const b = this.next();
    const durationMs = "durationMs" in b && b.durationMs !== undefined ? b.durationMs : 1;
    if (b.kind === "throw") throw new Error(b.message);
    if (b.kind === "timeout") {
      await never();
      throw new Error("unreachable"); // never settles
    }
    if (b.kind === "okFmt") {
      return {
        ok: true,
        exitCode: 0,
        stderr: "",
        stdout: b.text,
        timedOut: false,
        durationMs,
        parsed: parseCompilerDiags(""),
      };
    }
    if (b.kind === "refused") {
      return {
        ok: false,
        exitCode: 1,
        stderr: b.stderr,
        stdout: "",
        timedOut: false,
        durationMs,
        parsed: parseCompilerDiags(b.stderr),
      };
    }
    const stderr = b.stderr ?? "";
    return {
      ok: true,
      exitCode: 0,
      stderr,
      stdout: b.stdout ?? "(wat)",
      timedOut: false,
      durationMs,
      parsed: parseCompilerDiags(stderr),
    };
  }

  async format(req: CheckRequest, deadlineMs: number): Promise<FormatResult> {
    this.formatCalls.push({ req, deadlineMs });
    const b = this.next();
    const durationMs = "durationMs" in b && b.durationMs !== undefined ? b.durationMs : 1;
    if (b.kind === "throw") throw new Error(b.message);
    if (b.kind === "timeout") {
      await never();
      throw new Error("unreachable");
    }
    if (b.kind === "refused") {
      return { ok: false, exitCode: 1, stderr: b.stderr, stdout: "", timedOut: false, durationMs };
    }
    return {
      ok: true,
      exitCode: 0,
      stderr: "",
      stdout: b.kind === "okFmt" ? b.text : (b.stdout ?? ""),
      timedOut: false,
      durationMs,
      text: b.kind === "okFmt" ? b.text : (b.stdout ?? ""),
    };
  }
}

function never(): Promise<never> {
  return new Promise(() => {
    /* intentionally never settles */
  });
}

/** A runner whose calls hang forever (the pure DNF double). */
export class HangingRunner implements Runner {
  readonly calls: RecordedCall[] = [];
  info(): RunnerInfo {
    return { kind: "hanging", detail: "never settles" };
  }
  async dispose(): Promise<void> {
    /* nothing */
  }
  async check(req: CheckRequest, deadlineMs: number): Promise<CheckResult> {
    this.calls.push({ req, deadlineMs });
    await never();
    throw new Error("unreachable");
  }
  async format(req: CheckRequest, deadlineMs: number): Promise<FormatResult> {
    this.calls.push({ req, deadlineMs });
    await never();
    throw new Error("unreachable");
  }
}

/** Direct use of the deadline race in tests (the DNF unit pin). */
export async function raceNever(capMs: number): Promise<typeof RUNNER_TIMEOUT> {
  const result = await raceDeadline<string>(never(), capMs);
  return result as typeof RUNNER_TIMEOUT;
}
