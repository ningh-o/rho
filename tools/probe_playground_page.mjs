// Drive the REAL playground page in headless Chrome: serve site/, open
// playground.html, put hello world in the editor, click Run, and report
// what the page shows and when — reproducing the user's exact path
// (worker + warm handshake + caps), not a direct module import.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

const root = process.cwd();
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";

const SLOW_KBPS = Number(process.env.SLOW_WASM_KBPS || 0); // throttle rho.wasm to simulate a slow network
const server = createServer(async (req, res) => {
  try {
    let p = req.url.split("?")[0];
    if (p === "/") p = "/playground.html";
    const body = await readFile(join(root, "site", p));
    const ext = p.split(".").pop();
    const mime = { html: "text/html", js: "text/javascript", css: "text/css", wasm: "application/wasm", svg: "image/svg+xml" }[ext] || "application/octet-stream";
    res.writeHead(200, { "content-type": mime });
    if (SLOW_KBPS && p === "/assets/rho.wasm") {
      const chunk = 8192;
      const delay = (chunk / 1024 / SLOW_KBPS) * 1000;
      let off = 0;
      const timer = setInterval(() => {
        if (off >= body.length) {
          clearInterval(timer);
          res.end();
          return;
        }
        res.write(body.subarray(off, off + chunk));
        off += chunk;
      }, delay);
      res.on("close", () => clearInterval(timer));
      return;
    }
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end("nope");
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const port = server.address().port;
const base = `http://127.0.0.1:${port}`;
console.log("serving", base);

const profile = "build/chrome-test/page-profile";
spawn(chrome, [
  "--headless=new", "--no-first-run", "--remote-debugging-port=9333",
  `--user-data-dir=${resolve(profile)}`, "about:blank",
], { stdio: "ignore" });
await new Promise((r) => setTimeout(r, 2500));

// find the page target
const list = await (await fetch("http://127.0.0.1:9333/json/list")).json();
const page = list.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0;
const pending = new Map();
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
  }
};
const send = (method, params = {}) =>
  new Promise((r) => {
    const mid = ++id;
    pending.set(mid, r);
    ws.send(JSON.stringify({ id: mid, method, params }));
  });
const evaljs = async (expr) => {
  const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.result?.exceptionDetails) return { error: r.result.exceptionDetails.text + " " + JSON.stringify(r.result.exceptionDetails.exception?.description || "").slice(0, 300) };
  return r.result?.result?.value;
};

await send("Page.enable");
await send("Runtime.enable");
const consoleLines = [];
ws.onmessage2 = null;
// collect console + exceptions via a wrapper listener
const origOn = ws.onmessage;
ws.addEventListener?.("message", () => {});
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m);
    pending.delete(m.id);
    return;
  }
  if (m.method === "Runtime.consoleAPICalled" || m.method === "Runtime.exceptionThrown" || m.method === "Log.entryAdded") {
    const entry = m.params?.entry ?? m.params;
    consoleLines.push((m.method + ": " + JSON.stringify(entry).slice(0, 220)));
  }
};

await send("Page.navigate", { url: base + "/playground.html" });
await new Promise((r) => setTimeout(r, process.env.SLOW_WASM_KBPS ? 1200 : 3000)); // slow mode: click while the download is still running

// put hello world in the editor and click Run — the real controls
const setup = await evaljs(`(function(){
  const ta = document.getElementById('input');
  ta.value = 'fn main() -> i32 {\\n  printf("hello, world\\\\n");\\n  return 0;\\n}\\n';
  ta.dispatchEvent(new Event('input'));
  return { hasRun: !!document.getElementById('run'), status: document.getElementById('status').textContent };
})()`);
console.log("setup:", JSON.stringify(setup));

const t0 = Date.now();
await evaljs(`document.getElementById('run').click(); true`);
// poll the page state every second up to 35s
for (let i = 1; i <= (process.env.SLOW_WASM_KBPS ? 60 : 35); i++) {
  await new Promise((r) => setTimeout(r, 1000));
  const st = await evaljs(`(function(){
    return JSON.stringify({
      status: document.getElementById('status').textContent,
      out: document.getElementById('output').textContent.slice(0, 200),
      btn: document.getElementById('run').textContent,
      disabled: document.getElementById('run').disabled,
    });
  })()`);
  const s = typeof st === "string" ? JSON.parse(st) : st;
  console.log(`t=${i}s btn=${s.btn} disabled=${s.disabled} status="${s.status}" out="${s.out.trim().slice(0, 80)}"`);
  const doneNow = s.out.trim() || /exit|stopped|error|failed/i.test(s.status);
  if (doneNow) break;
}
if (consoleLines.length) {
  console.log("console/exceptions:");
  for (const l of consoleLines.slice(0, 8)) console.log("  ", l);
}
const total = ((Date.now() - t0) / 1000).toFixed(1);
console.log(`total ${total}s`);
process.exit(0);
