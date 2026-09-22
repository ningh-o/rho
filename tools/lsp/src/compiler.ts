// Compiler host: drives the real rho toolchain from the server process.
//
// Three backends, in descending order of preference:
//
//   1. wasm-worker  — the boot compiler as a wasm32-wasi program
//     (build/rho.wasm), run inside a dedicated worker thread through a
//     minimal WASI shim. Same in-process pipeline the browser playground
//     uses (site/assets/wasi.js), same bytes the native toolchain produces.
//     A hung or crashing compile is contained: the supervisor terminates
//     the worker and respawns it on the next request.
//   2. native-cli   — spawn a native `rho` binary (PATH, RHO_LSP_BIN, or
//     build/rho-boot) with a temp file per invocation.
//   3. none         — nothing found: every operation reports a failure
//     outcome, the server degrades to no diagnostics and logs once.
//
// check()/fmt() NEVER reject. Host-level failures (crash, timeout, missing
// compiler) resolve as { exitCode: -1 } so callers cannot leak unhandled
// rejections into notification handlers; onIssue carries the reason.

import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';
import type { RunResult } from './wasi.js';

export type CompilerBackend = 'wasm-worker' | 'native-cli' | 'none';

/** Raw compiler-process outcome. exitCode < 0 means "no result" (host-level
 *  failure): stdout/stderr are empty and the caller must degrade quietly. */
export interface CompilerOutcome extends RunResult {}

export interface Compiler {
  readonly backend: CompilerBackend;
  /** Absolute path of the compiler wasm in use (wasm-worker backend). */
  readonly wasmPath: string | null;
  /** Native binary in use (native-cli backend). */
  readonly binaryPath: string | null;
  /** `rho check` semantics: parse + type-check an in-memory source. */
  check(source: string): Promise<CompilerOutcome>;
  /** `rho fmt` semantics: canonical formatting on stdout, or diagnostics. */
  fmt(source: string): Promise<CompilerOutcome>;
  dispose(): Promise<void>;
  /** Test-only: simulate an abrupt worker crash to prove respawn works. */
  crashForTest(): void;
}

export interface CompilerOptions {
  /** Host-level failure reporter (worker crash, timeout, spawn error). */
  onIssue?: (message: string) => void;
  /** Per-invocation timeout for the wasm worker, ms (default 10_000). */
  timeoutMs?: number;
  /** Force a backend instead of auto-detection (env RHO_LSP_BACKEND:
   *  "wasm" | "native" | "none" — both the long backend ids and these
   *  short spellings are accepted). A forced backend that is unavailable
   *  degrades to the inert one with an issue reported. */
  backend?: CompilerBackend | 'wasm' | 'native';
}

const DEFAULT_TIMEOUT_MS = 10_000;

// ------------------------------------------------------------ discovery --

/** Package root: dist/ -> the package directory (rho/tools/lsp in-tree). */
function packageRoot(): string {
  return dirname(dirname(fileURLToPath(import.meta.url)));
}

/** Locate the compiler wasm, most recent artifact first. */
export function resolveCompilerWasm(): string | null {
  const root = packageRoot();
  const candidates: string[] = [];
  if (process.env.RHO_LSP_WASM) candidates.push(process.env.RHO_LSP_WASM);
  candidates.push(
    join(root, '..', '..', 'build', 'rho.wasm'),
    join(root, '..', '..', 'site', 'assets', 'rho.wasm'),
  );
  for (const c of candidates) {
    try {
      if (c && existsSync(c)) return c;
    } catch {
      // unreadable path — try the next candidate
    }
  }
  return null;
}

/** Locate a native rho binary (PATH first, then the in-tree build). */
export function resolveNativeBinary(): string | null {
  if (process.env.RHO_LSP_BIN && existsSync(process.env.RHO_LSP_BIN)) {
    return process.env.RHO_LSP_BIN;
  }
  const root = packageRoot();
  const inTree = join(root, '..', '..', 'build', 'rho-boot');
  if (existsSync(inTree)) return inTree;
  for (const dir of (process.env.PATH ?? '').split(':')) {
    if (!dir) continue;
    const candidate = join(dir, 'rho');
    try {
      if (existsSync(candidate)) return candidate;
    } catch {
      // unreadable path entry — keep scanning
    }
  }
  return null;
}

// --------------------------------------------------------- wasm backend --

interface Pending {
  resolve: (outcome: CompilerOutcome) => void;
  timer: NodeJS.Timeout;
}

/** One worker plus the requests in flight against it. Failing a session
 *  never touches other sessions — a dying worker cannot pollute a freshly
 *  spawned one. */
interface Session {
  worker: Worker;
  pending: Map<number, Pending>;
  /** Set when the host drops the session deliberately (dispose/abandon):
   *  its eventual exit event must not be reported as an issue. */
  quiet: boolean;
}

