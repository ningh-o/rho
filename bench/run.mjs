// rho benchmark runner — honest, reproducible numbers.
//
//   node bench/run.mjs [--runs 5] [--warmup 1] [--only <kernel>]
//
// Method (see bench/README.md for the full contract):
//   - every kernel is fixed-input and deterministic; the expected output
//     string is recomputed here, independently of all three implementations,
//     and every run's stdout must match it exactly (exit 0 too) — a run
//     that mismatches is recorded as failed, never averaged in;
//   - timing is wall-clock around the WHOLE runtime process (spawn -> exit):
//     rho and C both run as wasm32-wasi modules under wasmtime (the runtime
//     `rho run` itself shells out to), JavaScript runs under node. The noop
//     entry measures each runtime's process overhead so kernel medians can
//     be read against it;
//   - one warmup run (discarded), then `--runs` measured runs, median
//     reported;
//   - numbers come only from this machine, this run — nothing is estimated
//     or carried over.
//
// Tool paths can be overridden by env: RHOC, WASMTIME, WASI_CLANG, NODE_BIN.

import { spawnSync } from "node:child_process";
import { performance } from "node:perf_hooks";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const RHO_ROOT = path.resolve(HERE, "..");
const SRC = path.join(HERE, "src");
const OUT = path.join(HERE, "build");
const LOGS = path.join(HERE, "logs");

const RHOC = process.env.RHOC ?? path.join(RHO_ROOT, "build", "rho-boot");
const WASMTIME = process.env.WASMTIME ?? "/opt/homebrew/bin/wasmtime";
const WASI_CLANG =
  process.env.WASI_CLANG ??
  path.join(process.env.HOME ?? "", "Developer/tools/wasi-sdk/bin/clang");
const WASI_SYSROOT = path.join(path.dirname(WASI_CLANG), "..", "share", "wasi-sysroot");
const NODE = process.env.NODE_BIN ?? process.execPath;

const argv = process.argv.slice(2);
function flag(name, dflt) {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] ? Number(argv[i + 1]) : dflt;
}
const RUNS = flag("runs", 5);
const WARMUP = flag("warmup", 1);
const ONLY = argv.includes("--only") ? argv[argv.indexOf("--only") + 1] : null;

fs.mkdirSync(OUT, { recursive: true });
fs.mkdirSync(LOGS, { recursive: true });

// ------------------------------------------------- expected outputs ----
// Recomputed here so no implementation can grade its own homework.

function expectFib() {
  function fib(n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
  }
  return `fib(32)=${fib(32)}`;
}

function expectNqueens() {
  const FULL = 4095; // (1 << 12) - 1
  function count(mask, dl, dr) {
    if (mask === FULL) return 1;
    let total = 0;
    let avail = FULL & ~(mask | dl | dr);
    while (avail !== 0) {
      const bit = avail & -avail;
      avail &= avail - 1;
      total += count(mask | bit, (dl | bit) << 1, (dr | bit) >> 1);
    }
    return total;
  }
  return `queens(12)=${count(0, 0, 0)}`;
}

function expectMatmul(N) {
  const A = new Float64Array(N * N);
  const B = new Float64Array(N * N);
  for (let i = 0; i < N; i++)
    for (let j = 0; j < N; j++) {
      A[i * N + j] = ((i * 7 + j * 13) % 19) - 9;
      B[i * N + j] = ((i * 5 + j * 3) % 17) - 8;
    }
  // same accumulation order as the kernels: k innermost per element, then
  // row-major sum — bit-identical f64 results in all three languages
  let total = 0;
  for (let i = 0; i < N; i++)
    for (let j = 0; j < N; j++) {
      let s = 0;
      for (let k = 0; k < N; k++) s += A[i * N + k] * B[k * N + j];
      total += s;
    }
  const bits = new BigUint64Array(new Float64Array([total]).buffer)[0];
  return `matmul(${N}) sum_bits=${bits} sum_int=${Math.trunc(total)}`;
}

