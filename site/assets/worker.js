// The playground's run worker: execute compiled programs off the main
// thread, so a long-running program (naive fib(100) is ~10^21 calls) can
// never freeze the page — the UI terminates this worker by hand or at the
// wall-clock cap and says so honestly. COMPILATION happens on the page's
// main thread: V8 compiles the same source into different (invalid) bytes
// inside a worker context — the STARTER program's artifact was rejected
// by the worker's validator while the byte-identical main-thread artifact
// validated everywhere.
//
// Interactive stdin: an {interactive: true} run whose program reads past
// the provided stdin SUSPENDS here (JSPI suspending imports) and asks the
// page — {kind: "stdin-need"} — which answers {kind: "stdin-give", text}
// (a line; the newline is added here) or {kind: "stdin-give"} (no text:
// EOF). A worker that cannot suspend never asks; reads see EOF.

import { initCompiler, runProgram } from "./compiler.js";

const INTERACTIVE =
  typeof WebAssembly.Suspending === "function" &&
  typeof WebAssembly.promising === "function";

let giveResolver = null;

self.onmessage = async (e) => {
  const msg = e.data;
  const onProgress = (loaded, total, done) =>
    postMessage({ kind: "progress", loaded, total, done });
  if (msg.kind === "warm") {
    try {
      postMessage({ kind: "phase", phase: "boot" });
      await initCompiler(onProgress);
      postMessage({ kind: "ready", interactive: INTERACTIVE });
    } catch (err) {
      postMessage({ kind: "boot-error", stderr: String(err.message || err) });
    }
    return;
  }
  if (msg.kind === "stdin-give") {
    if (giveResolver) {
      const resolve = giveResolver;
      giveResolver = null;
      resolve(msg.text == null ? null : new TextEncoder().encode(msg.text + "\n"));
    }
    return;
  }
  const { id, program, stdin, interactive } = msg;
  const stdinProvider =
    INTERACTIVE && interactive
      ? () =>
          new Promise((resolve) => {
            giveResolver = resolve;
            postMessage({ kind: "stdin-need", id });
          })
      : null;
  try {
    const t0 = performance.now();
    const run = await runProgram(program, stdin || null, stdinProvider);
    postMessage({
      kind: "done",
      id,
      ok: true,
      stdout: run.stdout,
      stderr: run.stderr,
      exitCode: run.exitCode,
      runMs: run.ms,
      compileMs: 0,
      bytes: program.length,
    });
  } catch (err) {
    const raw = String((err && err.message) || err);
    // a blown host call stack surfaces as a plain JS RangeError — the page
    // already labels the stage, so one honest word suffices
    postMessage({
      kind: "done",
      id,
      ok: false,
      stage: "run",
      stderr: /maximum call stack|call stack exhausted/i.test(raw)
        ? "stack overflow"
        : raw,
    });
  }
};
