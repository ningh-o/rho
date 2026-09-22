// Driver for the rho compiler: the real toolchain as a wasm32-wasi program
// (rho.wasm, cross-compiled from the rho repo's boot/ C sources).
//
// The browser playground (apps/rho.ningh.org/app/rho/compiler.ts) drives the
// exact same bytes with the exact same command lines; this driver is that
// pattern, packaged for build tools. All commands run against an in-memory
// filesystem: the sources are written in, the artifact is read back.

import { createFS, runWasm, type MemFS } from './wasi.js';

/** Where to find the compiler wasm. */
export type CompilerSource =
  | string // a filesystem path to rho.wasm (node)
  | Uint8Array // raw bytes, already loaded
  | { url: string | URL }; // any fetchable URL

export interface DriverOptions {
  compiler: CompilerSource;
  /** Working-directory root used to resolve a relative `compiler` path. */
  root?: string;
}

export class RhoCompileError extends Error {
  readonly stderr: string;
  constructor(message: string, stderr: string) {
    super(message);
    this.name = 'RhoCompileError';
    this.stderr = stderr;
  }
}

export interface Driver {
  /** rho check: parse + type-check. Throws RhoCompileError on failure. */
  check(path: string, fs: MemFS): Promise<void>;
  /** rho fmt: canonical formatting (the compiler's own typed surface). */
  fmt(path: string, fs: MemFS): Promise<string>;
  /** rho build --target wasm32-wasi: returns the compiled program bytes. */
  buildWasm(entry: string, fs: MemFS): Promise<Uint8Array>;
  /** The version string the compiler reports (`rho --version`). */
  version(): Promise<string>;
}

let cachedBytes: Uint8Array | null = null;
let cachedKey: string | null = null;

async function loadCompilerBytes(options: DriverOptions): Promise<Uint8Array> {
  const src = options.compiler;
  const key =
    typeof src === 'string'
      ? 'path:' + src
      : src instanceof Uint8Array
        ? 'bytes:' + src.length
        : 'url:' + String(src.url);
  if (cachedBytes && cachedKey === key) return cachedBytes;
  let bytes: Uint8Array;
  if (src instanceof Uint8Array) {
    bytes = src;
  } else if (typeof src === 'string') {
    // node: read from disk
    const { readFileSync } = await import('node:fs');
    const path = options.root
      ? (await import('node:path')).resolve(options.root, src)
      : src;
    bytes = new Uint8Array(readFileSync(path));
  } else {
    const res = await fetch(src.url);
    if (!res.ok) throw new Error(`rho.wasm: HTTP ${res.status} at ${src.url}`);
    bytes = new Uint8Array(await res.arrayBuffer());
  }
  cachedBytes = bytes;
  cachedKey = key;
  return bytes;
}

/** Create a compiler driver bound to one rho.wasm. */
export async function createDriver(options: DriverOptions): Promise<Driver> {
  const bytes = await loadCompilerBytes(options);

  async function run(args: string[], fs: MemFS): Promise<{ exitCode: number; stdout: string; stderr: string }> {
    const result = await runWasm(bytes, { args, fs });
    return result;
  }

  return {
    async check(path, fs) {
      const r = await run(['rho', 'check', path], fs);
      if (r.exitCode !== 0) {
        throw new RhoCompileError(`rho check failed for ${path}`, r.stderr.trim());
      }
    },
    async fmt(path, fs) {
      const r = await run(['rho', 'fmt', path], fs);
      if (r.exitCode !== 0) {
        throw new RhoCompileError(`rho fmt failed for ${path}`, r.stderr.trim());
      }
      return r.stdout;
    },
    async buildWasm(entry, fs) {
      const r = await run(
        ['rho', 'build', entry, '--target', 'wasm32-wasi', '-o', '/out.wasm'],
        fs,
      );
      const out = fs.read('/out.wasm');
      if (r.exitCode !== 0 || !out) {
        throw new RhoCompileError(`rho build failed for ${entry}`, r.stderr.trim());
      }
      fs.unlink('/out.wasm');
      return out;
    },
    async version() {
      const r = await run(['rho', '--version'], createFS());
      return r.stdout.trim() || r.stderr.trim();
    },
  };
}
