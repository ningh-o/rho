// Drive the real playground with STDIN: feed the input box two lines and
// verify read_line consumes them in order, then hits EOF. The program
// source rides a base64 blob so the probe has no escaping pitfalls.
import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

const RHO_B64 = "Zm4gbWFpbigpIC0+IGkzMiB7CiAgbGV0IGE6IHN0cmluZyA9IHJlYWRfbGluZSgpOwogIGxldCBiOiBzdHJpbmcgPSByZWFkX2xpbmUoKTsKICBsZXQgYzogc3RyaW5nID0gcmVhZF9saW5lKCk7CiAgbGV0IG11dCBtYXJrOiBzdHJpbmcgPSAiZW9mIjsKICBpZiBjICE9ICIiIHsKICAgIG1hcmsgPSAicmVhZCI7CiAgfQogIHByaW50ZigiYT17fSBiPXt9IGM9e31cbiIsIGEsIGIsIG1hcmspOwogIHJldHVybiAwOwp9Cg==";

const root = process.cwd();
const chrome =
  process.env.CHROME_BIN ||
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
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

spawn(chrome, [
  "--headless=new", "--no-first-run", "--remote-debugging-port=9345",
  "--user-data-dir=" + resolve("build/chrome-test/stdin-profile"), "about:blank",
], { stdio: "ignore" });
await new Promise((r) => setTimeout(r, 2500));
const list = await (await fetch("http://127.0.0.1:9345/json/list")).json();
const ws = new WebSocket(list.find((t) => t.type === "page").webSocketDebuggerUrl);
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

const setup = await evaljs(`(function(){
  document.getElementById('stdin').value = 'Ada\\nsecond line\\n';
  document.getElementById('stdin').dispatchEvent(new Event('input'));
  window.__rhoEditor.setDoc(decodeURIComponent(escape(atob('${RHO_B64}'))));
  return window.__rhoEditor.view.state.doc.length;
})()`);
console.log("setDoc bytes:", setup);
await evaljs(`document.getElementById('run').click(); true`);
for (let i = 1; i <= 20; i++) {
  await new Promise((r) => setTimeout(r, 1000));
  const st = await evaljs(`JSON.stringify({
    out: document.getElementById('output').textContent,
    status: document.getElementById('status').textContent,
  })`);
  const s = JSON.parse(st);
  if (s.out.trim() && /exit/i.test(s.status)) {
    console.log(`t=${i}s ${s.status} out="${s.out.trim()}"`);
    const pass = s.out.includes("a=Ada b=second line c=eof");
    console.log(pass ? "STDIN PASS" : "STDIN FAIL");
    process.exit(pass ? 0 : 1);
  }
}
console.log("TIMEOUT");
process.exit(1);
