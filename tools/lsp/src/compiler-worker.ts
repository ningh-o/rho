// Worker-thread entry for the in-process rho compiler. Loaded once per
// server process: it compiles the compiler wasm module a single time and
// answers { id, op, source } requests with { id, ok, exitCode, stdout,
// stderr }. Running the compiler here keeps the LSP connection thread
// fully responsive no matter what a document does to the compiler — the
// supervisor (compiler.ts) can terminate and respawn this worker without
// the editor ever noticing.

import { parentPort, workerData } from 'node:worker_threads';
import { readFileSync } from 'node:fs';
import { createFS, runModule } from './wasi.js';

interface WorkerRequest {
  id: number;
  op: 'check' | 'fmt';
  source: string;
}

interface WorkerReply {
  id: number;
  ok: boolean;
  exitCode?: number;
  stdout?: string;
  stderr?: string;
  error?: string;
}

const port = parentPort;
if (!port) {
  throw new Error('compiler-worker: not started as a worker thread');
}

const wasmPath = String((workerData as { wasmPath?: string }).wasmPath ?? '');
const bytes = new Uint8Array(readFileSync(wasmPath));

let module: WebAssembly.Module | null = null;

/** Run one compiler invocation against an in-memory /main.rho. */
async function runOne(op: WorkerRequest['op'], source: string) {
  if (!module) module = await WebAssembly.compile(bytes);
  const fs = createFS();
  fs.write('/main.rho', new TextEncoder().encode(source));
  const args =
    op === 'fmt' ? ['rho', 'fmt', '/main.rho'] : ['rho', 'check', '/main.rho'];
  // The run is synchronous inside the worker thread; instantiation of the
  // cached module costs ~1ms for the ~600KB compiler.
  return runModule(module, { args, fs });
}

port.on('message', (req: WorkerRequest) => {
  runOne(req.op, String(req.source ?? ''))
    .then((result) => {
      const reply: WorkerReply = {
        id: req.id,
        ok: true,
        exitCode: result.exitCode,
        stdout: result.stdout,
        stderr: result.stderr,
      };
      port.postMessage(reply);
    })
    .catch((e: unknown) => {
      const reply: WorkerReply = {
        id: req.id,
        ok: false,
        error: e instanceof Error ? e.message : String(e),
      };
      port.postMessage(reply);
    });
});
