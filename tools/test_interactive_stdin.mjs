// Pins the runWasm stdin layers with the real compiler + the greet program:
// static redirect, EOF, the suspending provider (JSPI), a provider EOF, a
// two-line interactive read, and the no-JSPI fallback (a provider is
// configured but the embedder cannot suspend — it is ignored, reads see
// EOF, nothing hangs). Run: node tools/test_interactive_stdin.mjs
import { readFileSync } from "node:fs";
import { runWasm, createFS } from "../site/assets/wasi.js";
import { compile } from "../site/assets/compiler.js";

const compilerBytes = readFileSync("build/rho.wasm");
const enc = (s) => new TextEncoder().encode(s);

// compile() imports compiler.js which fetches rho.wasm relative to itself —
// in node there is no fetch, so drive the compiler through runWasm directly
async function build(source) {
  const fs = createFS();
  fs.write("/main.rho", enc(source));
  const r = await runWasm(compilerBytes, {
    args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
    fs,
  });
  if (r.exitCode !== 0) throw new Error("build failed: " + r.stderr);
  return fs.read("/out.wasm");
}

const GREET = `fn main() -> i32 {
  let name: string = read_line();
  if name == "" {
    printf("hello, stranger\\n");
    return 0;
  }
  printf("hello, {}\\n", name);
  return 0;
}
`;
const TWICE = `fn main() -> i32 {
  let a: string = read_line();
  let b: string = read_line();
  printf("hi {}\\nhi {}\\n", a, b);
  return 0;
}
`;

let failed = 0;
function check(name, ok, detail) {
  if (!ok) failed++;
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${ok ? "" : "  — " + detail}`);
}

const greet = await build(GREET);
const twice = await build(TWICE);

// 1 — static redirect
{
  const r = await runWasm(greet, { stdin: "World\n" });
  check("static stdin redirects", r.stdout === "hello, World\n" && r.exitCode === 0, JSON.stringify(r.stdout));
}
// 2 — empty stdin reads as EOF
{
  const r = await runWasm(greet, { stdin: null });
  check("empty stdin reads as EOF (the stranger)", r.stdout === "hello, stranger\n", JSON.stringify(r.stdout));
}
// 3 — the suspending provider hands over a line
if (typeof WebAssembly.Suspending === "function" && typeof WebAssembly.promising === "function") {
  let asks = 0;
  const r = await runWasm(greet, {
    stdin: null,
    stdinProvider: () => {
      asks++;
      return Promise.resolve(enc("rho\n"));
    },
  });
  check("interactive stdin resumes with the typed line",
    r.stdout === "hello, rho\n" && r.exitCode === 0 && asks === 1,
    JSON.stringify(r.stdout) + " asks=" + asks);
  // 4 — a provider EOF ends the read
  {
    let asks = 0;
    const r = await runWasm(greet, {
      stdin: null,
      stdinProvider: () => {
        asks++;
        return Promise.resolve(null);
      },
    });
    check("provider EOF greets the stranger",
      r.stdout === "hello, stranger\n" && asks === 1, JSON.stringify(r.stdout) + " asks=" + asks);
  }
  // 5 — two interactive reads in one run
  {
    const lines = ["ada\n", "grace\n"];
    const r = await runWasm(twice, {
      stdin: null,
      stdinProvider: () => Promise.resolve(enc(lines.shift())),
    });
    check("two interactive reads land in order",
      r.stdout === "hi ada\nhi grace\n", JSON.stringify(r.stdout));
  }
} else {
  console.log("SKIP  interactive legs (this node has no JSPI)");
}
// 6 — the no-JSPI fallback: a provider is configured but the embedder
// cannot suspend; it must be ignored (never called), reads see EOF, and
// the run completes instead of hanging
{
  const Suspending = WebAssembly.Suspending;
  const promising = WebAssembly.promising;
  // @ts-ignore — the whole point
  delete WebAssembly.Suspending;
  // @ts-ignore
  delete WebAssembly.promising;
  try {
    let called = 0;
    const r = await runWasm(greet, {
      stdin: null,
      stdinProvider: () => {
        called++;
        return Promise.resolve(enc("rho\n"));
      },
    });
    check("no-JSPI fallback ignores the provider and reads EOF",
      r.stdout === "hello, stranger\n" && called === 0,
      JSON.stringify(r.stdout) + " called=" + called);
  } finally {
    WebAssembly.Suspending = Suspending;
    WebAssembly.promising = promising;
  }
}

console.log(failed === 0 ? "\ninteractive stdin: all green" : `\ninteractive stdin: ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
