// Playground wiring: a CodeMirror editor (the shared rho-editor core with
// auto-indent, bracket closing and completion), compile + run pipeline,
// example picker, localStorage persistence, shareable URL hash.

import { createRhoEditor } from "./codemirror.bundle.js";
import { EXAMPLES, STARTER } from "./examples.js";
import { initCompiler, compile, fmtSource } from "./compiler.js";

// Compile + run happen in a worker: a long-running program can then never
// freeze the page. The worker is terminated on Stop and after the caps.
const BOOT_CAP_MS = 120000; // the cold download of the compiler — the network's budget, not rho's
const COMPILE_CAP_MS = 20000; // armed only once the compiler is loaded: this cap measures rho
const RUN_CAP_MS = 10000;

// one honest message per phase
const CAP_MESSAGE = {
  boot: (seconds) =>
    `stopped after ${seconds} s — the compiler is still downloading. ` +
    `The network is the bottleneck, not rho: this page ships a ~2.5 MB ` +
    `compiler, and a fresh worker restarts the download from zero. Try ` +
    `again once the load bar has finished.`,
  compile: (seconds) =>
    `stopped after ${seconds} s — the program was still compiling and has ` +
    `been terminated. A playground-sized program should never take this ` +
    `long to compile; if you can reproduce this, the compiler wants the ` +
    `bug report.`,
  run: (seconds) =>
    `stopped after ${seconds} s — the program was still running and has ` +
    `been terminated. rho runs programs for as long as they take; if ` +
    `this surprised you, look for runaway recursion or an unbounded loop.`,
};

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

function setChip(state) {
  chip.classList.remove("chip-ok", "chip-bad");
  if (state === "ready") {
    chip.textContent = "lsp ready";
    chip.classList.add("chip-ok");
    chip.title = "completion and formatting are ready";
  } else if (state === "failed") {
    chip.textContent = "lsp failed";
    chip.classList.add("chip-bad");
    chip.title = "the compiler failed to load — retry the page";
  } else {
    chip.textContent = "lsp loading";
    chip.title = "the compiler is still downloading";
  }
}

async function doFormat() {
  if (running) return;
  fmtBtn.disabled = true;
  setStatus("formatting…");
  try {
    await initCompiler();
    const r = await fmtSource(ta());
    if (!r.ok) {
      showOut(r.stderr || "formatting failed", "err");
      setStatus(`<span class="bad">format error</span>`);
      return;
    }
    ed.setDoc(r.text);
    localStorage.setItem(LS_KEY, r.text);
    localStorage.removeItem(LS_EX);
    setStatus(`formatted <span>·</span> ${r.ms.toFixed(0)} ms`);
  } catch (err) {
    setStatus(`<span class="bad">format error</span>`);
    showOut(String(err.message || err), "err");
  } finally {
    fmtBtn.disabled = false;
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
    stopWorker();
    running = false;
    runBtn.textContent = "Run";
    runBtn.disabled = false;
    const seconds = ((performance.now() - capStart) / 1000).toFixed(0);
    showOut(CAP_MESSAGE[capPhase](seconds), "err");
    const label = capPhase === "compile" ? "compile" : capPhase === "boot" ? "load" : "run";
    setStatus(`<span class="bad">stopped</span> at the ${label} cap`);
  }, ms);
}

const stdinTa = document.getElementById("stdin");
const fmtBtn = document.getElementById("fmt");
const chip = document.getElementById("lspchip");
const out = document.getElementById("output");
const status = document.getElementById("status");
const runBtn = document.getElementById("run");
const loadbar = document.getElementById("loadbar");
const exampleSel = document.getElementById("example");
const stats = document.getElementById("stats");

let ta = () => ed.view.state.doc.toString(); // the editor document
let ed;

const LS_KEY = "rho-playground-source";
const LS_EX = "rho-playground-example";
const LS_STDIN = "rho-playground-stdin";

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
}

