// Drives the compiler wasm exactly the way the browser playground does,
// over every corpus program, and compares stdout/exit against the .out
// files. `node tools/test_browser_compiler.mjs [compiler.wasm]`
import { readFileSync, readdirSync } from "node:fs";
import { runWasm, createFS } from "../site/assets/wasi.js";

const compilerPath = process.argv[2] || "build/rho.wasm";
const compilerBytes = readFileSync(compilerPath);

async function rhoCompile(source) {
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  const wasiOpts = {
    args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
    fs,
    onStdout: () => {},
    onStderr: () => {},
  };
  const result = await runWasm(compilerBytes, wasiOpts);
  const program = fs.read("/out.wasm");
  return {
    ok: !!program && result.exitCode === 0,
    program,
    stderr: result.stderr,
    stdout: result.stdout,
    exitCode: result.exitCode,
  };
}

let pass = 0,
  fail = 0;
const dir = "corpus";
for (const file of readdirSync(dir).filter((f) => f.endsWith(".rho")).sort()) {
  const name = file.replace(/\.rho$/, "");
  const source = readFileSync(`${dir}/${file}`, "utf8");
  const wantExit = /exit:\s*(\d+)/.exec(source.split("\n")[0]);
  let wantOut = "";
  try {
    wantOut = readFileSync(`${dir}/${name}.out`, "utf8");
  } catch {
    // no .out file: the program prints nothing
  }
  const t0 = performance.now();
  const compiled = await rhoCompile(source);
  const tCompile = performance.now() - t0;
  if (!compiled.ok) {
    console.log(`FAIL ${name} (compile): ${compiled.stderr.split("\n")[0]}`);
    fail++;
    continue;
  }
  const t1 = performance.now();
  const run = await runWasm(compiled.program, {});
  const tRun = performance.now() - t1;
  const want = wantExit ? parseInt(wantExit[1], 10) : 0;
  const ok = run.stdout === wantOut && run.exitCode === want;
  if (ok) {
    pass++;
    console.log(
      `ok   ${name}  (${compiled.program.length}B wasm, compile ${tCompile.toFixed(0)}ms, run ${tRun.toFixed(0)}ms)`,
    );
  } else {
    fail++;
    console.log(
      `FAIL ${name}: exit ${run.exitCode} want ${want}; stdout ${JSON.stringify(run.stdout)} want ${JSON.stringify(wantOut)}`,
    );
  }
}
console.log(`\n${fail === 0 ? "browser pipeline ok" : "browser pipeline FAIL"}: ${pass} ran, ${fail} failed`);
process.exit(fail ? 1 : 0);
