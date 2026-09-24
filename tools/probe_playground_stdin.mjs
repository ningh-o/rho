// Drive the real playground page with STDIN: set the input box, run the
// greet example, and verify the program read the fed line — the end-to-end
// grade for read_line + the shim's fd 0, in a real browser.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

const root = process.cwd();
const chrome = process.env.CHROME_BIN || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
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
const port = server.address().port;

spawn(chrome, ["--headless=new", "--no-first-run", "--remote-debugging-port=9339",
  "--user-data-dir=" + resolve("build/chrome-test/stdin-profile"), "about:blank"], { stdio: "ignore" });
await new Promise((r) => setTimeout(r, 2500));
const list = await (await fetch("http://127.0.0.1:9339/json/list")).json();
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
const send = (method, params = {}) => new Promise((r) => {
  const mid = ++id;
  pending.set(mid, r);
  ws.send(JSON.stringify({ id: mid, method, params }));
});
const evaljs = async (expr) => {
  const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
  return r.result?.result?.value;
};

await send("Page.enable");
await send("Page.navigate", { url: `http://127.0.0.1:${port}/playground.html` });
await new Promise((r) => setTimeout(r, 3000));

await evaljs(`(function(){
  document.getElementById('stdin').value = 'Ada\\nsecond line\\n';
  document.getElementById('stdin').dispatchEvent(new Event('input'));
  const ta = document.getElementById('input');
  ta.value = 'fn main() -> i32 {\\n  let a: string = read_line();\\n  let b: string = read_line();\\n  let c: string = read_line();\\n  printf("a={} b={} c-empty={}\\\\n", a, b, c == "");\\n  return 0;\\n}\\n';
  ta.dispatchEvent(new Event('input'));
  return true;
})()`);
await evaljs(`document.getElementById('run').click(); true`);
for (let i = 1; i <= 20; i++) {
  await new Promise((r) => setTimeout(r, 1000));
  const st = await evaljs(`JSON.stringify({
    out: document.getElementById('output').textContent,
    status: document.getElementById('status').textContent,
  })`);
  const s = JSON.parse(st);
  if (s.out.trim() || /exit|error/i.test(s.status)) {
    console.log(`t=${i}s out="${s.out.trim()}" status="${s.status}"`);
    process.exit(s.out.includes("a=Ada b=second") && s.out.includes("c-empty=true") ? 0 : 1);
  }
}
console.log("TIMEOUT: no output in 20s");
process.exit(1);
