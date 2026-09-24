// Interactive stdin probe: drive the REAL pages (tutorial + playground) in
// headless Chrome and exercise both stdin modes end to end —
//   1. tutorial greet example: Run → the terminal row appears → type a name,
//      Enter → the program resumes and greets it
//   2. EOF: run again, close stdin (Ctrl+D semantics) → "hello, stranger"
//   3. no-JSPI fallback: with WebAssembly.Suspending patched out, the same
//      run never shows the terminal row — reads see EOF immediately
//   4. playground: the same interactive read through the stdin box flow
// Run: node tools/probe_stdin_interactive.mjs
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { join } from "node:path";

const root = process.cwd();
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const DEBUG_PORT = Number(process.env.DEBUG_PORT || 9346);

const server = createServer(async (req, res) => {
  try {
    let p = req.url.split("?")[0];
    if (p === "/") p = "/playground.html";
    const body = await readFile(join(root, "site", p));
    const ext = p.split(".").pop();
    const mime = { html: "text/html", js: "text/javascript", css: "text/css", wasm: "application/wasm", svg: "image/svg+xml" }[ext] || "application/octet-stream";
    res.writeHead(200, { "content-type": mime });
    res.end(body);
  } catch {
    res.writeHead(404);
    res.end("nope");
  }
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = `http://127.0.0.1:${server.address().port}`;

// one fresh profile per run: a shared dir dies on Chrome's SingletonLock,
// and a shared chrome accumulates this probe's own seed scripts
const { mkdtempSync } = await import("node:fs");
const { tmpdir } = await import("node:os");
const profile = mkdtempSync(join(tmpdir(), "rho-stdin-"));
const chromeProc = spawn(chrome, [
  "--headless=new", "--no-first-run", `--remote-debugging-port=${DEBUG_PORT}`,
  "--window-size=1280,900",
  `--user-data-dir=${profile}`, "about:blank",
], { stdio: "ignore" });
process.on("exit", () => chromeProc.kill());
// the debug port answers when Chrome is truly up (cold starts exceed 2.5 s)
for (let up = false, t0 = Date.now(); !up && Date.now() - t0 < 30000; ) {
  try {
    await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json();
    up = true;
  } catch {
    await new Promise((r) => setTimeout(r, 500));
  }
}

const list = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json();
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
  if (r.result?.exceptionDetails) return { error: String(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text).slice(0, 300) };
  return r.result?.result?.value;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (expr, timeoutMs = 45000) => {
  const t0 = Date.now();
  for (;;) {
    if (await evaljs(expr)) return true;
    if (Date.now() - t0 > timeoutMs) return false;
    await sleep(250);
  }
};
const navigate = async (url) => {
  await send("Page.navigate", { url });
  await sleep(1500);
  // the first CDP mouse click after a navigation activates the target and
  // is swallowed — burn one on the page margin
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x: 3, y: 300, button: "left", clickCount: 1 });
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: 3, y: 300, button: "left", clickCount: 1 });
  await sleep(150);
};

await send("Page.enable");
await send("Runtime.enable");
// the no-JSPI fallback is gated on the URL (?nojspi) — addScriptToEvaluate
// scripts accumulate per target, so one gated script serves every scenario
await send("Page.addScriptToEvaluateOnNewDocument", {

});

