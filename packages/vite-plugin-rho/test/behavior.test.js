// behavior.test.js — the plugin's wasm not only matches the CLI byte
// for byte, the programs BEHAVE: every programs-tier program with
// goldens runs through the same WASI shim the compile rode, stdout +
// exit compared against the suite verb's pins (// out: / // rawout: /
// // exit: — boot/test.c's exact joining rule) or, for byte goldens,
// the sibling .out file judged byte-for-byte (run-corpus-repo.sh's
// law). A sample (the byte golden n11 plus a feature spread) re-runs
// under wasmtime, the same runner the repo's differential grades on.

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync, readdirSync, writeFileSync, rmSync, existsSync } from "node:fs";
import { join } from "node:path";
import { compileRho, warmCompiler, runProgram } from "../src/compiler.js";
import { worktree, repoAvailable, parseGoldens } from "../tools/cli-reference.mjs";

// the programs whose modules live in the corpus packages (geom/web/pk):
// they refuse at compile time (asserted in parity.test.js)
const PACKAGE_USERS = new Set([
  "032_pkg_test",
  "107_mod_structs_test",
  "t09_pub_use_forms_test",
]);

// the wasmtime sample: the byte golden + a feature spread
const WASMTIME_SAMPLE = new Set([
  "n11_str_slice_edges",
  "010_statics_test",
  "024_to_str_test",
  "042_divrem_signed_test",
  "078_closures_capture_test",
  "092_slice_copy_test",
  "n19_targs_test",
  "n10_format_edges_test",
]);

function programsDir() {
  return join(worktree, "tests", "suites", "programs");
}

// the expected stdout bytes: a sibling .out file is the byte golden
// (headers cannot carry non-UTF8); otherwise the // out:/rawout:
// headers joined exactly as boot/test.c joins them
function expectedStdout(name, goldens) {
  const outPath = join(programsDir(), `${name}.out`);
  if (existsSync(outPath)) return readFileSync(outPath, "utf8");
  if (goldens.out.length === 0 && goldens.rawout === null) return null;
  return goldens.out.map((l) => l + "\n").join("") + (goldens.rawout ?? "");
}

test("programs-tier behavior: goldens through the plugin's wasm", { timeout: 600_000 }, async (t) => {
  if (!repoAvailable()) {
    t.skip("the rho worktree (tests/suites/programs) is not present — npm-consumer install");
    return;
  }
  await warmCompiler();

  const files = readdirSync(programsDir()).filter((f) => f.endsWith(".rho")).sort();
  const results = [];
  for (const f of files) {
    const name = f.replace(/\.rho$/, "");
    const src = readFileSync(join(programsDir(), f), "utf8");
    const goldens = parseGoldens(src);
    if (goldens.sets.length > 0) continue; // none today (see parity.test.js)
    if (PACKAGE_USERS.has(name)) continue; // refuses at compile (asserted in parity)
    const compiled = await compileRho(src);
    assert.ok(compiled.ok, `${name} must compile: ${compiled.stderr}`);
    const run = await runProgram(compiled.bytes);
    const want = expectedStdout(name, goldens);
    const problems = [];
    if (want !== null && run.stdout !== want) {
      problems.push(`stdout: want ${JSON.stringify(want.slice(0, 120))}, got ${JSON.stringify(run.stdout.slice(0, 120))}`);
    }
    if (goldens.exit !== null && run.exitCode !== goldens.exit) {
      problems.push(`exit: want ${goldens.exit}, got ${run.exitCode}`);
    }
    results.push({ name, problems });
  }

  const red = results.filter((r) => r.problems.length > 0);
  console.log(`behavior: ${results.length} programs run, ${results.length - red.length} green, ${red.length} red`);
  for (const r of red) console.log(`  RED ${r.name}: ${r.problems.join("; ")}`);
  assert.equal(red.length, 0, `behavior failed for: ${red.map((r) => r.name).join(", ")}`);
});

test("wasmtime sample: goldens under the repo's own runner", { timeout: 300_000 }, async (t) => {
  if (!repoAvailable()) {
    t.skip("the rho worktree (tests/suites/programs) is not present — npm-consumer install");
    return;
  }
  let hasWasmtime = true;
  try {
    execFileSync("which", ["wasmtime"], { stdio: "ignore" });
  } catch {
    hasWasmtime = false;
  }
  if (!hasWasmtime) {
    t.skip("wasmtime not on PATH — the shim run above already graded behavior");
    return;
  }
  await warmCompiler();
  for (const name of [...WASMTIME_SAMPLE].sort()) {
    const src = readFileSync(join(programsDir(), `${name}.rho`), "utf8");
    const goldens = parseGoldens(src);
    const compiled = await compileRho(src);
    assert.ok(compiled.ok, `${name} must compile: ${compiled.stderr}`);
    const outPath = join(worktree, "tmp", `vite-plugin-rho-sample-${name}.wasm`);
    writeFileSync(outPath, compiled.bytes);
    try {
      let stdout = "";
      let exit = 0;
      try {
        stdout = execFileSync("wasmtime", [outPath], {
          timeout: 60_000, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"],
        });
      } catch (e) {
        stdout = String(e.stdout ?? "");
        exit = e.status ?? 0;
      }
      const want = expectedStdout(name, goldens);
      if (want !== null) assert.equal(stdout, want, `${name} stdout under wasmtime`);
      if (goldens.exit !== null) assert.equal(exit, goldens.exit, `${name} exit under wasmtime`);
    } finally {
      rmSync(outPath, { force: true });
    }
  }
});