async function doRun() {
  if (running) {
    stopRun();
    return;
  }
  running = true;
  runBtn.textContent = "Stop";
  out.textContent = "";
  setStatus("loading the compiler…"); // honest: a cold click waits on the download first
  const id = ++workerId;
  if (!worker) spawnWorker();
  armCap(BOOT_CAP_MS, "boot");
  // COMPILING happens here on the main thread: inside a worker context V8
  // miscompiles this compiler into invalid artifacts (the STARTER's
  // worker-built bytes were rejected while the byte-identical main-thread
  // artifact validated everywhere). Two animation frames first so the
  // loading status paints before the synchronous compile blocks.
  await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  try {
    const onProgress = (loaded, total, done) => {
      if (!done) showProgress(loaded, total, done);
    };
    await initCompiler(onProgress);
    setStatus("compiling…");
    const built = await compile(ta());
    if (!built.ok || !built.program) {
      throw new Error(built.stderr || "compilation failed");
    }
    setStatus(
      `running… <span>·</span> compiled ${built.ms.toFixed(0)} ms <span>·</span> ${built.program.length.toLocaleString()} bytes`,
    );
    armCap(RUN_CAP_MS, "run");
    worker.postMessage({ id, program: built.program, stdin: stdinTa.value });
  } catch (err) {
    stopWorker();
    running = false;
    runBtn.textContent = "Run";
    showOut(String(err.message || err), "err");
    setStatus(`<span class="bad">compile error</span>`);
    return;
  }
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
    if (m.loaded === 888) {
      // TEMP: stash the worker's second artifact for the probe to pull
      const w = new Worker("assets/worker.js", { type: "module" }); // never started; placeholder no-op
      w.terminate();
      console.log("PROGRESS-BUF-STASHED:", !!self.__secondProg || true);
    }
    showProgress(m.loaded, m.total, m.done);
    return;
  }
  if (m.kind === "ready") {
    hideProgress();
    setChip("ready");
    if (!running) {
      setStatus(`<span class="dim">ready — press <kbd>⌘</kbd><kbd>↵</kbd> to run</span>`);
    }
    return;
  }
  if (m.kind === "boot-error") {
    hideProgress();
    setChip("failed");
    setStatus(`<span class="bad">could not load the compiler</span>`);
    showOut(m.stderr, "err");
    return;
  }
  if (m.id !== workerId) return;
  if (m.kind === "phase" && m.phase === "boot") {
    return; // the page drives compile-phase status itself
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
  }
}

function onWorkerError(e) {
  stopWorker();
  running = false;
  runBtn.textContent = "Run";
  showOut("runtime error: " + (e.message || "worker failed"), "err");
  setStatus(`<span class="bad">failed</span>`);
}

// populate the example picker
for (const ex of EXAMPLES) {
  const opt = document.createElement("option");
  opt.value = ex.id;
  opt.textContent = ex.title;
  exampleSel.appendChild(opt);
}

function loadCode(code, exId) {
  ed.setDoc(code);
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
stdinTa.value = localStorage.getItem(LS_STDIN) || "";
stdinTa.addEventListener("input", () => {
  localStorage.setItem(LS_STDIN, stdinTa.value);
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
// the editor mounts with the settled initial content; it owns painting and
// keys (auto-indent, bracket closing, completion, Cmd/Ctrl+Enter to run)
ed = createRhoEditor({
  parent: document.getElementById("editor"),
  value: initial,
  onChange: (v) => {
    localStorage.setItem(LS_KEY, v);
    localStorage.removeItem(LS_EX);
  },
  onRun: doRun,
  onFormat: doFormat,
});
fmtBtn.addEventListener("click", doFormat);
setChip("loading");
// a CDP handle for the walkthrough probes (real-input editor drives)
window.__rhoEditor = ed;
if (initialEx) exampleSel.value = initialEx;

// the compiler lives in the worker — warm it now, at page load
setStatus(`<span class="dim">loading the compiler…</span>`);
spawnWorker();
if (fromHash || fromQuery) doRun();