// piece cycles shared by the strings kernels: lens/charcode-sums per piece
const PIECE_LENS = [1, 2, 3, 4];
const PIECE_SUMS = [97, 197, 303, 418]; // "a" | "bc" | "def" | "ghij"

function expectStrings(pieces) {
  let n = 0;
  let sum = 0;
  for (let i = 0; i < pieces; i++) {
    n += PIECE_LENS[i & 3];
    sum += PIECE_SUMS[i & 3];
  }
  return { n, sum };
}

function expectIntloop() {
  let acc = 0;
  for (let i = 0; i < 100000000; i++) {
    const t = i % 100000;
    acc = (acc + t * t) % 4294967291; // exact in doubles: t*t <= 1e10 < 2^53
  }
  return `intloop acc=${acc}`;
}

// ------------------------------------------------------- the kernels ----

const KERNELS = [
  {
    name: "noop",
    task: "empty program — runtime process overhead (spawn -> exit), not a kernel",
    expected: "",
  },
  {
    name: "fib",
    task: "naive recursive fib(32), i64",
    expected: expectFib(),
  },
  {
    name: "nqueens",
    task: "count 12-queens solutions via col/diag bitmask recursion, i32",
    expected: expectNqueens(),
  },
  {
    name: "matmul",
    task: "f64 matmul C = A*B, 256x256, k innermost; checksum = IEEE bit pattern of the row-major sum of C (order pinned, no fast-math anywhere). Escalated from the task's 192 because measured C compute at 192 (~4ms) sat under wasmtime's own 4.1ms process overhead — the task's 'too fast -> 256' clause; the 192 numbers are kept as the supplementary matmul192 kernel",
    expected: expectMatmul(256),
  },
  {
    name: "matmul192",
    task: "SUPPLEMENTARY: the task's original 192x192 matmul, kept because the headline escalated to 256. Same op order and checksum scheme as matmul",
    expected: expectMatmul(192),
  },
  {
    name: "strings",
    task: "append 200k small pieces into a 500KB string. rho: preallocated []u8 + index writes + one intrinsics.slice_string (rho's cat append is O(n^2) — see naive_append); C: one malloc + memcpy per piece; JS: s += piece",
    expected: (() => {
      const { n, sum } = expectStrings(200000);
      return `strings n=${n} bytes=${sum}`;
    })(),
  },
  {
    name: "naive_append",
    task: "SUPPLEMENTARY (1.5k pieces, not the 200k kernel): the naive append loop in each language — rho: s = cat(s, p); C: strcat; JS: s +=. rho's cat is O(n^2) in copies AND in live memory (the prelude bump allocator never frees; each append allocates twice — cat's make + slice_string's copy): at 20k pieces the module dies with `wasm trap: out of bounds memory access` (exit 134, observed this run), and 2k pieces dies the same way; 1.5k pieces (~5.6MB cumulative over the ~8MB heap) is the largest round scale that fits",
    expected: (() => {
      const { n, sum } = expectStrings(1500);
      return `naive n=${n} bytes=${sum}`;
    })(),
  },
  {
    name: "intloop",
    task: "tight integer loop, 1e8 iterations of acc = (acc + (i%100000)^2) % 4294967291, i64",
    expected: expectIntloop(),
  },
];

const selected = ONLY ? KERNELS.filter((k) => k.name === ONLY) : KERNELS;
if (selected.length === 0) {
  console.error(`no kernel named "${ONLY}"`);
  process.exit(2);
}

// --------------------------------------------------------- tooling ----

function sh(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, {
    cwd: RHO_ROOT,
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
    timeout: 300_000,
    ...opts,
  });
  return r;
}

function firstLine(s) {
  return (s ?? "").split("\n")[0].trim();
}

