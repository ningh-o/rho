// The playground's run worker: compile + execute off the main thread, so a
// long-running program (naive fib(100) is ~10^21 calls) can never freeze
// the page — the UI just terminates this worker and says so honestly.

import { initCompiler, compile, runProgram } from "./compiler.js";

// the page warms the worker at load; runs queue behind it in message order
self.onmessage = async (e) => {
  const msg = e.data;
  if (msg.kind === "warm") {
    try {
      postMessage({ kind: "phase", phase: "boot" });
      await initCompiler();
      postMessage({ kind: "ready" });
    } catch (err) {
      postMessage({ kind: "boot-error", stderr: String(err.message || err) });
    }
    return;
  }
  const { id, source } = msg;
  try {
    postMessage({ kind: "phase", id, phase: "boot" });
    await initCompiler();
    postMessage({ kind: "phase", id, phase: "compile" });
    const compiled = await compile(source);
    if (!compiled.ok) {
      postMessage({
        kind: "done",
        id,
        ok: false,
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
    postMessage({
      kind: "done",
      id,
      ok: false,
      stderr: "runtime error: " + (err.message || err),
    });
  }
};
