// Playground wiring: editor with highlight overlay, compile + run pipeline,
// example picker, localStorage persistence, shareable URL hash.

import { initCompiler, compile, runProgram } from "./compiler.js";
import { highlightRho } from "./highlight.js";
import { EXAMPLES, STARTER } from "./examples.js";

const ta = document.getElementById("input");
const hl = document.getElementById("highlight");
const out = document.getElementById("output");
const status = document.getElementById("status");
const runBtn = document.getElementById("run");
const exampleSel = document.getElementById("example");
const stats = document.getElementById("stats");

const LS_KEY = "rho-playground-source";
const LS_EX = "rho-playground-example";

function codeFromHash() {
  if (location.hash.startsWith("#code=")) {
    try {
      return decodeURIComponent(escape(atob(decodeURIComponent(location.hash.slice(6)))));
    } catch {
      return null;
    }
  }
  return null;
}

function shareHash(code) {
  return "#code=" + encodeURIComponent(btoa(unescape(encodeURIComponent(code))));
}

function render() {
  const scrolled = ta.scrollTop;
  const left = ta.scrollLeft;
  hl.innerHTML = highlightRho(ta.value) + "\n";
  hl.scrollTop = scrolled;
  hl.scrollLeft = left;
}

function setStatus(html) {
  status.innerHTML = html;
}

function showOut(text, cls) {
  const span = document.createElement("span");
  if (cls) span.className = cls;
  span.textContent = text;
  out.appendChild(span);
}

let running = false;

async function doRun() {
  if (running) return;
  running = true;
  runBtn.disabled = true;
  out.textContent = "";
  setStatus("compiling…");
  const t0 = performance.now();
  try {
    const compiled = await compile(ta.value);
    if (compiled.stale) return;
    if (!compiled.ok) {
      showOut(compiled.stderr || "compilation failed", "err");
      setStatus(`<span class="bad">compile error</span> in ${compiled.ms.toFixed(0)} ms`);
      return;
    }
    const run = await runProgram(compiled.program);
    showOut(run.stdout);
    if (run.stderr) showOut(run.stderr, "err");
    const tail = [];
    if (run.stderr) tail.push("stderr");
    const exitNote =
      run.exitCode === 0
        ? `<span class="ok">exit 0</span>`
        : `<span class="bad">exit ${run.exitCode}</span>`;
    setStatus(
      `${exitNote} <span>·</span> compiled ${compiled.ms.toFixed(0)} ms <span>·</span> ran ${run.ms.toFixed(0)} ms <span>·</span> ${compiled.program.length.toLocaleString()} bytes`,
    );
  } catch (e) {
    showOut("runtime error: " + (e.message || e), "err");
    setStatus(`<span class="bad">failed</span> after ${(performance.now() - t0).toFixed(0)} ms`);
  } finally {
    running = false;
    runBtn.disabled = false;
    render();
  }
}

// populate the example picker
for (const ex of EXAMPLES) {
  const opt = document.createElement("option");
  opt.value = ex.id;
  opt.textContent = ex.title;
  exampleSel.appendChild(opt);
}

function loadCode(code, exId) {
  ta.value = code;
  render();
  localStorage.setItem(LS_KEY, code);
  if (exId) {
    localStorage.setItem(LS_EX, exId);
    exampleSel.value = exId;
  }
  history.replaceState(null, "", location.pathname);
}

exampleSel.addEventListener("change", () => {
  const ex = EXAMPLES.find((e) => e.id === exampleSel.value);
  if (!ex) return;
  loadCode(ex.code, ex.id);
  doRun();
});

runBtn.addEventListener("click", doRun);
ta.addEventListener("input", () => {
  render();
  localStorage.setItem(LS_KEY, ta.value);
  localStorage.removeItem(LS_EX);
});
ta.addEventListener("scroll", render);
ta.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    doRun();
  }
  if (e.key === "Tab") {
    e.preventDefault();
    const s = ta.selectionStart;
    ta.setRangeText("  ", s, ta.selectionEnd, "end");
    render();
  }
});

// initial content: ?code= hash beats ?example= beats localStorage beats starter
const fromHash = codeFromHash();
const params = new URLSearchParams(location.search);
const fromQuery = params.get("example");
const saved = localStorage.getItem(LS_KEY);
const savedEx = localStorage.getItem(LS_EX);

let initial = STARTER;
let initialEx = "";
if (saved && !fromHash) {
  initial = saved;
  initialEx = savedEx || "";
}
if (fromQuery && EXAMPLES.some((e) => e.id === fromQuery)) {
  initial = EXAMPLES.find((e) => e.id === fromQuery).code;
  initialEx = fromQuery;
}
if (fromHash != null) {
  initial = fromHash;
  initialEx = "";
}
ta.value = initial;
if (initialEx) exampleSel.value = initialEx;
render();

setStatus(`<span class="dim">loading the compiler…</span>`);
initCompiler()
  .then(() => {
    setStatus(`<span class="dim">ready — press <kbd>⌘</kbd><kbd>↵</kbd> to run</span>`);
    if (fromHash || fromQuery) doRun();
  })
  .catch((e) => {
    setStatus(`<span class="bad">could not load the compiler</span>`);
    showOut(String(e), "err");
  });
