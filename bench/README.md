# rho/bench — the honest benchmark suite

Fixed-input, deterministic kernels, each implemented three ways: **rho**
(compiled by the repo's boot toolchain to wasm32-wasi, run on wasmtime —
the same runtime `rho run` shells out to), **C** (wasi-sdk clang
`--target=wasm32-wasi -O2`, also run on wasmtime), and **JavaScript**
(node, run from source). Every number comes from a real local run; nothing
is estimated, extrapolated, or carried over.

Reproduce:

```bash
node bench/run.mjs                 # full suite: builds + times + writes results
node bench/run.mjs --only fib     # one kernel
RHOC=... WASMTIME=... WASI_CLANG=... node bench/run.mjs   # other tools
```

Sources live in `bench/src/<kernel>.{rho,c,js}`; the runner is
`bench/run.mjs`; outputs land in `bench/results.json` (schema below) and
`bench/logs/<kernel>.<lang>.log` (one line per run with its wall time,
exit code, and output-verdict).

## Method

- **Timing**: wall clock around the WHOLE runtime process
  (`performance.now()` around `spawnSync`): process start → module
  instantiate → run → exit. rho and C both pay wasmtime process overhead;
  node pays its own (bigger) startup. The `noop` kernel measures each
  runtime's empty-program overhead — read every other number against it
  (it appears in results.json as a kernel row; medians are reported raw,
  never overhead-adjusted).
- **Warmup + median**: 1 warmup run (discarded), then 5 measured runs;
  the median is reported. Every individual run is also in results.json
  and the logs.
- **Correctness gate**: each kernel prints a checksum line; the runner
  recomputes the expected string independently (in the driver itself,
  from the kernel definitions) and every run's stdout must match it
  exactly, with exit 0. Failing runs are recorded as failures — never
  averaged in, never silently dropped. The matmul checksum is the exact
  IEEE-754 bit pattern of a row-major sum of C (k innermost, no
  fast-math anywhere), so any float reassociation between implementations
  shows up as a gate failure; on the run recorded here all three agreed
  bit-for-bit.
- **Scale honesty**: kernel inputs are the task's fixed sizes (fib(32),
  12 queens, 200k string pieces, ~1e8 integer-loop iterations). On a fast
  machine some kernels compute in single-digit milliseconds — for those,
  the process overhead is a large fraction of the median, and the noop
  row says how much. The one input the task allowed escalating (matmul
  192 → 256 "if too fast") WAS escalated: measured C compute at 192
  (~4 ms) sat under wasmtime's 4.1 ms process overhead; the 192 numbers
  are kept as the supplementary `matmul192` kernel.

## The kernels

| kernel | what runs (same inputs in all three languages) |
|---|---|
| `noop` | empty program — runtime overhead, not a kernel |
| `fib` | naive recursive fib(32), i64 |
| `nqueens` | 12-queens solution count via col/diag bitmask recursion, i32 (answer: 14200) |
| `matmul` | f64 C = A·B at 256×256, k innermost; checksum = bit pattern of the row-major sum |
| `matmul192` | supplementary: the same at the task's original 192×192 |
| `strings` | append 200k small pieces ("a","bc","def","ghij" cycled) into a 500KB string; checksum = length + byte sum |
| `naive_append` | supplementary (1.5k pieces): the NAIVE append loop in each language — see the strings section |
| `intloop` | 1e8 iterations of `acc = (acc + (i%100000)²) % 4294967291`, i64 (every step exact in i64 and in a JS double, so all three must agree digit-for-digit) |

## The strings kernels: what each language actually does

