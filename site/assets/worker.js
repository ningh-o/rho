// The playground's run worker: compile + execute off the main thread, so a
// long-running program (naive fib(100) is ~10^21 calls) can never freeze
// the page — the UI just terminates this worker and says so honestly.

import { initCompiler, compile, runProgram } from "./compiler.js";

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
  const { id, source } = msg;
  // which half of the pipeline a failure came from — the page labels it
  let stage = "compile";
  try {
    postMessage({ kind: "phase", id, phase: "boot" });
    await initCompiler(onProgress);
    postMessage({ kind: "phase", id, phase: "compile" });
    const compiled = await compile(source);
    if (!compiled.ok) {
      postMessage({
        kind: "done",
        id,
        ok: false,
        stage,
        stderr: compiled.stderr || "compilation failed",
        compileMs: compiled.ms,
      });
      return;
    }
    postMessage({
      kind: "phase",
      id,
      phase: "run",
      compileMs: compiled.ms,
      bytes: compiled.program.length,
    });
    stage = "run";
    const run = await runProgram(compiled.program);
    postMessage({
      kind: "done",
      id,
      ok: true,
      stdout: run.stdout,
      stderr: run.stderr,
      exitCode: run.exitCode,
      runMs: run.ms,
      compileMs: compiled.ms,
      bytes: compiled.program.length,
    });
  } catch (err) {
    const raw = String((err && err.message) || err);
    // a blown host call stack surfaces as a plain JS RangeError — say what
    // it actually is and how to fix it, instead of leaking engine jargon
    const stderr = /maximum call stack|call stack exhausted/i.test(raw)
      ? "stack overflow: recursion exceeded the host's call-stack limit " +
        "(browsers give wasm about 1 MB). Shrink the recursion, or make it " +
        "tail-recursive — rho turns tail calls into loops automatically."
      : "runtime error: " + raw;
    postMessage({
      kind: "done",
      id,
      ok: false,
      stage,
      stderr,
    });
  }
};
