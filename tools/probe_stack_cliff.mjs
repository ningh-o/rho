// Probe the browser pipeline's stack cliff: compile synthetic programs of
// increasing expression depth through the same runWasm path the playground
// uses, and report where the compile starts failing (call stack exhausted).
import { readFileSync } from "node:fs";
import { runWasm, createFS } from "../site/assets/wasi.js";

const compilerPath = process.argv[2] || "build/rho.wasm";
const compilerBytes = readFileSync(compilerPath);

async function compileOnly(source) {
  const fs = createFS();
  fs.write("/main.rho", new TextEncoder().encode(source));
  try {
    const result = await runWasm(compilerBytes, {
      args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
      fs,
      onStdout: () => {},
      onStderr: () => {},
    });
    return { ok: result.exitCode === 0, stderr: result.stderr, exitCode: result.exitCode };
  } catch (e) {
    const msg = String(e?.message ?? e);
    const frames = [...msg.matchAll(/wasm-function\[(\d+)\]/g)].map((m) => m[1]);
    const top = frames.sort((a, b) => frames.filter((x) => x === a).length - frames.filter((x) => x === b).length).pop();
    return { ok: false, stderr: msg, exitCode: -1, trap: msg.includes("stack"), topFn: top };
  }
}

function chain(n) {
  const terms = new Array(n).fill("1").join(" + ");
  return `fn main() -> i32 {\n  let x: i32 = ${terms};\n  printf("{}\\n", x);\n  return 0;\n}\n`;
}
function parens(n) {
  return `fn main() -> i32 {\n  let x: i32 = ${"(".repeat(n)}1${")".repeat(n)};\n  printf("{}\\n", x);\n  return 0;\n}\n`;
}
function stmts(n) {
  const body = new Array(n).fill("  x = x + 1;").join("\n");
  return `fn main() -> i32 {\n  let mut x: i32 = 0;\n${body}\n  printf("{}\\n", x);\n  return 0;\n}\n`;
}

const suites = [
  ["chain", chain],
  ["parens", parens],
  ["stmts", stmts],
];
for (const [name, gen] of suites) {
  let last = { ok: true };
  const marks = [];
  for (const n of [50, 100, 200, 400, 800, 1600, 3200]) {
    const r = await compileOnly(gen(n));
    marks.push(`${n}:${r.ok ? "ok" : r.trap ? "STACK" : "FAIL"}${r.topFn ? "(fn" + r.topFn + ")" : ""}`);
    last = r;
    if (!r.ok) break;
  }
  console.log(`${name}: ${marks.join(" ")}${last.ok ? "" : "  last-stderr: " + last.stderr.split("\n")[0].slice(0, 90)}`);
}