const rhoVersion = firstLine(sh(RHOC, []).stderr) || firstLine(sh(RHOC, []).stdout) || "rho (version unknown)";
const wasmtimeVersion = firstLine(sh(WASMTIME, ["--version"]).stdout) || "wasmtime (version unknown)";
const clangVersion = firstLine(sh(WASI_CLANG, ["--version"]).stdout) || "clang (version unknown)";
const nodeVersion = firstLine(sh(NODE, ["--version"]).stdout) || "node (version unknown)";

const rhoHead = sh("git", ["rev-parse", "HEAD"]).stdout?.trim() ?? "unknown";
const rhoDirty = (sh("git", ["status", "--porcelain"]).stdout ?? "").trim().split("\n").filter(Boolean);

// provenance of the boot binary: is build/rho-boot newer than every input
// the Makefile would rebuild it from? (This runner never rebuilds it — the
// working tree may carry in-flight compiler work that is not ours.)
const BOOT_SOURCES = [
  ...fs.readdirSync(path.join(RHO_ROOT, "boot", "src")).filter((f) => f.endsWith(".c") || f.endsWith(".h")),
  "boot/src/prelude_data.c",
  ...fs.readdirSync(path.join(RHO_ROOT, "boot", "prelude")).filter((f) => f.endsWith(".rho")),
];
const bootBinStat = fs.statSync(RHOC);
const newestSource = BOOT_SOURCES.reduce(
  (acc, f) => {
    for (const dir of [path.join(RHO_ROOT, "boot", "src"), path.join(RHO_ROOT, "boot", "prelude")]) {
      const p = path.join(dir, f);
      if (fs.existsSync(p)) {
        const m = fs.statSync(p).mtimeMs;
        if (m > acc.mtime) return { path: f, mtime: m };
      }
    }
    return acc;
  },
  { path: "(none)", mtime: -1 },
);
const bootBinaryFresh = bootBinStat.mtimeMs > newestSource.mtime;

// ------------------------------------------------- build every impl ----

function buildRho(name) {
  const src = path.join(SRC, `${name}.rho`);
  const out = path.join(OUT, `${name}.rho.wasm`);
  const t0 = performance.now();
  const r = sh(RHOC, ["build", src, "-o", out]);
  const build_ms = performance.now() - t0;
  if (r.status !== 0) {
    return { error: `rho build failed (exit ${r.status}): ${firstLine(r.stderr) || firstLine(r.stdout)}` };
  }
  return {
    lang: "rho",
    tool: `${rhoVersion} build (wasm32-wasi, spill-everything register allocation)`,
    build_cmd: `rho build bench/src/${name}.rho -o bench/build/${name}.rho.wasm`,
    artifact: `bench/build/${name}.rho.wasm`,
    artifact_bytes: fs.statSync(out).size,
    build_ms,
    runtime_cmd: `wasmtime run bench/build/${name}.rho.wasm`,
  };
}

function buildC(name) {
  const src = path.join(SRC, `${name}.c`);
  const out = path.join(OUT, `${name}.c.wasm`);
  const t0 = performance.now();
  const r = sh(WASI_CLANG, ["--target=wasm32-wasi", `--sysroot=${WASI_SYSROOT}`, "-O2", "-o", out, src]);
  const build_ms = performance.now() - t0;
  if (r.status !== 0) {
    return { error: `clang build failed (exit ${r.status}): ${firstLine(r.stderr)}` };
  }
  return {
    lang: "c",
    tool: `${clangVersion} --target=wasm32-wasi -O2 (wasi-sdk)`,
    build_cmd: `clang --target=wasm32-wasi --sysroot=... -O2 -o bench/build/${name}.c.wasm bench/src/${name}.c`,
    artifact: `bench/build/${name}.c.wasm`,
    artifact_bytes: fs.statSync(out).size,
    build_ms,
    runtime_cmd: `wasmtime run bench/build/${name}.c.wasm`,
  };
}

function buildJs(name) {
  const src = path.join(SRC, `${name}.js`);
  return {
    lang: "js",
    tool: `${nodeVersion} (V8)`,
    build_cmd: "(none — run from source)",
    artifact: `bench/src/${name}.js`,
    artifact_bytes: fs.statSync(src).size,
    build_ms: null,
    runtime_cmd: `node bench/src/${name}.js`,
  };
}

