// parity.test.js — THE acceptance law (docs/ecosystem.md §2):
//
//   The plugin's wasm is byte-identical to the CLI's for every
//   programs-tier program (same compiler, same flags).
//
// Plugin side: the pinned artifact on the Node runtime face, the WAT
// assembled by the vendored wabt (the site's own vendor file). CLI
// side: the differential's mirror leg (tools/cli-reference.mjs). The
// package-module users (032/107/t09) once refused cleanly — the app
// face had no runtime module channel; the /mods channel closed the
// gap, so every program now compiles on both sides and the bytes must
// agree.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { compileRho, warmCompiler } from "../src/compiler.js";
import { collectModulesFor } from "../src/index.js";
import { pkgDir, repoAvailable, cliTools, cliReference, parseGoldens } from "../tools/cli-reference.mjs";

test("programs-tier byte parity: plugin == CLI, every program", { timeout: 900_000 }, async (t) => {
  if (!repoAvailable()) {
    t.skip("the rho worktree (tests/suites/programs) is not present — npm-consumer install");
    return;
  }
  cliTools(); // boot + wasmtime + wat2wasm (builds boot on demand)
  await warmCompiler();

  const files = readdirSync(join(pkgDir, "..", "..", "tests", "suites", "programs"))
    .filter((f) => f.endsWith(".rho"))
    .sort();
  assert.equal(files.length, 100, `the programs tier pins 100 cases (found ${files.length})`);

  const results = [];
  const CONCURRENCY = 6;
  let cursor = 0;
  async function worker() {
    for (;;) {
      const i = cursor++;
      if (i >= files.length) return;
      const name = files[i].replace(/\.rho$/, "");
      const src = readFileSync(join(pkgDir, "..", "..", "tests", "suites", "programs", files[i]), "utf8");
      results.push({ name, src, ...(await parityOne(name, src)) });
    }
  }
  await Promise.all(Array.from({ length: CONCURRENCY }, worker));

  const identical = results.filter((r) => r.status === "identical");
  const red = results.filter((r) => r.status !== "identical");
  const sets = results.filter((r) => r.goldens.sets.length > 0);

  console.log(`parity: ${identical.length} byte-identical, ${red.length} red`);
  if (sets.length) {
    console.log(`  NOTE: ${sets.length} program(s) carry // set: headers — they rode the comparison verbatim`);
  }
  for (const r of red) {
    console.log(`  RED ${r.name}: ${r.detail}`);
  }
  assert.equal(red.length, 0, `byte parity failed for: ${red.map((r) => r.name).join(", ")}`);
  assert.equal(identical.length, files.length, "every programs-tier case is classified exactly once");
});

async function parityOne(name, src) {
  const goldens = parseGoldens(src);
  if (goldens.sets.length > 0) {
    // none exists today; if one lands, the harness must be taught the
    // // set: mechanism (boot's --set) before its bytes may be judged
    return { status: "red", detail: "carries // set: headers — extend the harness first", goldens };
  }
  const cli = cliReference(src);
  if (!cli.ok) {
    return { status: "red", detail: `CLI reference path failed (rc=${cli.rc}): ${cli.stderr.slice(0, 200)}`, goldens };
  }
  let modules = null;
  try {
    modules = collectModulesFor(join(pkgDir, "..", "..", "tests", "suites", "programs", name + ".rho"), src);
  } catch (e) {
    return { status: "red", detail: `module collection failed: ${String(e.message).slice(0, 200)}`, goldens };
  }
  const plugin = await compileRho(src, { modules });
  if (!plugin.ok) {
    return { status: "red", detail: `plugin compile failed: ${plugin.stderr.slice(0, 200)}`, goldens };
  }
  if (Buffer.compare(Buffer.from(plugin.bytes), cli.bytes) !== 0) {
    return {
      status: "red",
      detail: `bytes differ (plugin ${plugin.bytes.length} vs CLI ${cli.bytes.length})`,
      goldens,
    };
  }
  return { status: "identical", detail: `${plugin.bytes.length} bytes`, goldens };
}
