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
await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
const workerSessions = [];
const origOnmessage = ws.onmessage;
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.method === "Target.attachedToTarget") {
    const sid = m.params.sessionId;
    workerSessions.push(sid);
    send("Runtime.enable", { sessionId: sid });
    return;
  }
  if (m.method === "Runtime.consoleAPICalled" && workerSessions.includes(m.sessionId)) {
    const vals = m.params.args.map(a => a.value ?? a.description ?? "");
    if (vals.some(v => String(v).includes("second-bytes"))) {
      const bufArg = m.params.args.find(a => a.objectId);
      if (bufArg) consoleLines.push("WORKER-SECOND-BYTES-OBJ: " + bufArg.objectId);
    }
    consoleLines.push("WORKER-LOG: " + JSON.stringify(vals).slice(0, 300));
    return;
  }
  if (false) {
    consoleLines.push("WORKER-LOG: " + JSON.stringify(m.params.args.map(a => a.value ?? a.description ?? "")).slice(0, 300));
    return;
  }
  if (m.method === "Runtime.exceptionThrown" && workerSessions.includes(m.sessionId)) {
    consoleLines.push("WORKER-EXC: " + JSON.stringify(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text).slice(0, 300));
    return;
  }
  origOnmessage(e);
};
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
    consoleLines.push((m.method + ": " + JSON.stringify(entry).slice(0, 400)));
  }
};

await send("Page.navigate", { url: base + "/playground.html" });
await new Promise((r) => setTimeout(r, process.env.SLOW_WASM_KBPS ? 1200 : 3000)); // slow mode: click while the download is still running

  await new Promise((r) => setTimeout(r, 3000)); // let the page modules load
// diagnose: compile the STARTER in-page (worker-free) and ship the bytes
const diag = await evaljs(`(async () => {
  const { STARTER } = await import('./assets/examples.js');
  const { compile } = await import('./assets/compiler.js');
  const r = await compile(STARTER);
  if (!r.ok) return JSON.stringify({ ok: false, stderr: r.stderr.slice(0, 200) });
  const b = new Uint8Array(r.program);
  let bin = '';
  for (let i = 0; i < b.length; i += 32768) bin += String.fromCharCode.apply(null, b.subarray(i, i + 32768));
  return JSON.stringify({ ok: true, len: b.length, b64: btoa(bin) });
})()`);
const d = JSON.parse(diag);
console.log('starter diag: ok=' + d.ok + ' len=' + (d.len ?? d.stderr ?? ''));
  // full worker-flow replica on the main thread: does V8 reject the bytes?
  const flow = await evaljs(`(async () => {
    const { STARTER } = await import('./assets/examples.js');
    const { initCompiler, compile, runProgram } = await import('./assets/compiler.js');
    await initCompiler();
    const r = await compile(STARTER);
    if (!r.ok) return JSON.stringify({ stage: 'compile', stderr: r.stderr.slice(0, 150) });
    try {
      await WebAssembly.compile(r.program);
    } catch (e) {
      return JSON.stringify({ stage: 'validate', err: String(e).slice(0, 150) });
    }
    const run = await runProgram(r.program);
    return JSON.stringify({ stage: 'ran', stdout: run.stdout.slice(0, 60), exit: run.exitCode });
  })()`);
  console.log('main-thread flow:', flow);
if (d.ok) {
  (await import('node:fs')).writeFileSync('build/probe/starter-page.wasm', Buffer.from(d.b64, 'base64'));
}
// fmt: click Format and verify the doc was rewritten (messy -> canonical)
const fmtRes = await evaljs(`(function(){
  const rhoKeys = Object.keys(window).filter(k => k.toLowerCase().includes('rho'));
  return JSON.stringify({ has: typeof window.__rhoEditor, rhoKeys });
})()`);
console.log('handle check:', fmtRes);
const fmtRes2 = await evaljs(`(function(){
  const doc0 = window.__rhoEditor.view.state.doc.toString();
  window.__rhoEditor.setDoc(decodeURIComponent(escape(atob('Zm4gbWFpbigpIC0+IGkzMiB7CmxldCB4PTE7CnJldHVybiAwOwp9Cg=='))));
  return true;
})()`);
console.log('fmt setup:', fmtRes);
  if (consoleLines.length) { console.log('PAGE EXCEPTIONS:'); consoleLines.forEach(c => console.log('  ', c.slice(0, 300))); }
await evaljs(`document.getElementById('fmt').click(); true`);
let fmtOk = false;
for (let i = 1; i <= 10; i++) {
  await new Promise((r) => setTimeout(r, 500));
  const doc = await evaljs(`window.__rhoEditor.view.state.doc.toString()`);
  if (typeof doc === 'string' && doc.includes('let x = 1;')) { fmtOk = true; console.log('fmt ok at', i * 0.5, 's:', JSON.stringify(doc.slice(0, 60))); break; }
}
if (!fmtOk) console.log('FMT CHECK: not applied in 5s');

// live diagnostics: set a type-broken doc, expect squiggles/gutter markers
const lintSet = await evaljs(`(function(){
  window.__rhoEditor.setDoc(decodeURIComponent(escape(atob('Zm4gbWFpbigpIC0+IGkzMiB7CiAgbGV0IHg6IGkzMiA9ICJubyI7CiAgcmV0dXJuIDA7Cn0K'))));
  return true;
})()`);
let lintSeen = false;
for (let i = 1; i <= 12; i++) {
  await new Promise((r) => setTimeout(r, 500));
  const has = await evaljs(`!!document.querySelector('.cm-lintRange-error, .cm-lint-marker')`);
  if (has) { lintSeen = true; console.log('lint markers at', i * 0.5, 's'); break; }
}
if (!lintSeen) console.log('LINT CHECK FAILED: no markers');

// put hello world in the editor and click Run — the real controls
const setup = await evaljs(`(function(){
  window.__rhoEditor.setDoc(decodeURIComponent(escape(atob('Zm4gbWFpbigpIC0+IGkzMiB7CiAgcHJpbnRmKCJoZWxsbywgd29ybGRcbiIpOwogIHJldHVybiAwOwp9Cg=='))));
  ta.value = 'fn main() -> i32 {\\n  printf("hello, world\\\\n");\\n  return 0;\\n}\\n';
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
      out: document.getElementById('output').textContent,
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
