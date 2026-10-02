// Renders each tutorial example as a live rho editor — the same CodeMirror
// core the playground and rho.ningh.org use — and runs it IN PLACE: the
// compiler loads once for the whole page, every edit is checked through the
// lint system (squiggles, gutter markers), and Run compiles + executes in
// the shared worker. stdin rides the same two modes the playground has:
// pre-filled redirect content from the example, and — when the program
// reads past it — a terminal-style line that hands the run the next chunk.
import { EXAMPLES } from "./examples.js";
import { createRhoEditor } from "./codemirror.bundle.js";
import { initCompiler, compile, checkSource, parseDiagnostics } from "./compiler.js";

const RUN_HINT = "run to see the output";

// the page shares one worker (and one compiler load) across all examples;
// one run at a time — Run on another example stops the first
let worker = null;
let workerInteractive = false;
let workerReady = false;
let runId = 0;
let active = null; // { id, run, ed }

function ensureWorker() {
  if (worker) return worker;
  workerReady = false;
  worker = new Worker("assets/worker.js", { type: "module" });
  worker.onmessage = (e) => onWorkerMessage(e.data);
  worker.onerror = () => stopRun("the worker failed");
  worker.postMessage({ kind: "warm" });
  return worker;
}

function stopRun(note) {
  if (worker) {
    worker.terminate();
    worker = null;
  }
  if (active) {
    active.run.btn.textContent = "Run ▸";
    if (note) say(active.run.out, note, "err");
    active = null;
  }
}

function say(out, text, cls) {
  const span = document.createElement("span");
  if (cls) span.className = cls;
  span.textContent = text;
  out.appendChild(span);
}

// the terminal line: the run is suspended on a read and the page offers a
// line — Enter hands it over, Ctrl+D or the EOF button closes stdin
function showTermLine(id) {
  if (!active || active.id !== id) return;
  const { out } = active.run;
  const row = document.createElement("div");
  row.className = "term-line";
  row.innerHTML =
    `<span class="term-glyph">❯</span>` +
    `<input type="text" spellcheck="false" autocomplete="off" autocapitalize="off" ` +
    `placeholder="stdin — type a line, Enter to send">` +
    `<button class="term-eof" title="close stdin (Ctrl+D)">EOF</button>`;
  out.appendChild(row);
  out.scrollTop = out.scrollHeight;
  const input = row.querySelector("input");
  input.focus();
  const send = (text) => {
    if (!active || active.id !== id) return;
    row.remove();
    if (text != null) say(out, "❯ " + text, "dim");
    worker.postMessage({ kind: "stdin-give", id, text });
  };
  input.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") send(input.value);
    else if (ev.key === "d" && ev.ctrlKey) send(null);
  });
  row.querySelector(".term-eof").addEventListener("click", () => send(null));
}

function onWorkerMessage(m) {
  if (m.kind === "ready") {
    workerReady = true;
    workerInteractive = !!m.interactive;
    return;
  }
  if (m.kind === "stdin-give") return; // page → worker only
  if (!active || m.id !== active.id) return;
  const { run } = active;
  if (m.kind === "stdin-need") {
    showTermLine(m.id);
    return;
  }
  if (m.kind === "done") {
    active = null;
    run.btn.textContent = "Run ▸";
    if (!m.ok) {
      say(run.out, m.stderr || "runtime error", "err");
    } else {
      say(run.out, m.stdout);
      if (m.stderr) say(run.out, m.stderr, "err");
      const status = m.exitCode === 0 ? "exit 0" : `exit ${m.exitCode}`;
      say(run.out, `\n— ${status} · ran ${m.runMs.toFixed(0)} ms`, "dim");
    }
  }
}

async function doRun(state) {
  if (active) {
    const wasOther = active.run !== state.run;
    stopRun(wasOther ? "stopped — another example started" : undefined);
    if (!wasOther) return; // Stop on the same example
  }
  const id = ++runId;
  state.run.out.textContent = "";
  state.run.btn.textContent = "Stop";
  say(state.run.out, "compiling…", "dim");
  active = { id, run: state.run };
  try {
    const src = state.ed.view.state.doc.toString();
    const built = await compile(src);
    if (!active || active.id !== id) return; // stopped while compiling
    if (!built.ok || !built.program) {
      active = null;
      state.run.btn.textContent = "Run ▸";
      say(state.run.out, built.stderr || "compilation failed", "err");
      return;
    }
    state.run.out.textContent = ""; // the compiling note has served its turn
    say(state.run.out, `— compiled ${built.ms.toFixed(0)} ms · ${built.program.length.toLocaleString()} bytes\n`, "dim");
    ensureWorker().postMessage({
      kind: "run",
      id,
      program: built.program,
      stdin: state.stdinTa ? state.stdinTa.value : "",
      interactive: true,
    });
  } catch (err) {
    if (active && active.id === id) {
      active = null;
      state.run.btn.textContent = "Run ▸";
      say(state.run.out, String(err.message || err), "err");
    }
  }
}

function scheduleCheck(state) {
  clearTimeout(state.checkTimer);
  state.checkTimer = setTimeout(async () => {
    try {
      const r = await checkSource(state.ed.view.state.doc.toString());
      const diags = parseDiagnostics(r.stderr);
      state.ed.setDiagnostics(diags);
      state.dot.classList.toggle("dot-ok", diags.length === 0);
      state.dot.classList.toggle("dot-bad", diags.length > 0);
      state.dot.title = diags.length
        ? `${diags.length} problem${diags.length > 1 ? "s" : ""} — hover the underlines`
        : "checks clean";
    } catch {
      // a failed check stays quiet; the next edit retries
    }
  }, 700);
}

// the compiler warms with the page — the worker fetch warms the HTTP cache
// the page's own checker then hits; edits before it lands simply queue
window.addEventListener("load", () => ensureWorker());

document.querySelectorAll("template[data-example]").forEach((tpl) => {
  const ex = EXAMPLES.find((e) => e.id === tpl.dataset.example);
  if (!ex) return;
  const div = document.createElement("div");
  div.className = "example";
  div.innerHTML = `
    <div class="bar"><span>${ex.id}.rho</span><span class="dot" title="the checker warms with the page"></span>
      <span class="spacer"></span>
      <button class="run">Run ▸</button></div>
    <div class="ed"></div>
    <div class="out"><span class="hint">${RUN_HINT}</span></div>`;
  const state = {
    ed: createRhoEditor({
      parent: div.querySelector(".ed"),
      value: ex.code,
      onChange: () => scheduleCheck(state),
      onRun: () => doRun(state),
    }),
    run: {
      btn: div.querySelector(".bar .run"),
      out: div.querySelector(".out"),
    },
    dot: div.querySelector(".dot"),
    stdinTa: null,
    checkTimer: null,
  };
  if (ex.stdin) {
    // redirect mode: the example ships its own stdin, editable in place
    const row = document.createElement("div");
    row.className = "stdin-row";
    row.innerHTML = `<span>stdin — one line per read_line()</span>`;
    const ta = document.createElement("textarea");
    ta.className = "stdin";
    ta.rows = 2;
    ta.spellcheck = false;
    ta.autocapitalize = "off";
    ta.value = ex.stdin;
    row.appendChild(ta);
    div.insertBefore(row, state.run.out);
    state.stdinTa = ta;
  }
  state.run.btn.addEventListener("click", () => doRun(state));
  tpl.replaceWith(div);
});
