// Screenshots of the two interactive-stdin scenes for eyeballing:
// the tutorial's greet card mid-run (terminal row up) and the playground's
// output pane with the terminal row. Saves /tmp/rho-stdin-*.png.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";

const root = process.cwd();
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const DEBUG_PORT = Number(process.env.DEBUG_PORT || 9347);

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

const profile = mkdtempSync(join(tmpdir(), "rho-shot-"));
const chromeProc = spawn(chrome, [
  "--headless=new", "--no-first-run", `--remote-debugging-port=${DEBUG_PORT}`,
  "--window-size=1280,1400",
  `--user-data-dir=${profile}`, "about:blank",
], { stdio: "ignore" });
process.on("exit", () => chromeProc.kill());
for (let up = false, t0 = Date.now(); !up && Date.now() - t0 < 30000; ) {
  try { await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json(); up = true; }
  catch { await new Promise((r) => setTimeout(r, 500)); }
}
const list = await (await fetch(`http://127.0.0.1:${DEBUG_PORT}/json/list`)).json();
const ws = new WebSocket(list.find((t) => t.type === "page").webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0;
const pending = new Map();
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const send = (method, params = {}) => new Promise((r) => {
  const mid = ++id; pending.set(mid, r); ws.send(JSON.stringify({ id: mid, method, params }));
});
const ev = async (x) => (await send("Runtime.evaluate", { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (expr, timeoutMs = 60000) => {
  const t0 = Date.now();
  for (;;) {
    if (await ev(expr)) return true;
    if (Date.now() - t0 > timeoutMs) return false;
    await sleep(250);
  }
};
const shot = async (name) => {
  const r = await send("Page.captureScreenshot", { format: "png" });
  writeFileSync(`/tmp/rho-stdin-${name}.png`, Buffer.from(r.result.data, "base64"));
};

await send("Page.enable");
await send("Runtime.enable");

// the tutorial greet card mid-run
await send("Page.navigate", { url: base + "/tutorial.html" });
await sleep(1500);
await until(`document.querySelectorAll('.example .ed .cm-content').length >= 10`);
await ev(`document.querySelector('#ch-input').scrollIntoView({ block: 'center' })`);
await sleep(300);
const btn = await ev(`(() => {
  const el = document.querySelector('#ch-input .bar .run');
  const r = el.getBoundingClientRect();
  return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) };
})()`);
// real-input correctness is the other probe's job — here we just need the
// scene, so click through the DOM directly
await ev(`document.querySelector('#ch-input .bar .run').click()`);
await until(`!!document.querySelector('#ch-input .out .term-line input')`);
await sleep(400);
await shot("tutorial-terminal");
// type the name, capture the finished state too
await send("Input.insertText", { text: "rho" });
await send("Input.dispatchKeyEvent", { type: "keyDown", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13 });
await send("Input.dispatchKeyEvent", { type: "keyUp", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13 });
await until(`document.querySelector('#ch-input .out').textContent.includes('hello, rho')`);
await sleep(300);
await ev(`document.querySelector('#ch-input').scrollIntoView({ block: 'center' })`);
await sleep(300);
await shot("tutorial-greeted");

// the playground mid-read
await send("Page.navigate", { url: base + "/playground.html" });
await sleep(1500);
await until(`document.getElementById('lspchip').textContent.includes('ready')`);
await ev(`(() => {
  const sel = document.getElementById('example');
  sel.value = 'greet';
  sel.dispatchEvent(new Event('change'));
})()`);
await until(`!!document.querySelector('#output .term-line input')`);
await sleep(400);
await shot("playground-terminal");

console.log("shots written");
ws.close();
server.close();
process.exit(0);