rho has **no growable string builder**. Its only string-append API,
`cat` (`boot/prelude/core.rho:110`), allocates a fresh buffer and copies
byte-by-byte — O(n) per append, O(n²) per build. And it gets worse:
`__free` in the WASI prelude is a no-op bump allocator
(`boot/prelude/wasi.rho:19-31`) and each append allocates twice (cat's
`make` + `intrinsics.slice_string`'s fresh copy, `boot/src/lower.c:2675`),
so the naive append's **live memory** is also O(n²) against the ~8MB heap
(`boot/src/emit_wasm.c:279`). Measured this run: at 20k pieces the module
dies with `wasm trap: out of bounds memory access` (exit 134); at 2k
pieces too; 1.5k pieces (~5.6MB cumulative) is the largest round scale
that fits.

So the `strings` kernel times the honest linear path per language, and
each is spelled out in its source file:

- **rho**: one preallocated `[]u8`, filled by index from the piece
  strings, converted once with `intrinsics.slice_string`
  (`spec/module-system.md:71` — std-only by convention, and the
  convention is not enforced);
- **C**: one `malloc` + `memcpy` per piece (the idiomatic amortized
  append);
- **JS**: `s += piece` (V8 cons strings).

The supplementary `naive_append` kernel then times the actual naive loop
(`s = cat(s, p)` / `strcat` / `s +=`) at the largest scale rho survives.

## Compiler status — read before comparing

- The rho compiler's register allocation is **still spill-everything**:
  `docs/todo.md`'s "0.2 backlog (unchanged)" lists *"linear-scan register
  allocator (replaces spill-everything)"* as open, and no linear-scan
  work exists in this tree (the uncommitted `boot/` changes at run time
  are WASI import wiring + width fixes in `emit_wasm.c`/prelude — not the
  allocator). Nuance: spills here are at the IR→wasm-local level; the
  wasm then goes through wasmtime's Cranelift, which re-allocates real
  registers — so rho numbers carry "extra wasm locals and copies", not
  native spill/reload.
- Every rho-vs-C gap in results.json is **current compiler state**, not
  the language's ceiling.
- Provenance is recorded per run in `results.json` under
  `meta.boot_binary_provenance`: the runner never rebuilds the compiler;
  it uses the working tree's `build/rho-boot` as found and checks its
  mtime against its newest input. This run's binary was built from the
  exact tree it ran on.

## results.json schema (`rho-bench-results/1`)

```jsonc
{
  "schema": "rho-bench-results/1",
  "meta": {
    "date": "<ISO timestamp of the run>",
    "host": { "os": "...", "arch": "arm64", "cpu": "...", "memory_bytes": 0, "note": "..." },
    "tools": { "rho": "...", "wasmtime": "...", "clang": "...", "node": "..." },
    "rho_git": { "head": "<sha>", "dirty_files": 0, "dirty_paths": ["..."] },
    "compiler_status": "<the spill-everything paragraph above>",
    "boot_binary_provenance": {
      "path": "build/rho-boot", "mtime": "<ISO>", "newest_input": "<file>",
      "newest_input_mtime": "<ISO>", "built_from_current_tree": true,
      "note": "this runner never rebuilds the compiler; ..."
    },
    "method": {
      "timing": "wall-clock around the whole runtime process ...",
      "warmup": 1, "runs": 5, "aggregate": "median of measured runs ...",
      "runtimes": "rho and C both as wasm32-wasi modules under wasmtime ...; JS under node",
      "correctness_gate": "every run's stdout must equal the expected string ..."
    }
  },
  "kernels": [
    {
      "name": "fib",
      "task": "<what the kernel does + any caveat>",
      "expected_output": "<the exact checksum line all impls must print>",
      "all_implementations_passed": true,
      "impls": [
        {
          "lang": "rho",                       // "rho" | "c" | "js"
          "tool": "<exact compiler/runtime + flags>",
          "build_cmd": "<command>", "runtime_cmd": "<command>",
          "artifact": "bench/build/fib.rho.wasm",
          "artifact_bytes": 12345,             // wasm module / js source size
          "build_ms": 12.3,                    // null for js (no build)
          "warmups": 1, "runs_ms": [ /* the 5 measured wall times */ ],
          "median_ms": 18.9, "min_ms": 0, "max_ms": 0,
          "runs_passed": 5, "runs_total": 5,
          "ok": true,
          "note": "only present when something failed or is missing"
        }
      ]
    }
  ]
}
```

A missing or failed baseline appears as an impl entry with `ok: false`
and a `note` (or is absent with the absence recorded in the kernel's
`task` text) — numbers are never invented to fill a hole.
