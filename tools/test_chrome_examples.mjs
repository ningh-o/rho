// The playground's law, measured in a REAL browser: every example in
// site/assets/examples.js is compiled and run by headless Chrome itself,
// through the exact assets the site ships (site/assets/wasi.js +
// examples.js + rho.wasm) — not through a node re-implementation of the
// pipeline. tools/verify_examples.mjs covers the same examples against
// the same asset in node; this one exists because the browser's JS
// engine, stack limits and WebAssembly embedding are the deployment
// condition, and only Chrome can grade that.
//
//   node tools/test_chrome_examples.mjs [compiler.wasm]
//
// Mechanism: headless Chrome with --remote-debugging-port; this script
// speaks the DevTools protocol over the WebSocket that node ships
// (global WebSocket — no puppeteer, no npm) and Runtime.evaluate's one
// async function in the page. The function imports the site's own
// modules, compiles and runs every example, and returns the results as
// a plain value.
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

const compilerPath = resolve(process.argv[2] || "site/assets/rho.wasm");
const root = process.cwd();
// CHROME_BIN overrides the binary; without a Chrome the tool refuses
// (exit 2) rather than passing vacuously — a skipped browser test is a
// red in disguise
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
if (!existsSync(chrome)) {
  console.log(`chrome harness FAIL: no Chrome at ${chrome} (set CHROME_BIN)`);
  process.exit(2);
}
const work = "build/chrome-test/profile";

// the playground's own code path, as one async function: compile each
// example with the shipped rho.wasm, run the artifact, match `expect`
// (when the example carries one) against stdout — exactly what the
// picker does in the real UI. Built after the server binds so the port
// can be baked into the module URLs (about:blank has no base URL).
const harnessExpr = (port) => `
(async () => {
  const base = "http://127.0.0.1:${port}";
  const { runWasm, createFS } = await import(base + "/site/assets/wasi.js");
  const { EXAMPLES } = await import(base + "/site/assets/examples.js");
  const bytes = new Uint8Array(await (await fetch(base + "/site/assets/rho.wasm")).arrayBuffer());
  const rows = [];
  let fail = 0;
  for (const ex of EXAMPLES) {
    try {
      const fs = createFS();
      fs.write("/main.rho", new TextEncoder().encode(ex.code));
      const res = await runWasm(bytes, {
        args: ["rho", "build", "/main.rho", "--target", "wasm32-wasi", "-o", "/out.wasm"],
        fs,
      });
      const program = fs.read("/out.wasm");
      if (!program || res.exitCode !== 0) {
        fail++;
        rows.push({ id: ex.id, ok: false, why: "compile: " + ((res.stderr || "").split("\\n")[0] || "no artifact") });
        continue;
      }
      const run = await runWasm(program, {});
      const ok = ex.expect == null || run.stdout === ex.expect;
      if (!ok) fail++;
      rows.push({ id: ex.id, ok, exit: run.exitCode, stdout: run.stdout });
    } catch (e) {
      fail++;
      rows.push({ id: ex.id, ok: false, why: String(e) });
    }
  }
  return { pass: rows.length - fail, fail, rows };
})()
`;

// a static server over the repo root (the page reads /site/assets/*);
// an explicit compiler argument is served at the asset's path, so any
// artifact can stand in for the shipped one (e.g. the web chain's
// grandchild)
const server = createServer(async (req, res) => {
  if (process.env.RHO_CHROME_DEBUG) console.error(`[srv] ${req.method} ${req.url}`);
  const url = new URL(req.url, "http://127.0.0.1").pathname;
  if (url === "/site/assets/rho.wasm" && process.argv[2]) {
    const body = await readFile(compilerPath);
    res.writeHead(200, { "content-type": "application/wasm" }).end(body);
    return;
  }
  const path = join(root, decodeURIComponent(url));
  if (!resolve(path).startsWith(root)) {
    res.writeHead(403).end();
    return;
  }
  try {
    const body = await readFile(path);
    const type = path.endsWith(".html") ? "text/html" : path.endsWith(".js") ? "text/javascript" : path.endsWith(".wasm") ? "application/wasm" : "application/octet-stream";
    res.writeHead(200, { "content-type": type }).end(body);
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((res) => server.listen(0, "127.0.0.1", res));
const port = server.address().port;
const expr = harnessExpr(port);

rmSync(work, { recursive: true, force: true });
mkdirSync(work, { recursive: true });
// the starting page must BE the server's origin AND a real page: an
// opaque origin (about:blank) fails module fetches on CORS, and a
// failed navigation leaves an error page whose context blocks them too
writeFileSync(
  join(work, "blank.html"),
  "<!doctype html><title>rho chrome harness</title>",
);
const args = [
  "--headless=new",
  "--disable-gpu",
  "--no-first-run",
  "--no-default-browser-check",
  `--user-data-dir=${join(root, work)}`,
  `--remote-debugging-port=0`,
  `http://127.0.0.1:${port}/${work}/blank.html`,
];

const child = spawn(chrome, args, { stdio: ["ignore", "ignore", "pipe"] });
const kill = setTimeout(() => child.kill("SIGKILL"), 240_000);

// the chosen debug port lands on stderr: "DevTools listening on ws://…"
const wsUrl = await new Promise((res, rej) => {
  let err = "";
  const t = setTimeout(() => rej(new Error("chrome never announced its debug port")), 30_000);
  child.stderr.on("data", (d) => {
    err += d;
    const m = err.match(/DevTools listening on (ws:\/\/\S+)/);
    if (m) {
      clearTimeout(t);
      res(m[1]);
    }
  });
  child.on("exit", (c) => rej(new Error(`chrome exited ${c} before announcing: ${err}`)));
});

// the browser endpoint → the page target's webSocketDebuggerUrl
const dbg = new URL(wsUrl.replace(/^ws/, "http"));
dbg.pathname = "/json/list";
const res0 = await fetch(dbg);
const targets = await res0.json();
const page = targets.find((t) => t.type === "page");

const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((res, rej) => {
  ws.onopen = res;
  ws.onerror = () => rej(new Error("ws error"));
});

let seq = 0;
const pending = new Map();
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
  }
};
const send = (method, params = {}) =>
  new Promise((res) => {
    const id = ++seq;
    pending.set(id, res);
    ws.send(JSON.stringify({ id, method, params }));
  });

try {
  await send("Runtime.enable");
  const r = await send("Runtime.evaluate", {
    expression: expr,
    awaitPromise: true,
    returnByValue: true,
  });
  if (r.result?.exceptionDetails) {
    const d = r.result.exceptionDetails;
    console.log(`chrome harness FAIL: page threw: ${d.text} ${d.exception?.description ?? ""}`);
    process.exit(1);
  }
  const out = r.result?.result?.value;
  for (const row of out.rows) {
    if (row.ok) console.log(`ok   ${row.id}`);
    else console.log(`FAIL ${row.id}: ${row.why ?? `exit ${row.exit}, stdout ${JSON.stringify(row.stdout)}`}`);
  }
  console.log(`\n${out.fail === 0 ? "headless chrome ok" : "headless chrome FAIL"}: ${out.pass} ran, ${out.fail} failed`);
  process.exit(out.fail === 0 ? 0 : 1);
} finally {
  clearTimeout(kill);
  ws.close();
  child.kill("SIGTERM");
  server.close();
}
