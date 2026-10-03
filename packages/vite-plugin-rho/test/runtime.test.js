// runtime.test.js — the package's own integrity: the generation pin is
// the single source of truth, the artifact matches it, the vendored
// wabt is the file the generation hashes, and the runtime face speaks
// all three verbs (build / check / fmt) with the CLI's semantics.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { loadGeneration, compileRho, warmCompiler, runProgram } from "../src/compiler.js";

const pkgDir = join(dirname(fileURLToPath(import.meta.url)), "..");

test("generation pin: the artifact on disk is the pinned artifact", () => {
  const gen = loadGeneration();
  const artifact = readFileSync(join(pkgDir, "assets", gen.artifact));
  const sha = createHash("sha256").update(artifact).digest("hex");
  assert.equal(sha, gen.artifactSha256, "assets/" + gen.artifact + " must match generation.json's sha256");
  assert.equal(artifact.length, gen.artifactBytes);
  assert.match(gen.rhoCommit, /^[0-9a-f]{40}$/, "the pin carries the compiler's commit");
  // the vendored assembler the generation hashes is the file we load
  const wabt = readFileSync(join(pkgDir, gen.wabt.vendor));
  assert.equal(createHash("sha256").update(wabt).digest("hex"), gen.wabt.sha256);
});

test("runtime face: build compiles a std-using program and the program runs", async () => {
  await warmCompiler();
  const src = `use std.io;
fn main() -> i32 {
  printf("hi, {}\\n", "plugin");
  return 0;
}
`;
  const r = await compileRho(src);
  assert.ok(r.ok, `compile: ${r.stderr}`);
  const run = await runProgram(r.bytes);
  assert.equal(run.stdout, "hi, plugin\n");
  assert.equal(run.exitCode, 0);
});

test("runtime face: a bad program refuses with the compiler's diagnostics", async () => {
  await warmCompiler();
  const r = await compileRho("fn main() -> i32 { return oops; }\n");
  assert.ok(!r.ok);
  assert.match(r.stderr, /check: unknown name 'oops'/);
  assert.match(r.stderr, /check: 1 error\(s\)/);
});

test("runtime face: check mode types without emitting", async () => {
  await warmCompiler();
  const ok = await compileRho("fn main() -> i32 { return 0; }\n", { mode: "check" });
  assert.ok(ok.ok);
  assert.equal(ok.bytes, null);
  const bad = await compileRho("fn main() -> i32 { return oops; }\n", { mode: "check" });
  assert.ok(!bad.ok);
  assert.match(bad.stderr, /check: unknown name 'oops'/);
});

test("runtime face: fmt mode returns the canonical formatting", async () => {
  await warmCompiler();
  const r = await compileRho("fn main() -> i32 {return 0;}", { mode: "fmt" });
  assert.ok(r.ok);
  assert.equal(r.wat, "fn main() -> i32 {\n  return 0;\n}\n");
});