// ------------------------------------------------------- run + time ----

function runOnce(impl, kernel) {
  const [cmd, ...args] =
    impl.lang === "rho"
      ? [WASMTIME, "run", impl.artifact]
      : impl.lang === "c"
        ? [WASMTIME, "run", impl.artifact]
        : [NODE, impl.artifact];
  const t0 = performance.now();
  const r = sh(cmd, args);
  const ms = performance.now() - t0;
  return { ms, exit: r.status, stdout: r.stdout ?? "", stderr: r.stderr ?? "" };
}

function measure(kernel, build) {
  const logLines = [];
  const log = (s) => logLines.push(s);
  log(`# ${kernel.name} / ${build.lang}`);
  log(`# build: ${build.build_cmd}`);
  log(`# run:   ${build.runtime_cmd}`);
  log(`# expected stdout: ${JSON.stringify(kernel.expected)}`);

  const runs = [];
  for (let i = -WARMUP; i < RUNS; i++) {
    const r = runOnce(build, kernel);
    const isWarmup = i < 0;
    const output_ok = r.exit === 0 && r.stdout.trim() === kernel.expected;
    const rec = { ms: r.ms, exit: r.exit, output_ok, warmup: isWarmup };
    if (!output_ok) {
      rec.stdout = r.stdout.slice(0, 2000);
      if (r.stderr) rec.stderr = r.stderr.slice(0, 2000);
    }
    runs.push(rec);
    log(
      `${isWarmup ? "warmup" : `run ${i + 1}`}   : ${r.ms.toFixed(2)} ms  exit=${r.exit}  output_ok=${output_ok}` +
        (output_ok ? "" : `\n  stdout: ${JSON.stringify(r.stdout.slice(0, 500))}`),
    );
    if (r.exit !== 0 && !isWarmup && r.stderr) log(`  stderr: ${r.stderr.slice(0, 500)}`);
  }

  const measured = runs.filter((r) => !r.warmup);
  const passes = measured.filter((r) => r.output_ok);
  const ok = passes.length === measured.length;
  const mss = passes.map((r) => r.ms).sort((a, b) => a - b);
  const median = mss.length ? mss[Math.floor(mss.length / 2)] : null;
  const result = {
    ...build,
    warmups: WARMUP,
    runs_ms: measured.map((r) => Number(r.ms.toFixed(2))),
    median_ms: median === null ? null : Number(median.toFixed(2)),
    min_ms: mss.length ? Number(mss[0].toFixed(2)) : null,
    max_ms: mss.length ? Number(mss[mss.length - 1].toFixed(2)) : null,
    runs_passed: passes.length,
    runs_total: measured.length,
    ok,
  };
  if (!ok) {
    const bad = measured.find((r) => !r.output_ok);
    result.note = bad
      ? `run failed: exit=${bad.exit}, output_ok=${bad.output_ok} — see bench/logs/${kernel.name}.${build.lang}.log`
      : "failed";
  }
  fs.writeFileSync(path.join(LOGS, `${kernel.name}.${build.lang}.log`), logLines.join("\n") + "\n");
  return result;
}

// ------------------------------------------------------------- main ----

console.log(`rho bench — ${new Date().toISOString()}`);
console.log(`tools: ${rhoVersion} | ${wasmtimeVersion} | ${clangVersion} | ${nodeVersion}`);
console.log(`method: ${WARMUP} warmup + ${RUNS} measured runs, median, whole-process wall clock\n`);