class WasmWorkerHost {
  private session: Session | null = null;
  private nextId = 1;

  constructor(
    private readonly wasmPath: string,
    private readonly onIssue: (message: string) => void,
    private readonly timeoutMs: number,
  ) {}

  private spawn(): Session {
    const worker = new Worker(new URL('./compiler-worker.js', import.meta.url), {
      workerData: { wasmPath: this.wasmPath },
    });
    // NOTE: the worker stays ref'd — the host owns its lifecycle and
    // dispose() (or process exit) tears it down.
    const session: Session = { worker, pending: new Map(), quiet: false };
    worker.on('message', (msg: { id: number; ok: boolean; exitCode?: number; stdout?: string; stderr?: string; error?: string }) => {
      const p = session.pending.get(msg.id);
      if (!p) return;
      session.pending.delete(msg.id);
      clearTimeout(p.timer);
      if (msg.ok) {
        p.resolve({
          exitCode: msg.exitCode ?? -1,
          stdout: msg.stdout ?? '',
          stderr: msg.stderr ?? '',
        });
      } else {
        // a request-level failure (e.g. a read error): the worker is alive
        this.onIssue(`rho compiler request failed: ${msg.error ?? 'unknown error'}`);
        p.resolve({ exitCode: -1, stdout: '', stderr: '' });
      }
    });
    worker.on('error', (err: Error) => {
      session.quiet = true; // the error event already surfaces the failure
      this.failSession(session, `rho compiler worker crashed: ${err.message}`, false);
    });
    worker.on('exit', (code: number) => {
      if (this.session === session) this.session = null;
      this.failSession(session, `rho compiler worker exited (code ${code})`, session.quiet);
    });
    this.session = session;
    return session;
  }

  /** Resolve every in-flight request of one session as failed. quiet=true
   *  stays silent when nothing was in flight (an orderly stop is not an
   *  issue). */
  private failSession(session: Session, reason: string, quiet: boolean) {
    const dropped = session.pending.size;
    for (const [, p] of session.pending) {
      clearTimeout(p.timer);
      p.resolve({ exitCode: -1, stdout: '', stderr: '' });
    }
    session.pending.clear();
    if (reason && !(quiet && dropped === 0)) this.onIssue(reason);
  }

  private ensure(): Session {
    if (!this.session) return this.spawn();
    return this.session;
  }

  run(op: 'check' | 'fmt', source: string): Promise<CompilerOutcome> {
    return new Promise((resolve) => {
      const session = this.ensure();
      const id = this.nextId++;
      const timer = setTimeout(() => {
        // the worker is wedged: abandon it and let the next request respawn
        session.pending.delete(id);
        this.abandon(`rho compiler timed out after ${this.timeoutMs}ms — worker restarted`);
        resolve({ exitCode: -1, stdout: '', stderr: '' });
      }, this.timeoutMs);
      session.pending.set(id, { resolve, timer });
      try {
        session.worker.postMessage({ id, op, source });
      } catch (e) {
        clearTimeout(timer);
        session.pending.delete(id);
        this.onIssue(`rho compiler worker unreachable: ${e instanceof Error ? e.message : String(e)}`);
        resolve({ exitCode: -1, stdout: '', stderr: '' });
      }
    });
  }

  /** Drop the current worker without a graceful exit; a fresh one is
   *  spawned lazily on the next request. */
  private abandon(reason: string) {
    const session = this.session;
    this.session = null;
    if (session) {
      session.quiet = true;
      void session.worker.terminate().catch(() => {});
    }
    if (reason) this.onIssue(reason);
  }

  async dispose(): Promise<void> {
    const session = this.session;
    this.session = null;
    if (session) {
      session.quiet = true;
      this.failSession(session, '', true);
      await session.worker.terminate().catch(() => {});
    }
  }

  crashForTest(): void {
    this.abandon('');
  }
}

// ------------------------------------------------------- native backend --

class NativeCliHost {
  private dir: string | null = null;

  constructor(
    private readonly binaryPath: string,
    private readonly onIssue: (message: string) => void,
  ) {}

  private tempDir(): string | null {
    if (this.dir) return this.dir;
    try {
      this.dir = mkdtempSync(join((process.env.TMPDIR ?? '/tmp'), 'rho-lsp-'));
      return this.dir;
    } catch (e) {
      this.onIssue(`rho-lsp: cannot create temp dir for native CLI: ${e instanceof Error ? e.message : String(e)}`);
      return null;
    }
  }

