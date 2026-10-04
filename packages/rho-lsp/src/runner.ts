// runner.ts — the seam between the LSP shell and the compiler. The
// server knows NOTHING about how the compiler is invoked; it hands a
// CheckRequest to a Runner and consumes a RawResult. Two shapes are
// contemplated (ecosystem.md §4: "the same embedding the playground
// uses"):
//
//   - SubprocessRunner (chosen for this delivery): boot bakes the
//     document into a fresh self-hosted compiler wasm with `--set
//     SRC=... --set MODS=...`, then wasmtime runs it bare — the exact
//     two-step face tests/run-corpus-diff.sh and tests/run-selfhost.sh
//     pin, with a pinned wasmtime. A wedged child is killable, which is
//     what makes the per-request time cap honest.
//   - an in-process wasm shim (the browser-playground face) can slot in
//     behind the same interface; it needs a WASI shim and a
//     worker-thread supervisor for its cap. Not built this round — the
//     choice and its reasons live in WORKTREE-NOTES.md.
//
// Pins: test/runner.test.ts drives SubprocessRunner against a fake
// toolchain; test/server-transcript.test.ts drives the server through
// fake Runners.

import type { ParsedDiags } from "./diag-parser.js";

/** The document under the cursor plus the open module texts around it. */
export interface CheckRequest {
  /** The root document's text (what SRC carries). */
  rootText: string;
  /**
   * Same-directory sibling modules to ride MODS, name-sorted, each as
   * `{ path, text }` with `path` relative to the root's directory.
   * The mirror reads the module tree from MODS blocks; boot would read
   * the same files from disk.
   */
  modules: { path: string; text: string }[];
  /** Editor-facing path of the root document, for diagnostics' file field. */
  docPath?: string;
}

export interface RawResult {
  /** true when the compiler accepted the document (rc 0). */
  ok: boolean;
  /** The wasm face's exit code; null when the run never completed. */
  exitCode: number | null;
  /** The compiler's stderr, verbatim. */
  stderr: string;
  /** The compiler's stdout, verbatim (WAT on success, canonical text for fmt). */
  stdout: string;
  /** true when the request was aborted at its time cap. */
  timedOut: boolean;
  /** Wall-clock duration of the runner call. */
  durationMs: number;
}

/** A completed check with diagnostics parsed (order preserved). */
export interface CheckResult extends RawResult {
  parsed: ParsedDiags;
}

export interface FormatResult extends RawResult {
  /** Canonical text when ok; undefined otherwise (fmt never half-formats). */
  text?: string;
}

/** What the server reports about itself at initialize. */
export interface RunnerInfo {
  kind: string;
  detail: string;
}

/**
 * Every runner method takes a hard deadline (ms). A runner MUST abort
 * its work and return `timedOut: true` by then; the server enforces a
 * second, outer cap — a runner that never settles is abandoned by the
 * server race, the request degrades, and the failure is counted.
 */
export interface Runner {
  check(req: CheckRequest, deadlineMs: number): Promise<CheckResult>;
  format(req: CheckRequest, deadlineMs: number): Promise<FormatResult>;
  info(): RunnerInfo;
  dispose(): Promise<void>;
}

export const RUNNER_TIMEOUT = Symbol("runner-timeout");

/**
 * The transport budget for ONE build argument: every `--set` value
 * rides a single argv element, and the POSIX exec ceiling per element
 * is MAX_ARG_STRLEN = 32 pages = 131072 bytes (Linux kernel,
 * include/uapi/linux/binfmts.h) — above it execve fails with E2BIG
 * before the child ever runs. macOS's per-element ceiling is higher
 * (bounded by ARG_MAX overall), so this Linux limit is the portable,
 * conservative budget: a request whose SRC or MODS value does not fit
 * is skipped, never spawned. (The alternative transport — handing SRC/
 * MODS to boot through a file or stdin — does not exist: boot/driver.c
 * reads `--set` values from argv only, and changing the invocation
 * would break the byte-for-byte run-corpus-diff face this runner
 * pins.)
 */
export const MAX_ARG_BYTES = 131072;

/** Which `--set` value (if any) exceeds the transport budget, named for
 * an honest log/skip ("SRC" | "MODS"); undefined when both fit. */
export function transportOverflow(rootText: string, mods: string): "SRC" | "MODS" | undefined {
  if (Buffer.byteLength(`SRC=${rootText}`, "utf8") > MAX_ARG_BYTES) return "SRC";
  if (Buffer.byteLength(`MODS=${mods}`, "utf8") > MAX_ARG_BYTES) return "MODS";
  return undefined;
}

/**
 * Race a runner call against the server's outer cap. Resolves with
 * RUNNER_TIMEOUT when the runner does not settle in time; a runner
 * ERROR rejects through unchanged (timeout and failure are counted
 * separately by the server). The losing timer of a race the runner
 * wins is cleared; a runner that never settles leaks its own promise
 * by design — a real runner aborts itself, and nothing the server does
 * downstream can be poisoned by the abandoned call.
 */
export function raceDeadline<T>(promise: Promise<T>, capMs: number): Promise<T | typeof RUNNER_TIMEOUT> {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        resolve(RUNNER_TIMEOUT);
      }
    }, capMs);
    timer.unref?.();
    promise.then(
      (value) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          resolve(value);
        }
      },
      (err: unknown) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(err instanceof Error ? err : new Error(String(err)));
        }
      },
    );
  });
}

/**
 * Assemble the MODS build parameter: `@MOD@ <path>\n<text>` blocks,
 * NAME-SORTED — the gate's own law (tools/gate.sh leg 4 and the corpus
 * differential both assemble sorted); the caller's map order must not
 * leak into the baked bytes. Byte-exact format pinned by
 * test/runner.test.ts against the face run-corpus-diff.sh:41-47 uses.
 */
export function assembleMods(modules: { path: string; text: string }[]): string {
  const sorted = [...modules].sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  let mods = "";
  for (const m of sorted) {
    mods += `@MOD@ ${m.path}\n${m.text}\n`;
  }
  return mods;
}
