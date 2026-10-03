// cli-reference.mjs — the parity harness's shared machinery.
//
// The acceptance law (docs/ecosystem.md §2): the plugin's wasm is
// byte-identical to the CLI's for every programs-tier program (same
// compiler, same flags). The "CLI path" is the differential's
// self-hosted leg (tests/run-corpus-diff.sh):
//
//   ./build/rho build libs/compiler/main.rho -o c.wasm \
//       --set "SRC=<program>" --set "MODS=<module tree>"
//   wasmtime c.wasm > c.wat          # the compiler emits canonical WAT
//   wat2wasm c.wat -o prog.wasm      # system wabt assembles
//
// The module tree is the same for every program (the corpus packages
// + the std tree, assembled verbatim as run-corpus-diff.sh does). A
// bounded re-run (once) guards every leg, exactly as the
// differential's environment-blip law does.

import { execFileSync } from "node:child_process";
import { existsSync, readFileSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { globRho } from "../tools/glob.mjs";

export const pkgDir = join(dirname(fileURLToPath(import.meta.url)), "..");
export const worktree = join(pkgDir, "..", "..");
export const programsDir = join(worktree, "tests", "suites", "programs");

export function repoAvailable() {
  return existsSync(join(worktree, "tests", "suites", "programs")) &&
    existsSync(join(worktree, "libs", "compiler", "main.rho"));
}

function need(cmd) {
  try {
    execFileSync("which", [cmd], { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
}

// the repo tools the CLI path needs; build/rho is built on demand
export function cliTools() {
  const boot = join(worktree, "build", "rho");
  if (!existsSync(boot)) {
    execFileSync("make", ["all"], { cwd: worktree, timeout: 180_000, stdio: "ignore" });
  }
  if (!existsSync(boot)) throw new Error("./build/rho missing — run `make all` in the worktree root");
  for (const cmd of ["wasmtime", "wat2wasm"]) {
    if (!need(cmd)) throw new Error(`${cmd} not found on PATH — the repo's gate tools are required for the CLI reference path`);
  }
  return { boot };
}

// the corpus packages + the std tree, assembled exactly as
// tests/run-corpus-diff.sh assembles MODS (paths relative to the
// entry's directory for the packages, repo-relative for std)
export function buildMods() {
  // run-corpus-diff.sh's exact glob sequence: geom levels 1-2, web
  // level 1, pk levels 1-3, then the std tree levels 1-3 (each level
  // name-sorted, matching zsh's glob order); the packages' paths are
  // stripped of "tests/suites/programs/" — the entry's directory —
  // keeping geom/, pk/, web/ in the module path
  const programs = "tests/suites/programs";
  const globs = [
    ["tests/suites/programs/geom", 1, 2, programs],
    ["tests/suites/programs/web", 1, 1, programs],
    ["tests/suites/programs/pk", 1, 3, programs],
    ["std", 1, 3, null],
  ];
  let mods = "";
  for (const [base, minDepth, maxDepth, stripPrefix] of globs) {
    for (let depth = minDepth; depth <= maxDepth; depth++) {
      const pattern = [...Array(depth - 1).fill("*"), "*.rho"].join("/");
      for (const rel of globRho(worktree, join(base, pattern))) {
        // globRho returns repo-relative paths; the packages' module
        // paths are stripped of the entry dir exactly as the
        // differential does; "$(cat f)" semantics on the content
        // (trailing newlines stripped, one added)
        const path = stripPrefix ? rel.slice(stripPrefix.length + 1) : rel;
        const text = readFileSync(join(worktree, rel), "utf8").replace(/\n+$/, "");
        mods += `@MOD@ ${path}\n${text}\n`;
      }
    }
  }
  return mods;
}

let scratch = null;
function scratchDir() {
  if (!scratch) {
    mkdirSync(join(worktree, "tmp"), { recursive: true });
    scratch = mkdtempSync(join(worktree, "tmp", "eco-vite-parity-"));
    process.on("exit", () => rmSync(scratch, { recursive: true, force: true }));
  }
  return scratch;
}

let modsCache = null;

// the CLI reference for one program: { ok, bytes, rc, stderr }.
// Every leg is time-capped; a failing leg re-runs ONCE (the
// differential's bounded re-check: environment blips fail once,
// regressions fail twice).
export function cliReference(src, sets = []) {
  if (!modsCache) modsCache = buildMods();
  const run = () => cliOnce(src, modsCache, sets);
  let r = run();
  if (!r.ok) r = run();
  return r;
}

function cliOnce(src, mods, sets) {
  const { boot } = cliTools();
  const t = scratchDir();
  const tag = createHash("sha1").update(src).digest("hex").slice(0, 12);
  const mirror = join(t, `m-${tag}.wasm`);
  const watPath = join(t, `p-${tag}.wat`);
  const progPath = join(t, `p-${tag}.wasm`);
  try {
    try {
      execFileSync(
        boot,
        [
          "build", "libs/compiler/main.rho", "-o", mirror,
          "--set", `SRC=${src}`,
          "--set", `MODS=${mods}`,
          ...sets.flatMap((s) => ["--set", s]),
        ],
        { cwd: worktree, timeout: 300_000, stdio: ["ignore", "ignore", "pipe"] },
      );
    } catch (e) {
      return { ok: false, rc: 1, bytes: null, stderr: String(e.stderr || e) };
    }
    let wat;
    try {
      wat = execFileSync("wasmtime", [mirror], {
        timeout: 120_000, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], maxBuffer: 1 << 28,
      });
    } catch (e) {
      return { ok: false, rc: 2, bytes: null, stderr: String(e.stderr || e) };
    }
    writeFileSync(watPath, wat);
    try {
      execFileSync("wat2wasm", [watPath, "-o", progPath], { timeout: 120_000, stdio: "ignore" });
    } catch (e) {
      return { ok: false, rc: 3, bytes: null, stderr: String(e) };
    }
    const bytes = readFileSync(progPath);
    return { ok: true, rc: 0, bytes, stderr: "" };
  } finally {
    for (const p of [mirror, watPath, progPath]) rmSync(p, { force: true });
  }
}

// the suite verb's golden headers (boot/test.c): each `// out:` line
// pins one full output line; an optional `// rawout:` tail is appended
// verbatim with NO trailing newline; `// exit:` pins the exit code;
// `// set:` names a build parameter (none exist in the programs tier
// today — the parity harness refuses to guess at one)
export function parseGoldens(src) {
  const out = [];
  let rawout = null;
  let exit = null;
  const sets = [];
  for (const line of src.split("\n")) {
    // the s flag: a golden header may carry RAW control bytes (066's
    // carriage-return pin rides inside the // out: line) and JS's dot
    // would stop at a \r without it — boot's reader takes the whole
    // rest of the line, control bytes included
    const m = /^\/\/ (out|rawout|exit|set): ?(.*)$/s.exec(line);
    if (!m) continue;
    if (m[1] === "out") out.push(m[2]);
    else if (m[1] === "rawout") rawout = m[2];
    else if (m[1] === "exit") exit = Number(m[2]);
    else sets.push(m[2]);
  }
  let stdout = out.map((l) => l + "\n").join("");
  if (rawout !== null) stdout += rawout;
  return { out, rawout, exit, stdout, sets };
}