  run(op: 'check' | 'fmt', source: string): Promise<CompilerOutcome> {
    return new Promise((resolve) => {
      const dir = this.tempDir();
      if (!dir) {
        resolve({ exitCode: -1, stdout: '', stderr: '' });
        return;
      }
      const file = join(dir, 'main.rho');
      try {
        writeFileSync(file, source, 'utf8');
      } catch (e) {
        this.onIssue(`rho-lsp: cannot write temp file: ${e instanceof Error ? e.message : String(e)}`);
        resolve({ exitCode: -1, stdout: '', stderr: '' });
        return;
      }
      const child = spawn(this.binaryPath, [op, file], { stdio: ['ignore', 'pipe', 'pipe'] });
      let stdout = '';
      let stderr = '';
      child.stdout.on('data', (d: Buffer) => (stdout += d.toString('utf8')));
      child.stderr.on('data', (d: Buffer) => (stderr += d.toString('utf8')));
      child.on('error', (err: Error) => {
        this.onIssue(`rho-lsp: cannot run ${this.binaryPath}: ${err.message}`);
        resolve({ exitCode: -1, stdout: '', stderr: '' });
      });
      child.on('close', (code: number | null) => {
        resolve({ exitCode: code ?? -1, stdout, stderr });
      });
    });
  }

  dispose(): Promise<void> {
    if (this.dir) {
      try {
        rmSync(this.dir, { recursive: true, force: true });
      } catch {
        // best effort cleanup
      }
      this.dir = null;
    }
    return Promise.resolve();
  }

  crashForTest(): void {}
}

// ---------------------------------------------------------- inert backend --

class NoCompiler implements Compiler {
  readonly backend = 'none' as const;
  readonly wasmPath = null;
  readonly binaryPath = null;
  private reported = false;

  private fail(): CompilerOutcome {
    if (!this.reported) {
      this.reported = true;
    }
    return { exitCode: -1, stdout: '', stderr: '' };
  }

  check(): Promise<CompilerOutcome> {
    return Promise.resolve(this.fail());
  }
  fmt(): Promise<CompilerOutcome> {
    return Promise.resolve(this.fail());
  }
  dispose(): Promise<void> {
    return Promise.resolve();
  }
  crashForTest(): void {}
}

// --------------------------------------------------------------- factory --

function inert(onIssue: (message: string) => void, reason: string): Compiler {
  onIssue(reason);
  return new NoCompiler();
}

/** Build the best available compiler backend for this environment. */
export function createCompiler(options: CompilerOptions = {}): Compiler {
  const onIssue = options.onIssue ?? (() => {});
  const timeoutMs = options.timeoutMs ?? DEFAULT_TIMEOUT_MS;

  // RHO_LSP_BACKEND forces one backend (wasm | native | none); a forced
  // backend that cannot be honored degrades to inert with a logged reason.
  const forced = normalizeBackend(options.backend ?? backendFromEnv());

  const wantWasm = forced === null || forced === 'wasm-worker';
  const wantNative = forced === null || forced === 'native-cli';

  if (wantWasm) {
    const wasmPath = resolveCompilerWasm();
    if (wasmPath) {
      const host = new WasmWorkerHost(wasmPath, onIssue, timeoutMs);
      return {
        backend: 'wasm-worker',
        wasmPath,
        binaryPath: null,
        check: (source) => host.run('check', source),
        fmt: (source) => host.run('fmt', source),
        dispose: () => host.dispose(),
        crashForTest: () => host.crashForTest(),
      };
    }
    if (forced === 'wasm-worker') {
      return inert(onIssue, 'rho-lsp: RHO_LSP_BACKEND=wasm but no compiler wasm found');
    }
  }

  if (wantNative) {
    const binaryPath = resolveNativeBinary();
    if (binaryPath) {
      const host = new NativeCliHost(binaryPath, onIssue);
      return {
        backend: 'native-cli',
        wasmPath: null,
        binaryPath,
        check: (source) => host.run('check', source),
        fmt: (source) => host.run('fmt', source),
        dispose: () => host.dispose(),
        crashForTest: () => host.crashForTest(),
      };
    }
    if (forced === 'native-cli') {
      return inert(onIssue, 'rho-lsp: RHO_LSP_BACKEND=native but no rho binary found');
    }
  }

  return inert(
    onIssue,
    'rho-lsp: no rho compiler found (looked for build/rho.wasm and ' +
      'site/assets/rho.wasm next to the package, RHO_LSP_WASM, and a ' +
      'native `rho` on PATH) — running without diagnostics',
  );
}

function backendFromEnv(): CompilerBackend | 'wasm' | 'native' | null {
  switch (process.env.RHO_LSP_BACKEND) {
    case 'wasm':
    case 'wasm-worker':
      return 'wasm';
    case 'native':
    case 'native-cli':
      return 'native';
    case 'none':
      return 'none';
    default:
      return null;
  }
}

/** Accept both the long backend ids and the short env spellings. */
function normalizeBackend(
  value: CompilerOptions['backend'] | null,
): CompilerBackend | null {
  switch (value) {
    case 'wasm':
      return 'wasm-worker';
    case 'native':
      return 'native-cli';
    case 'wasm-worker':
    case 'native-cli':
    case 'none':
      return value;
    default:
      return null;
  }
}
