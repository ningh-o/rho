// compiler.js — the rho compiler's Node runtime face.
//
// Loads the pinned compiler artifact (assets/rho-compiler.wasm — the
// self-hosted mirror in its app configuration, MODS_APP = the std
// tree) and exposes compile/check/fmt/run over the app face: the
// artifact reads /main.rho (plus a /mode marker for check and fmt)
// from its in-memory filesystem and writes canonical WAT to stdout;
// assembleWat (wabt, same generation as the CLI's wat2wasm) turns the
// WAT into program bytes. A refusal rides stderr with a nonzero exit,
// exactly as the command-line tool behaves.
//
// The compiler's WebAssembly.Module is compiled ONCE per process and
// every run reuses it, paying only instantiation.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { runWasm, createFS } from "./wasi.js";
import { assembleWat } from "./wat.js";

const generationUrl = new URL("../assets/generation.json", import.meta.url);
const compilerUrl = new URL("../assets/rho-compiler.wasm", import.meta.url);

let cachedBytes = null;
let cachedModule = null;
let warming = null;

export function loadGeneration() {
  return JSON.parse(readFileSync(generationUrl, "utf8"));
}

function artifactPath() {
  return fileURLToPath(compilerUrl);
}

// warm the compiler: read the artifact and compile its module once.
// Idempotent; concurrent callers share one promise.
export function warmCompiler() {
  if (cachedModule) return Promise.resolve(cachedModule);
  if (!warming) {
    warming = (async () => {
      if (!cachedBytes) cachedBytes = new Uint8Array(readFileSync(artifactPath()));
      cachedModule = await WebAssembly.compile(cachedBytes);
      return cachedModule;
    })();
    // a failed warm must not poison every later call: drop the shared
    // promise so the next call reads the artifact again
    warming.catch(() => {
      warming = null;
    });
  }
  return warming;
}

// One compiler face over the app-face verbs. mode maps to the /mode
// marker the artifact reads — "build" (default, no marker),
// "check", "fmt" — the same verb surface the CLI's flags drive.
// Resolves { ok, wat, bytes, stderr, ms }:
//   ok:     exit 0 and the WAT assembled cleanly
//   wat:    the compiler's canonical WAT (stdout)
//   bytes:  the program's wasm bytes (wabt-assembled; null for check)
//   stderr: diagnostics (empty on success)
export async function compileRho(source, { mode = "build" } = {}) {
  await warmCompiler();
  const module = cachedModule;
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  if (mode === "check" || mode === "fmt") {
    fs.write("/mode", new TextEncoder().encode(mode));
  }
  const t0 = performance.now();
  const result = await runWasm(cachedBytes, {
    args: mode === "build" ? ["rho", "build", "/main.rho"] : ["rho"],
    fs,
    module,
  });
  const ms = performance.now() - t0;
  if (result.exitCode !== 0) {
    return { ok: false, wat: null, bytes: null, stderr: result.stderr.trim(), ms };
  }
  if (mode === "check") {
    return { ok: true, wat: null, bytes: null, stderr: result.stderr.trim(), ms };
  }
  if (mode === "fmt") {
    return { ok: true, wat: result.stdout, bytes: null, stderr: result.stderr.trim(), ms };
  }
  let bytes;
  try {
    bytes = await assembleWat(result.stdout);
  } catch (e) {
    return {
      ok: false,
      wat: null,
      bytes: null,
      stderr: "wabt: " + (e && e.message ? e.message : String(e)),
      ms,
    };
  }
  return { ok: true, wat: result.stdout, bytes, stderr: result.stderr.trim(), ms };
}

// Run a compiled program's bytes on the same shim (fd_write + proc_exit,
// static stdin). Returns { stdout, stderr, exitCode }.
export async function runProgram(bytes, { stdin = null } = {}) {
  return runWasm(bytes, { stdin });
}
