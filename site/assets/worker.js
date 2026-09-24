// The playground's run worker: execute compiled programs off the main
// thread, so a long-running program (naive fib(100) is ~10^21 calls) can
// never freeze the page — the UI terminates this worker by hand or at the
// wall-clock cap and says so honestly. COMPILATION happens on the page's
// main thread: V8 compiles the same source into different (invalid) bytes
// inside a worker context — the STARTER program's artifact was rejected
// by the worker's validator while the byte-identical main-thread artifact
// validated everywhere.

import { initCompiler, runProgram } from "./compiler.js";

// the page warms the worker at load; runs queue behind it in message order
self.onmessage = async (e) => {
  const msg = e.data;
  const onProgress = (loaded, total, done) =>
    postMessage({ kind: "progress", loaded, total, done });
  if (msg.kind === "warm") {
    try {
      postMessage({ kind: "phase", phase: "boot" });
      await initCompiler(onProgress);
      postMessage({ kind: "ready" });
    } catch (err) {
      postMessage({ kind: "boot-error", stderr: String(err.message || err) });
    }
    return;
  }
  const { id, program, stdin } = msg;
  try {
    const t0 = performance.now();
    const run = await runProgram(program, stdin || null);
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
