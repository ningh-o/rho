import { readFileSync } from "node:fs";
import { runWasm, createFS } from "../site/assets/wasi.js";
import { EXAMPLES } from "../site/assets/examples.js";

const compilerBytes = readFileSync("build/rho-boot.wasm");
async function rhoCompile(source) {
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  const result = await runWasm(compilerBytes, {
    args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
    fs,
  });
  return { ok: !!fs.read("/out.wasm") && result.exitCode === 0, program: fs.read("/out.wasm"), stderr: result.stderr };
}
let bad = 0;
for (const ex of EXAMPLES) {
  const c = await rhoCompile(ex.code);
  if (!c.ok) {
    bad++;
    console.log(`FAIL ${ex.id}: ${c.stderr.split("\n")[0]}`);
    continue;
  }
  const run = await runWasm(c.program, {});
  if (ex.expect != null && run.stdout !== ex.expect) {
    bad++;
    console.log(`FAIL ${ex.id}: stdout ${JSON.stringify(run.stdout)} want ${JSON.stringify(ex.expect)} (exit ${run.exitCode})`);
    continue;
  }
  console.log(`ok ${ex.id} (exit ${run.exitCode})`);
}
process.exit(bad ? 1 : 0);