const results = [];
for (const kernel of selected) {
  const impls = [];
  const builds = [buildRho(kernel.name), buildC(kernel.name), buildJs(kernel.name)];
  for (const b of builds) {
    if (b.error) {
      impls.push({ lang: null, ok: false, note: b.error, artifact_bytes: null, median_ms: null });
      console.log(`  ${kernel.name}: ${b.error}`);
      continue;
    }
    const m = measure(kernel, b);
    impls.push(m);
    console.log(
      `  ${kernel.name.padEnd(13)} ${m.lang.padEnd(4)} median ${String(m.median_ms).padStart(10)} ms  ` +
        `(${m.runs_passed}/${m.runs_total} runs passed${m.ok ? "" : "  — FAILED, see log"})`,
    );
  }
  // cross-impl agreement check for the record
  const allOk = impls.every((i) => i.ok);
  results.push({
    name: kernel.name,
    task: kernel.task,
    expected_output: kernel.expected,
    all_implementations_passed: allOk,
    impls,
  });
}

const out = {
  schema: "rho-bench-results/1",
  meta: {
    date: new Date().toISOString(),
    host: {
      os: `${os.type()} ${os.release()}`,
      arch: os.arch(),
      cpu: os.cpus()[0]?.model ?? "unknown",
      memory_bytes: os.totalmem(),
      note: "laptop, plugged-in state unknown — treat small deltas (a few %) as noise",
    },
    tools: {
      rho: rhoVersion,
      wasmtime: wasmtimeVersion,
      clang: clangVersion,
      node: nodeVersion,
    },
    rho_git: {
      head: rhoHead,
      dirty_files: rhoDirty.length,
      dirty_paths: rhoDirty,
    },
    compiler_status:
      "The rho compiler's register allocation is still spill-everything: docs/todo.md's " +
      "'0.2 backlog (unchanged)' lists 'linear-scan register allocator (replaces spill-everything)' " +
      "as open, and no linear-scan work exists in this tree — the uncommitted boot/ changes are " +
      "WASI import wiring + width fixes in emit_wasm.c/prelude, not the allocator. Every rho number " +
      "here carries the full spill/reload cost; treat rho-vs-C gaps as 'current compiler state', " +
      "not the language's ceiling.",
    boot_binary_provenance: {
      path: "build/rho-boot",
      mtime: new Date(bootBinStat.mtimeMs).toISOString(),
      newest_input: newestSource.path,
      newest_input_mtime: new Date(newestSource.mtime).toISOString(),
      built_from_current_tree: bootBinaryFresh,
      note: "this runner never rebuilds the compiler; it uses the working tree's binary as found",
    },
    method: {
      timing: "wall-clock around the whole runtime process (spawn -> exit), performance.now()",
      warmup: WARMUP,
      runs: RUNS,
      aggregate: "median of measured runs (all runs also recorded)",
      runtimes: "rho and C both as wasm32-wasi modules under wasmtime (the runtime `rho run` shells out to, boot/src/main.c:609); JS under node",
      correctness_gate:
        "every run's stdout must equal the expected string recomputed independently by this driver; failing runs are recorded as failures, never averaged in",
    },
  },
  kernels: results,
};

const resultsPath = path.join(HERE, "results.json");
fs.writeFileSync(resultsPath, JSON.stringify(out, null, 2) + "\n");
console.log(`\nwrote ${path.relative(RHO_ROOT, resultsPath)}`);
console.log(`logs in ${path.relative(RHO_ROOT, LOGS)}/`);

// summary table for the console (medians only — full data in results.json)
console.log(`\n${"kernel".padEnd(15)}${"rho (wasmtime)".padStart(15)}${"C (wasmtime)".padStart(15)}${"js (node)".padStart(15)}${"rho/js".padStart(9)}${"C/js".padStart(8)}`);
for (const k of results) {
  const m = Object.fromEntries(k.impls.map((i) => [i.lang ?? "?", i.median_ms]));
  const ratio = (a) => (a != null && m.js != null ? (a / m.js).toFixed(1) + "x" : "—");
  console.log(
    k.name.padEnd(15) +
      String(m.rho ?? "—").padStart(15) +
      String(m.c ?? "—").padStart(15) +
      String(m.js ?? "—").padStart(15) +
      ratio(m.rho).padStart(9) +
      ratio(m.c).padStart(8),
  );
}
