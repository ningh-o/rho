// runtime.js — the rho compiler's fmt face for Node.
//
// Loads the pinned compiler artifact (assets/rho-compiler.wasm — the
// self-hosted mirror's app face with an empty MODS_APP, baked per
// tools/build-artifact.mjs) and exposes the formatter over the app
// face: the artifact reads /main.rho (plus the /mode marker "fmt")
// from its in-memory filesystem and prints the canonical text to
// stdout; a refusal rides stderr with a nonzero exit, exactly as the
// command-line `rho fmt` behaves.
//
// Everything here is SYNCHRONOUS: the artifact imports no suspending
// WASI calls, so the synchronous WebAssembly.Module/Instance APIs are
// exactly equivalent to the async ones and prettier's sync parser can
// call straight in. The module is compiled once per process; every
// call instantiates a fresh instance over a fresh filesystem (no
// state leaks between runs).

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { runWasm, createFS } from "./wasi.js";

const generationUrl = new URL("../assets/generation.json", import.meta.url);
const compilerUrl = new URL("../assets/rho-compiler.wasm", import.meta.url);

let cachedBytes = null;
let cachedModule = null;

export function loadGeneration() {
  return JSON.parse(readFileSync(generationUrl, "utf8"));
}

// warm the compiler: read the artifact and compile its module once.
// Idempotent; safe to call eagerly or lazily.
export function warmCompiler() {
  if (!cachedModule) {
    if (!cachedBytes) cachedBytes = new Uint8Array(readFileSync(fileURLToPath(compilerUrl)));
    cachedModule = new WebAssembly.Module(cachedBytes);
  }
  return cachedModule;
}

// Format rho source with the compiler's own `fmt` (canonical text to
// stdout; diagnostics to stderr, nonzero exit on syntax errors).
// Returns { ok, text, stderr, exitCode }.
//
// Note the formatter's law-scope: the self-hosted fmt is the repo's
// subset-grammar formatter (tests/run-fmt-self.sh pins it against boot
// on that scope). See README "Formatter scope" for what that means
// for programs outside it.
export function fmtRho(source) {
  const module = warmCompiler();
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  // the mode marker is exactly "fmt" — no trailing newline (the site's
  // compiler.js writes it the same way; a trailing newline would fall
  // through to the emit path)
  fs.write("/mode", new TextEncoder().encode("fmt"));
  const result = runWasm(module, { args: ["rho"], fs });
  return {
    ok: result.exitCode === 0,
    text: result.stdout,
    stderr: result.stderr.trim(),
    exitCode: result.exitCode,
  };
}
