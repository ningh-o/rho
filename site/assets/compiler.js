// Loads the rho compiler (a wasm32-wasi program itself) and exposes
// compile(source) -> { ok, program: Uint8Array, stderr }.
import { runWasm, createFS } from "./wasi.js";

let compilerBytesPromise = null;
let cachedBytes = null;

function fetchCompiler() {
  if (!compilerBytesPromise) {
    // anchored to this module's URL: the same path resolves on the page and
  // inside a worker (whose relative base is /assets/, not /)
  compilerBytesPromise = fetch(new URL("rho-boot.wasm", import.meta.url)).then((r) => {
      if (!r.ok) throw new Error("cannot load the compiler (" + r.status + ")");
      return r.arrayBuffer();
    }).then((buf) => {
      cachedBytes = new Uint8Array(buf);
      return cachedBytes;
    });
  }
  return compilerBytesPromise;
}

export async function initCompiler() {
  await fetchCompiler();
}

let compileSeq = 0;

// Compile rho source to a wasm32-wasi program. Resolves with
// { ok, program, stderr, ms }.
export async function compile(source) {
  const bytes = await fetchCompiler();
  const seq = ++compileSeq;
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  const t0 = performance.now();
  const result = await runWasm(bytes, {
    args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
    fs,
  });
  const ms = performance.now() - t0;
  if (seq !== compileSeq) return { ok: false, stale: true, program: null, stderr: "", ms };
  const program = fs.read("/out.wasm");
  return {
    ok: !!program && result.exitCode === 0,
    program,
    stderr: result.stderr.trim(),
    ms,
  };
}

// Run a compiled program. Returns { stdout, stderr, exitCode, ms }.
export async function runProgram(program) {
  const t0 = performance.now();
  const result = await runWasm(program, {});
  return { ...result, ms: performance.now() - t0 };
}
