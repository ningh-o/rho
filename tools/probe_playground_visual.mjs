// Visual probe for the playground chrome: serve site/, open playground.html
// in headless Chrome with a SEEDED DRAFT (a program with a build-parameter
// error), and assert the family-law surface — the first check runs with no
// edit (squiggles on load), themed dark buttons (never the browser's white
// default), transparent gutters, the scrollbar law (invisible until hover,
// never a track), and the dark lint tooltip. Screenshots land in
// /tmp/rho-visual-*.png. Run: node tools/probe_playground_visual.mjs
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const root = process.cwd();
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const DEBUG_PORT = Number(process.env.DEBUG_PORT || 9344);

const BAD_DRAFT = 'const PI: f32 = 3.14;\nfn main() -> void { printf("hi\\n"); }\n';
const GOOD_PROGRAM = 'fn main() -> void {\n  printf("hello from the probe\\n");\n}\n';

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

spawn(chrome, [
  "--headless=new", "--no-first-run", `--remote-debugging-port=${DEBUG_PORT}`,
  "--window-size=1280,900",
  `--user-data-dir=${resolve("build/chrome-test/visual-profile")}`, "about:blank",
], { stdio: "ignore" });
await new Promise((r) => setTimeout(r, 2500));

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
const until = async (expr, timeoutMs = 30000) => {
  const t0 = Date.now();
  for (;;) {
    if (await evaljs(expr)) return true;
    if (Date.now() - t0 > timeoutMs) return false;
    await sleep(250);
  }
};
const shot = async (name) => {
  const r = await send("Page.captureScreenshot", { format: "png" });
  writeFileSync(`/tmp/rho-visual-${name}.png`, Buffer.from(r.result.data, "base64"));
};
const hover = async (x, y) => {
  await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
};

await send("Page.enable");
await send("Runtime.enable");
// seed the draft BEFORE the page runs: the first check must fire on load
await send("Page.addScriptToEvaluateOnNewDocument", {
  source: `localStorage.setItem('rho-playground-source', ${JSON.stringify(BAD_DRAFT)});`,
});
await send("Page.navigate", { url: base + "/playground.html" });

await until(`!!document.querySelector('.cm-editor')`);
// THE user question: diagnostics appear on first load, no edit required
const firstCheck = await until(`document.querySelectorAll('.cm-lintRange').length > 0`, 60000);
await until(`document.getElementById('lspchip').textContent.includes('ready')`, 60000);
await sleep(300);

const results = [];
const check = (name, ok, detail) => results.push({ name, ok, detail });
check("first check runs on load (squiggles without an edit)", firstCheck, `lintRange seen: ${firstCheck}`);

// --- ground truth: computed styles, not pixels ---
const styles = await evaljs(`(() => {
  const cs = (sel, props) => { const el = document.querySelector(sel); if (!el) return null;
    const s = getComputedStyle(el); return Object.fromEntries(props.map(p => [p, s[p]])); };
  return {
    fmt: cs('#fmt', ['backgroundColor', 'color', 'borderColor']),
    run: cs('#run', ['backgroundColor', 'color', 'borderColor']),
    gutters: cs('.cm-gutters', ['backgroundColor']),
    scroller: cs('.cm-scroller', ['scrollbarColor', 'scrollbarWidth']),
    stdinHead: !!document.querySelector('.stdin-head'),
    stdinPlaceholder: document.getElementById('stdin')?.placeholder || '',
  };
})()`);

check("no stdin head row (single caption)", styles.stdinHead === false, JSON.stringify(styles.stdinHead));
check("stdin placeholder carries the caption", /read_line/.test(styles.stdinPlaceholder), styles.stdinPlaceholder);
check("Format button is themed (transparent bg, copper ink)",
  styles.fmt.backgroundColor === "rgba(0, 0, 0, 0)" && styles.fmt.color === "rgb(232, 160, 76)",
  JSON.stringify(styles.fmt));
check("Run button is filled copper",
  styles.run.backgroundColor === "rgb(232, 160, 76)" && styles.run.color === "rgb(26, 18, 7)",
  JSON.stringify(styles.run));
check("gutters are transparent (no white strip)",
  styles.gutters.backgroundColor === "rgba(0, 0, 0, 0)",
  JSON.stringify(styles.gutters));
check("scrollbar invisible by default (the family law)",
  /rgba\(0, 0, 0, 0\)/.test(styles.scroller.scrollbarColor),
  JSON.stringify(styles.scroller));

await shot("default");

// --- hover the editor: the scrollbar reveals, no track ---
const edRect = await evaljs(`(() => { const r = document.querySelector('.cm-editor').getBoundingClientRect();
  return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) }; })()`);
await hover(edRect.x, edRect.y);
await sleep(200);
const hoverColor = await evaljs(`getComputedStyle(document.querySelector('.cm-scroller')).scrollbarColor`);
check("scrollbar thumb reveals on hover, never a track",
  hoverColor.startsWith("rgb(58, 58, 66)") && /rgba\(0, 0, 0, 0\)$/.test(hoverColor),
  hoverColor);

// --- hover the diagnostic: the lint tooltip must be dark chrome ---
const lintPos = await evaljs(`(() => {
  const r = document.querySelector('.cm-lintRange').getBoundingClientRect();
  return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) }; })()`);
await hover(lintPos.x, lintPos.y);
await sleep(900);
const tooltip = await evaljs(`(() => {
  const tip = document.querySelector('.cm-tooltip');
  if (!tip) return null;
  const s = getComputedStyle(tip);
  return { bg: s.backgroundColor, ink: s.color, text: (tip.textContent || '').slice(0, 80) };
})()`);
check("lint tooltip is dark chrome",
  !!tooltip && tooltip.bg === "rgb(23, 23, 27)" && tooltip.ink === "rgb(232, 230, 225)",
  JSON.stringify(tooltip));
await shot("tooltip");

// --- swap to a clean program and run: the full pipeline stays green ---
await evaljs(`window.__rhoEditor.setDoc(${JSON.stringify(GOOD_PROGRAM)});`);
await until(`document.querySelectorAll('.cm-lintRange').length === 0`, 15000);
await evaljs(`document.getElementById('run').click()`);
await until(`document.getElementById('status').textContent.includes('exit')`, 30000);
await sleep(200);
const runState = await evaljs(`({
  status: document.getElementById('status').textContent,
  out: (document.getElementById('output').textContent || '').slice(0, 40),
})`);
check("clean program runs green", runState.status.includes("exit 0") && /hello from the probe/.test(runState.out),
  JSON.stringify(runState));
await shot("after-run");

// --- narrow viewport: the single-column layout keeps the law ---
await send("Emulation.setDeviceMetricsOverride", { width: 568, height: 1100, deviceScaleFactor: 2, mobile: true });
await sleep(400);
await shot("narrow");
await send("Emulation.clearDeviceMetricsOverride");

const failed = results.filter((r) => !r.ok);
for (const r of results) console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.ok ? "" : "  — " + r.detail}`);
console.log(failed.length === 0 ? `\nvisual probe: ${results.length}/${results.length} green` : `\nvisual probe: ${failed.length} failed`);
ws.close();
server.close();
process.exit(failed.length === 0 ? 0 : 1);
