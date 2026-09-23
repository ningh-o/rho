// Playground wiring: editor with highlight overlay, compile + run pipeline,
// example picker, localStorage persistence, shareable URL hash.

import { highlightRho } from "./highlight.js";
import { EXAMPLES, STARTER } from "./examples.js";
import { attachCompletion } from "./completion.js";

// Compile + run happen in a worker: a long-running program can then never
// freeze the page. The worker is terminated on Stop and after the caps.
const COMPILE_CAP_MS = 20000; // cold fetch + compiling the compiler itself
const RUN_CAP_MS = 10000;

let worker = null;
let workerId = 0;
let capTimer = null;
let capPhase = null;
let capStart = 0;

function stopWorker() {
  if (capTimer) {
    clearTimeout(capTimer);
    capTimer = null;
  }
  capPhase = null;
  if (worker) {
    worker.terminate();
    worker = null;
  }
}

function spawnWorker() {
  worker = new Worker("assets/worker.js", { type: "module" });
  worker.onmessage = (e) => onWorkerMessage(e.data);
  worker.onerror = (e) => onWorkerError(e);
  // warm immediately: fetch + instantiate the compiler now, so the first
  // Run doesn't pay for it
  worker.postMessage({ kind: "warm" });
  return worker;
}

function armCap(ms, phase) {
  if (capTimer) clearTimeout(capTimer);
  capPhase = phase;
  capStart = performance.now();
  capTimer = setTimeout(() => {
    const phaseLabel = capPhase === "compile" ? "compiling" : "running";
    stopWorker();
    running = false;
    runBtn.textContent = "Run";
    runBtn.disabled = false;
    const seconds = ((performance.now() - capStart) / 1000).toFixed(0);
    showOut(
      `stopped after ${seconds} s — the program was still ${phaseLabel} and has ` +
        `been terminated. rho runs programs for as long as they take; if ` +
        `this surprised you, look for runaway recursion or an unbounded loop.`,
      "err",
    );
    setStatus(`<span class="bad">stopped</span> at the ${phaseLabel === "compiling" ? "compile" : "run"} cap`);
    render();
  }, ms);
}

const ta = document.getElementById("input");
const hl = document.getElementById("highlight");
const out = document.getElementById("output");
const status = document.getElementById("status");
const runBtn = document.getElementById("run");
const loadbar = document.getElementById("loadbar");
const exampleSel = document.getElementById("example");
const stats = document.getElementById("stats");

// fast completion (keywords + buffer symbols + prelude tables); the
// type-aware service it is an adapter for is specified in
// docs/language-service.md
const suggest = attachCompletion(ta);

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

function stopRun() {
  stopWorker();
  running = false;
  runBtn.textContent = "Run";
  runBtn.disabled = false;
  showOut("stopped — the worker was terminated.", "err");
  setStatus(`<span class="bad">stopped</span> by hand`);
  render();
}

async function doRun() {
  if (running) {
    stopRun();
    return;
  }
  running = true;
  runBtn.textContent = "Stop";
  out.textContent = "";
  setStatus("compiling…");
  const id = ++workerId;
  if (!worker) spawnWorker();
  armCap(COMPILE_CAP_MS, "compile");
  // queues behind the boot warm in worker message order: even a cold first
  // click waits for the compiler, then compiles
  worker.postMessage({ id, source: ta.value });
}

function fmtBytes(n) {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.round(n / 1024) + " KB";
}

function showProgress(loaded, total, done) {
  loadbar.hidden = false;
  const fill = loadbar.firstElementChild;
  if (total) {
    loadbar.classList.remove("indet");
    fill.style.width = done ? "100%" : Math.min(99, (loaded / total) * 100) + "%";
  } else {
    loadbar.classList.add("indet");
    fill.style.width = "";
  }
  setStatus(
    `loading the compiler… ${fmtBytes(loaded)}${total ? " / " + fmtBytes(total) : ""}`,
  );
}

function hideProgress() {
  loadbar.hidden = true;
}

function onWorkerMessage(m) {
  if (m.kind === "progress") {
    showProgress(m.loaded, m.total, m.done);
    return;
  }
  if (m.kind === "ready") {
    hideProgress();
    if (!running.current) {
      setStatus(`<span class="dim">ready — press <kbd>⌘</kbd><kbd>↵</kbd> to run</span>`);
    }
    return;
  }
  if (m.kind === "boot-error") {
    hideProgress();
    setStatus(`<span class="bad">could not load the compiler</span>`);
    showOut(m.stderr, "err");
    return;
  }
  if (m.id !== workerId) return;
  if (m.kind === "phase" && (m.phase === "boot" || m.phase === "compile")) {
    hideProgress(); // the download is over; the rest is compute
    setStatus(m.phase === "boot" ? "loading the compiler…" : "compiling…");
    return;
  }
  if (m.kind === "phase" && m.phase === "run") {
    setStatus(`running… <span>·</span> compiled ${m.compileMs.toFixed(0)} ms <span>·</span> ${m.bytes.toLocaleString()} bytes`);
    armCap(RUN_CAP_MS, "run");
    return;
  }
  if (m.kind === "done") {
    stopWorker();
    running = false;
    runBtn.textContent = "Run";
    if (!m.ok) {
      showOut(m.stderr, "err");
      setStatus(
        `<span class="bad">${m.stage === "run" ? "runtime error" : "compile error"}</span> in ${(m.compileMs || 0).toFixed(0)} ms`,
      );
    } else {
      showOut(m.stdout);
      if (m.stderr) showOut(m.stderr, "err");
      const exitNote =
        m.exitCode === 0
          ? `<span class="ok">exit 0</span>`
          : `<span class="bad">exit ${m.exitCode}</span>`;
      setStatus(
        `${exitNote} <span>·</span> compiled ${m.compileMs.toFixed(0)} ms <span>·</span> ran ${m.runMs.toFixed(0)} ms <span>·</span> ${m.bytes.toLocaleString()} bytes`,
      );
    }
    render();
  }
}

function onWorkerError(e) {
  stopWorker();
  running = false;
  runBtn.textContent = "Run";
  showOut("runtime error: " + (e.message || "worker failed"), "err");
  setStatus(`<span class="bad">failed</span>`);
  render();
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
  suggest.dismiss(); // the popup would be stale against swapped-in code
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
    return;
  }
  if (suggest.onKeydown(e)) return; // the popup consumed the key (Tab/Enter/arrows/Escape)
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

// the compiler lives in the worker — warm it now, at page load
setStatus(`<span class="dim">loading the compiler…</span>`);
spawnWorker();
if (fromHash || fromQuery) doRun();
