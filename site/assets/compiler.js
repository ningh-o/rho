// Loads the rho compiler (a wasm32-wasi program itself) and exposes
// compile(source) -> { ok, program: Uint8Array, stderr }.
import { runWasm, createFS } from "./wasi.js";

let compilerBytesPromise = null;
let cachedBytes = null;

function fetchCompiler(onProgress) {
  if (!compilerBytesPromise) {
    // anchored to this module's URL: the same path resolves on the page and
    // inside a worker (whose relative base is /assets/, not /)
    compilerBytesPromise = (async () => {
      const res = await fetch(new URL("rho-boot.wasm", import.meta.url));
      if (!res.ok) throw new Error("cannot load the compiler (" + res.status + ")");
      if (!res.body || !onProgress) {
        cachedBytes = new Uint8Array(await res.arrayBuffer());
        return cachedBytes;
      }
      // stream so the page can show progress. Content-Length is the
      // compressed size while the reader yields decoded bytes — cap the
      // fraction and trust the end of the stream, not the arithmetic
      const total = Number(res.headers.get("content-length")) || 0;
      const reader = res.body.getReader();
      const chunks = [];
      let loaded = 0;
      let lastPost = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        loaded += value.length;
        const now = performance.now();
        if (now - lastPost > 80) {
          lastPost = now;
          onProgress(loaded, total, false);
        }
      }
      onProgress(loaded, total, true);
      const out = new Uint8Array(loaded);
      let off = 0;
      for (const c of chunks) {
        out.set(c, off);
        off += c.length;
      }
      cachedBytes = out;
      return out;
    })();
  }
  return compilerBytesPromise;
}

export async function initCompiler(onProgress) {
  if (cachedBytes) {
    if (onProgress) onProgress(cachedBytes.length, cachedBytes.length, true);
    return;
  }
  await fetchCompiler(onProgress);
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