const results = [];
const check = (name, ok, detail) => results.push({ name, ok, detail });
// click a real point (the buttons are ordinary DOM buttons; center-click)
const clickAt = async (sel, within) => {
  const pos = await evaljs(`(() => {
    const host = ${within ? `document.querySelector(${JSON.stringify(within)})` : "document"};
    const el = [...host.querySelectorAll(${JSON.stringify(sel)})].pop();
    if (!el) return null;
    el.scrollIntoView({ block: "center" });
    const r = el.getBoundingClientRect();
    return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) };
  })()`);
  if (!pos) return false;
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x: pos.x, y: pos.y, button: "left", clickCount: 1 });
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: pos.x, y: pos.y, button: "left", clickCount: 1 });
  return true;
};
// headless quirk: the first click(s) after a navigation can be swallowed —
// click again like a human would until the expected effect shows. The
// guard keeps the retries honest: a Run button mid-run means Stop, so it
// is only clicked while it still reads idle.
const clickUntil = async (sel, within, effect, guard, timeoutMs = 45000) => {
  const t0 = Date.now();
  for (;;) {
    if (await evaljs(effect)) return true;
    if (await evaljs(guard || "true")) await clickAt(sel, within);
    if (Date.now() - t0 > timeoutMs) return false;
    await sleep(700);
  }
};
const typeLine = async (text) => {
  await send("Input.insertText", { text });
  await send("Input.dispatchKeyEvent", { type: "keyDown", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13 });
  await send("Input.dispatchKeyEvent", { type: "keyUp", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13 });
};
// --- 1+2: the tutorial's greet example, interactive then EOF ---
await navigate(base + "/tutorial.html");
const jspi = await evaljs(`String(typeof WebAssembly.Suspending) + "/" + typeof WebAssembly.promising`);
check("this Chrome speaks JSPI (otherwise the probe proves fallback only)", jspi === "function/function", jspi);
await until(`document.querySelectorAll('.example .ed .cm-content').length >= 10`, 30000);
await until(`window.WebAssembly && !!document.querySelector('.example .out')`);
check("tutorial renders the shared editor in every example",
  await evaljs(`[...document.querySelectorAll('.example .ed')].every(d => d.querySelector('.cm-editor'))`));

check("tutorial: greet Run suspends on its read (terminal row appears)",
  await clickUntil(".bar .run", "#ch-input .example",
    `!!document.querySelector('#ch-input .out .term-line input')`,
    `document.querySelector('#ch-input .bar .run').textContent === 'Run \u25b8'`));

// focus is already on the terminal input; type the name and send it
await typeLine("rho");
check("tutorial: the program resumes with the typed line",
  await until(`(() => { const t = document.querySelector('#ch-input .out').textContent;
    return t.includes("hello, rho") && t.includes("exit 0"); })()`, 20000),
  await evaljs(`document.querySelector('#ch-input .out').textContent`));

check("tutorial: EOF run greets the stranger",
  await clickUntil(".bar .run", "#ch-input .example",
    `!!document.querySelector('#ch-input .out .term-line input')`,
    `document.querySelector('#ch-input .bar .run').textContent === 'Run \u25b8'`) &&
  await clickUntil(".term-eof", "#ch-input .out",
    `!document.querySelector('#ch-input .out .term-line')`,
    `!!document.querySelector('#ch-input .out .term-line')`) &&
  await until(`(() => { const t = document.querySelector('#ch-input .out').textContent;
    return t.includes("hello, stranger") && t.includes("exit 0"); })()`, 20000),
  await evaljs(`document.querySelector('#ch-input .out').textContent`));

// (the no-JSPI fallback is pinned at the runWasm level by
// tools/test_interactive_stdin.mjs — a page-side seed cannot reach the
// worker's own global scope, so it is not simulated here)

// --- 4: the playground — interactive read with an EMPTY stdin box ---
await navigate(base + "/playground.html");
await evaljs(`localStorage.clear()`);
await navigate(base + "/playground.html");
await until(`!!document.querySelector('.cm-editor')`, 30000);
await until(`document.getElementById('lspchip').textContent.includes('ready')`, 90000);
// swap the editor to the greet example through the picker
await evaljs(`(() => {
  const sel = document.getElementById('example');
  const opt = [...sel.options].find(o => o.value === 'greet');
  if (!opt) return false;
  sel.value = 'greet';
  sel.dispatchEvent(new Event('change'));
  return true;
})()`);
check("playground: greet suspends and the terminal row lands in the output pane",
  await until(`(() => { const out = document.getElementById('output');
    return out && out.querySelector('.term-line input'); })()`, 60000));
await typeLine("rho");
check("playground: the answer flows and the run completes green",
  await until(`(() => { const o = document.getElementById('output');
    return o.textContent.includes("hello, rho") && document.getElementById('status').textContent.includes("exit 0"); })()`, 20000),
  await evaljs(`document.getElementById('output').textContent.slice(0, 120)`));

const failed = results.filter((r) => !r.ok);
for (const r of results) console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.ok ? "" : "  — " + r.detail}`);
console.log(failed.length === 0 ? `\nstdin probe: ${results.length}/${results.length} green` : `\nstdin probe: ${failed.length} failed`);
ws.close();
server.close();
process.exit(failed.length === 0 ? 0 : 1);
