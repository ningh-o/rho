#!/usr/bin/env node
// build-check.mjs — produce the LSP package's pinned compiler
// generation: the check and fmt faces of the self-hosted compiler plus
// artifacts/generation.json, the pin the server validates at startup.
//
// The route is tests/run-selfhost.sh's, exactly: boot compiles
// libs/compiler (the document rides nothing here — SRC is the
// compiler's own source, so the artifacts are the STOCK mirror and the
// FMT=1 self face; per-request documents are baked by the server's
// runner the same way). Legs:
//
//   1. boot present (or --make builds it; never built silently)
//   2. artifacts/rho-check.wasm   boot build, SRC=compiler, MODS=tree
//   3. artifacts/rho-fmt.wasm     same + FMT=1
//   4. determinism: rebuild both into a temp tree, byte-compare
//      (the language's D1 law reaches this script; inequality = alarm)
//   5. generation.json: rho commit, per-source SHA-256, artifact
//      SHA-256, pinned tool versions — byte-stable, no timestamps
//
// This script RUNS the toolchain (it is the owner's build step, not
// part of any test). Usage:
//
//   node tools/build-check.mjs [--repo <rho root>] [--rho <boot binary>]
//                              [--make] [--out <artifacts dir>]
//
// Exit 0 green; 1 a red leg (with the reason); 2 usage/config trouble.

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { mkdtempSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import process from "node:process";

const DEFAULT_CAP_MS = 600_000; // the gate's own per-build budget

function fail(msg) {
  process.stderr.write(`build-check: ${msg}\n`);
  process.exit(msg.startsWith("usage") ? 2 : 1);
}

function parseArgs(argv) {
  const opts = { make: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--repo") opts.repo = argv[++i];
    else if (a === "--rho") opts.rho = argv[++i];
    else if (a === "--out") opts.out = argv[++i];
    else if (a === "--make") opts.make = true;
    else fail(`usage: unknown flag ${a}`);
  }
  return opts;
}

function runCapped(cmd, args, cwd, capMs) {
  return new Promise((done) => {
    // detached on POSIX: the cap kills the whole process group, so a
    // wedged child cannot leave an orphan holding the stdio pipes open
    const child = spawn(cmd, args, { cwd, windowsHide: true, detached: process.platform !== "win32" });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    const killTree = (sig) => {
      try {
        if (child.pid !== undefined && process.platform !== "win32") process.kill(-child.pid, sig);
        else child.kill(sig);
      } catch {
        child.kill(sig);
      }
    };
    const timer = setTimeout(() => {
      timedOut = true;
      killTree("SIGKILL");
    }, capMs);
    child.stdout?.on("data", (d) => (stdout += d.toString("utf8")));
    child.stderr?.on("data", (d) => (stderr += d.toString("utf8")));
    child.on("error", (err) => {
      clearTimeout(timer);
      done({ code: -1, stdout, stderr: stderr + String(err), timedOut: false });
    });
    child.on("close", (code) => {
      clearTimeout(timer);
      done({ code, stdout, stderr, timedOut });
    });
  });
}

function sha256File(p) {
  return createHash("sha256").update(readFileSync(p)).digest("hex");
}

const here = dirname(fileURLToPath(import.meta.url));
const pkgRoot = resolve(here, "..");
const opts = parseArgs(process.argv.slice(2));
const repoRoot = resolve(opts.repo ?? resolve(pkgRoot, "..", ".."));
const artifactsDir = resolve(opts.out ?? join(pkgRoot, "artifacts"));
const bootBinary = resolve(opts.rho ?? join(repoRoot, "build", "rho"));
const compilerDir = join(repoRoot, "libs", "compiler");

if (!existsSync(compilerDir)) fail(`usage: no libs/compiler under ${repoRoot} (pass --repo)`);
if (!existsSync(bootBinary)) {
  if (opts.make) {
    process.stdout.write("build-check: boot binary missing; running `make all`\n");
    const mk = await runCapped("make", ["all"], repoRoot, DEFAULT_CAP_MS);
    if (mk.code !== 0) fail(`make all failed (rc=${mk.code}):\n${mk.stderr}`);
  } else {
    fail(`usage: boot binary not found at ${bootBinary} — run \`make all\` or pass --rho/--make`);
  }
}
if (!existsSync(bootBinary)) fail(`boot binary still missing at ${bootBinary}`);

// ---- leg 1/2: the check and fmt faces (run-selfhost.sh's route) ----

const compilerSources = readdirSync(compilerDir)
  .filter((f) => f.endsWith(".rho"))
  .sort();
if (!compilerSources.includes("main.rho")) fail("libs/compiler/main.rho missing");
const src = readFileSync(join(compilerDir, "main.rho"), "utf8");
let mods = "";
for (const f of compilerSources) {
  if (f === "main.rho") continue;
  mods += `@MOD@ ${f}\n${readFileSync(join(compilerDir, f), "utf8")}\n`;
}

mkdirSync(artifactsDir, { recursive: true });
const faces = [
  { name: "rho-check.wasm", fmt: false },
  { name: "rho-fmt.wasm", fmt: true },
];

async function buildFace(face, outDir) {
  const out = join(outDir, face.name);
  const args = ["build", join("libs", "compiler", "main.rho"), "-o", out, "--set", `SRC=${src}`, "--set", `MODS=${mods}`];
  if (face.fmt) args.push("--set", "FMT=1");
  const r = await runCapped(bootBinary, args, repoRoot, DEFAULT_CAP_MS);
  // throws, never fail(): process.exit would skip the caller's finally
  // (the determinism leg's temp-tree cleanup). Callers turn this into
  // fail() once their cleanup has run.
  if (r.timedOut) throw new Error(`building ${face.name} exceeded the ${DEFAULT_CAP_MS} ms cap`);
  if (r.code !== 0) throw new Error(`boot could not build ${face.name} (rc=${r.code}):\n${r.stderr}`);
  if (!existsSync(out)) throw new Error(`boot reported success but ${out} is missing`);
  return out;
}

const errorMessage = (err) => (err instanceof Error ? err.message : String(err));

for (const face of faces) {
  process.stdout.write(`build-check: building ${face.name}\n`);
  try {
    await buildFace(face, artifactsDir);
  } catch (err) {
    fail(errorMessage(err));
  }
}

// ---- leg 3: the determinism alarm (double build, byte-compare) ----

process.stdout.write("build-check: determinism leg (rebuild and byte-compare)\n");
const tmp = mkdtempSync(join(artifactsDir, ".rebuild-"));
let rebuildIdentical = true;
let determinismAlarm = false;
let legError = null;
try {
  for (const face of faces) {
    const out = await buildFace(face, tmp);
    if (sha256File(out) !== sha256File(join(artifactsDir, face.name))) {
      rebuildIdentical = false;
      determinismAlarm = true;
      process.stderr.write(`build-check: DETERMINISM ALARM — ${face.name} differs across identical builds\n`);
    }
  }
} catch (err) {
  legError = err;
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
// fail only after the temp tree is gone (process.exit skips finally blocks)
if (legError !== null) fail(`determinism leg could not rebuild: ${errorMessage(legError)}`);
if (determinismAlarm) fail("determinism leg red: two identical builds differ");

// ---- leg 4: the pin ----

async function toolVersion(cmd) {
  const r = await runCapped(cmd, ["--version"], repoRoot, 10_000);
  if (r.code !== 0) return null;
  return r.stdout.trim().split("\n")[0] ?? null;
}

const git = await runCapped("git", ["rev-parse", "HEAD"], repoRoot, 10_000);
if (git.code !== 0) fail(`git rev-parse failed: ${git.stderr}`);

/** JSON with a fixed key order, 2-space indent, trailing newline —
 * byte-stable across runs (no timestamps, no volatile fields). */
function stableJson(value) {
  return JSON.stringify(value, null, 2) + "\n";
}

const manifest = {
  schemaVersion: "rho-lsp.generation/1",
  rhoCommit: git.stdout.trim(),
  compilerSources: Object.fromEntries(
    compilerSources.map((f) => [f, sha256File(join(compilerDir, f))]),
  ),
  artifacts: Object.fromEntries(faces.map((f) => [f.name, sha256File(join(artifactsDir, f.name))])),
  tools: {
    wasmtime: await toolVersion("wasmtime"),
    wat2wasm: await toolVersion("wat2wasm"),
  },
  rebuildIdentical,
};

const manifestPath = join(artifactsDir, "generation.json");
writeFileSync(manifestPath, stableJson(manifest));

process.stdout.write(`build-check: generation pinned at ${manifestPath}\n`);
process.stdout.write(`  rho commit: ${manifest.rhoCommit}\n`);
process.stdout.write(`  wasmtime: ${manifest.tools.wasmtime ?? "NOT FOUND"}\n`);
process.stdout.write(`  wat2wasm: ${manifest.tools.wat2wasm ?? "NOT FOUND"}\n`);
process.stdout.write("build-check: green\n");
