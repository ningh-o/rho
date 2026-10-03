// parity.test.js — THE acceptance law (docs/ecosystem.md §2):
//
//   The plugin's wasm is byte-identical to the CLI's for every
//   programs-tier program (same compiler, same flags).
//
// Plugin side: the pinned artifact on the Node runtime face, the WAT
// assembled by the vendored wabt (the site's own vendor file). CLI
// side: the differential's mirror leg (tools/cli-reference.mjs). A program
// the plugin cannot compile through the app face — the corpus-package
// users, whose modules the app face has no runtime channel for — must
// refuse CLEANLY (the honest gap, asserted, never a wrong artifact).

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { compileRho, warmCompiler } from "../src/compiler.js";
import { pkgDir, repoAvailable, cliTools, cliReference, parseGoldens } from "../tools/cli-reference.mjs";

// the programs whose modules live in the corpus packages (geom/web/pk):
// the CLI bakes their module tree into MODS; the plugin's artifact
// bakes the std tree only and the app face reads no module tree at
// run time, so these refuse cleanly. The gap is the app face's
// runtime channel (libs/compiler/main.rho), not the compiler.
const PACKAGE_USERS = new Set([
  "032_pkg_test",
  "107_mod_structs_test",
  "t09_pub_use_forms_test",
]);

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
  assert.equal(files.length, 99, `the programs tier pins 99 cases (found ${files.length})`);

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
  const refusals = results.filter((r) => r.status === "expected-refusal");
  const red = results.filter((r) => r.status !== "identical" && r.status !== "expected-refusal");
  const sets = results.filter((r) => r.goldens.sets.length > 0);

  console.log(`parity: ${identical.length} byte-identical, ${refusals.length} expected refusals (package-module users), ${red.length} red`);
  if (refusals.length) {
    console.log(`  refusals: ${refusals.map((r) => r.name).join(", ")}`);
  }
  if (sets.length) {
    console.log(`  NOTE: ${sets.length} program(s) carry // set: headers — they rode the comparison verbatim`);
  }
  for (const r of red) {
    console.log(`  RED ${r.name}: ${r.detail}`);
  }
  assert.equal(red.length, 0, `byte parity failed for: ${red.map((r) => r.name).join(", ")}`);
  assert.equal(identical.length + refusals.length, files.length, "every programs-tier case is classified exactly once");
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
  const plugin = await compileRho(src);
  if (PACKAGE_USERS.has(name)) {
    if (plugin.ok) {
      return { status: "red", detail: "compiled but was expected to refuse (module channel gap)", goldens };
    }
    if (!plugin.stderr.includes("mods: unresolved module path(s)")) {
      return { status: "red", detail: `wrong refusal: ${plugin.stderr.slice(0, 200)}`, goldens };
    }
    return { status: "expected-refusal", detail: "package-module user refuses cleanly", goldens };
  }
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
