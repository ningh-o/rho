// Minimal WASI preview1 shim for running the rho compiler in-process on
// node. The syscall semantics are ported from the proven browser shim
// (rho/site/assets/wasi.js, mirrored in apps/rho.ningh.org/app/rho/wasi.ts):
// the compiler needs args, an in-memory filesystem (read /main.rho, write
// /out.wasm), fd_write for stdout/stderr, and little else. Everything is
// synchronous — the modules we load import no memory and never suspend, so
// instantiation always completes without awaiting imports.

/** A tiny in-memory filesystem: absolute path -> bytes (or null for dirs). */
export interface MemFS {
  write(path: string, data: Uint8Array): void;
  read(path: string): Uint8Array | null;
  exists(path: string): boolean;
  unlink(path: string): void;
}

export function createFS(): MemFS {
  const root = new Map<string, Uint8Array | null>();
  root.set('/', null); // "/" is a dir; entries keyed by absolute path
  const norm = (p: string): string => {
    const parts: string[] = [];
    for (const seg of String(p).split('/')) {
      if (!seg || seg === '.') continue;
      if (seg === '..') parts.pop();
      else parts.push(seg);
    }
    return '/' + parts.join('/');
  };
  return {
    write(path, data) {
      root.set(norm(path), new Uint8Array(data));
    },
    read(path) {
      return root.get(norm(path)) ?? null;
    },
    exists(path) {
      return root.has(norm(path));
    },
    unlink(path) {
      root.delete(norm(path));
    },
  };
}

const ERRNO = {
  SUCCESS: 0,
  NOENT: 44,
  BADF: 8,
  INVAL: 28,
} as const;

export interface WasiOptions {
  args?: string[];
  fs?: MemFS | null;
}

export interface WasiHost {
  /** The import object for WebAssembly.instantiate. Function parameters are
   *  typed `any` at this boundary on purpose: the concrete per-call
   *  signatures are asserted into shape here, and strict contravariance
   *  would otherwise reject the varargs-shaped preview1 record. */
  imports: { wasi_snapshot_preview1: Record<string, (...a: any[]) => number> };
  setMemory(mem: WebAssembly.Memory): void;
  readonly exitCode: number | null;
  takeOutput(): { stdout: string; stderr: string };
}

/** Thrown by proc_exit and caught by runWasm; never escapes the shim. */
class WasiExit extends Error {}

const EXIT = new WasiExit('wasi exit');

export function createWasi(options: WasiOptions): WasiHost {
  const args = options.args ?? [];
  const fs = options.fs ?? null;
  let exited: number | null = null;
  const outBuf: string[] = [];
  const errBuf: string[] = [];

  let memory: WebAssembly.Memory | null = null;
  let view: DataView | null = null;
  function setMemory(mem: WebAssembly.Memory) {
    memory = mem;
    view = new DataView(mem.buffer);
  }
  // re-create the view when memory grows (it detaches the old buffer)
  function checkView() {
    if (!memory) throw new Error('wasi: memory not set');
    if (!view || view.buffer.byteLength !== memory.buffer.byteLength) {
      view = new DataView(memory.buffer);
    }
  }

  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  function readStr(ptr: number, len: number): string {
    checkView();
    return decoder.decode(new Uint8Array(memory!.buffer, ptr, len));
  }
  function writeStr(ptr: number, s: string) {
    checkView();
    new Uint8Array(memory!.buffer, ptr, s.length).set(encoder.encode(s));
  }

  const openFiles = new Map<number, { path: string; pos: number }>();
  let nextFd = 4; // 0-2 std streams, 3 the preopen "/"

  const preview1 = {
    args_sizes_get(argcPtr: number, argvBufSizePtr: number): number {
      checkView();
      let total = 0;
      for (const a of args) total += encoder.encode(a).length + 1;
      view!.setUint32(argcPtr, args.length, true);
      view!.setUint32(argvBufSizePtr, total, true);
      return ERRNO.SUCCESS;
    },
    args_get(argvPtr: number, argvBufPtr: number): number {
      checkView();
      for (let i = 0; i < args.length; i++) {
        view!.setUint32(argvPtr + i * 4, argvBufPtr, true);
        writeStr(argvBufPtr, args[i] + '\0');
        argvBufPtr += args[i].length + 1;
      }
      return ERRNO.SUCCESS;
    },
    environ_sizes_get(envCountPtr: number, envBufSizePtr: number): number {
      checkView();
      view!.setUint32(envCountPtr, 0, true);
      view!.setUint32(envBufSizePtr, 0, true);
      return ERRNO.SUCCESS;
    },
    environ_get(): number {
      return ERRNO.SUCCESS;
    },
    clock_time_get(_clockId: number, _precision: number, timePtr: number): number {
      checkView();
      view!.setBigUint64(timePtr, BigInt(Math.floor(performance.now() * 1e6)), true);
      return ERRNO.SUCCESS;
    },
    clock_res_get(_clockId: number, resolutionPtr: number): number {
      checkView();
      view!.setBigUint64(resolutionPtr, 1000n, true);
      return ERRNO.SUCCESS;
    },
    random_get(bufPtr: number, bufLen: number): number {
      checkView();
      // globalThis.crypto (node >= 19 webcrypto) provides getRandomValues
      crypto.getRandomValues(new Uint8Array(memory!.buffer, bufPtr, bufLen));
      return ERRNO.SUCCESS;
    },
    proc_exit(code: number): never {
      exited = code >>> 0;
      throw EXIT;
    },
    fd_write(fd: number, iovsPtr: number, iovsLen: number, nwrittenPtr: number): number {
      checkView();
      let written = 0;
      for (let i = 0; i < iovsLen; i++) {
        const ptr = view!.getUint32(iovsPtr + i * 8, true);
        const len = view!.getUint32(iovsPtr + i * 8 + 4, true);
        if (len === 0) continue;
        if (fd === 1) {
          outBuf.push(readStr(ptr, len));
        } else if (fd === 2) {
          errBuf.push(readStr(ptr, len));
        } else {
          // a file opened via path_open: append the raw bytes
          const entry = openFiles.get(fd);
          if (!entry) return ERRNO.BADF;
          const prev = fs?.read(entry.path) ?? new Uint8Array(0);
          const next = new Uint8Array(prev.length + len);
          next.set(prev, 0);
          next.set(new Uint8Array(memory!.buffer, ptr, len), prev.length);
          fs?.write(entry.path, next);
        }
        written += len;
      }
      view!.setUint32(nwrittenPtr, written, true);
      return ERRNO.SUCCESS;
    },
    fd_read(fd: number, iovsPtr: number, iovsLen: number, nreadPtr: number): number {
      checkView();
      view!.setUint32(nreadPtr, 0, true);
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      const bytes = fs?.read(entry.path) ?? new Uint8Array(0);
      let total = 0;
      for (let i = 0; i < iovsLen && entry.pos < bytes.length; i++) {
        const ptr = view!.getUint32(iovsPtr + i * 8, true);
        const len = view!.getUint32(iovsPtr + i * 8 + 4, true);
        const n = Math.min(len, bytes.length - entry.pos);
        new Uint8Array(memory!.buffer, ptr, n).set(bytes.subarray(entry.pos, entry.pos + n));
        entry.pos += n;
        total += n;
      }
      view!.setUint32(nreadPtr, total, true);
      return ERRNO.SUCCESS;
    },
    fd_close(): number {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_get(fd: number, statPtr: number): number {
      checkView();
      // character device for 0-2, regular file otherwise
      const isTty = fd <= 2;
      view!.setUint8(statPtr, isTty ? 2 : 4);
      view!.setUint16(statPtr + 2, 0, true);
      view!.setUint32(statPtr + 8, 0, true);
      view!.setBigUint64(statPtr + 16, BigInt(isTty ? 0 : 0o600), true);
      view!.setBigUint64(statPtr + 24, BigInt(isTty ? 0 : 0o600), true);
      return ERRNO.SUCCESS;
    },
    fd_seek(fd: number, offsetBig: bigint, whence: number, newOffsetPtr: number): number {
      checkView();
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      const bytes = fs?.read(entry.path) ?? new Uint8Array(0);
      const offset = Number(offsetBig);
      let pos = entry.pos;
      if (whence === 0) pos = offset;
      else if (whence === 1) pos += offset;
      else pos = bytes.length + offset;
      if (pos < 0) return ERRNO.INVAL;
      entry.pos = pos;
      view!.setBigUint64(newOffsetPtr, BigInt(pos), true);
      return ERRNO.SUCCESS;
    },
    fd_prestat_get(fd: number, prestatPtr: number): number {
      checkView();
      if (fd === 3 && fs) {
        view!.setUint8(prestatPtr, 0); // tag: prestat_dir
        view!.setUint32(prestatPtr + 4, 1, true); // path len of "/"
        return ERRNO.SUCCESS;
      }
      return ERRNO.BADF; // end of preopens
    },
    fd_prestat_dir_name(fd: number, pathPtr: number, _pathLen: number): number {
      if (fd === 3 && fs) {
        writeStr(pathPtr, '/');
        return ERRNO.SUCCESS;
      }
      return ERRNO.BADF;
    },
    path_open(
      _dirfd: number,
      _dirflags: number,
      pathPtr: number,
      pathLen: number,
      oflags: number,
      _rightsBase: bigint,
      _rightsInheriting: bigint,
      _fdflags: number,
      openedFdPtr: number,
    ): number {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith('/') ? path : '/' + path;
      const creat = (oflags & 1) !== 0;
      const trunc = (oflags & 8) !== 0;
      if (!fs.exists(abs)) {
        if (!creat) return ERRNO.NOENT;
        fs.write(abs, new Uint8Array(0));
      } else if (trunc) {
        fs.write(abs, new Uint8Array(0));
      }
      const fd = nextFd++;
      openFiles.set(fd, { path: abs, pos: 0 });
      view!.setUint32(openedFdPtr, fd, true);
      return ERRNO.SUCCESS;
    },
    path_filestat_get(
      _dirfd: number,
      _flags: number,
      pathPtr: number,
      pathLen: number,
      bufPtr: number,
    ): number {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith('/') ? path : '/' + path;
      if (!fs.exists(abs)) return ERRNO.NOENT;
      // filestat: dev, ino, filetype(4=regular), nlink, size, atime...
      view!.setBigUint64(bufPtr, 0n, true);
      view!.setBigUint64(bufPtr + 8, 0n, true);
      view!.setUint8(bufPtr + 16, 4);
      view!.setBigUint64(bufPtr + 24, 1n, true);
      const bytes = fs.read(abs);
      view!.setBigUint64(bufPtr + 40, BigInt(bytes ? bytes.length : 0), true);
      for (let off = 48; off < 96; off += 8) view!.setBigUint64(bufPtr + off, 0n, true);
      return ERRNO.SUCCESS;
    },
    fd_filestat_get(fd: number, bufPtr: number): number {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      const bytes = fs.read(entry.path);
      view!.setBigUint64(bufPtr, 0n, true);
      view!.setBigUint64(bufPtr + 8, 0n, true);
      view!.setUint8(bufPtr + 16, 4);
      view!.setBigUint64(bufPtr + 24, 1n, true);
      view!.setBigUint64(bufPtr + 40, BigInt(bytes ? bytes.length : 0), true);
      for (let off = 48; off < 96; off += 8) view!.setBigUint64(bufPtr + off, 0n, true);
      return ERRNO.SUCCESS;
    },
    path_unlink_file(_dirfd: number, pathPtr: number, pathLen: number): number {
      if (!fs) return ERRNO.NOENT;
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith('/') ? path : '/' + path;
      if (!fs.exists(abs)) return ERRNO.NOENT;
      fs.unlink(abs);
      return ERRNO.SUCCESS;
    },
    fd_sync(): number {
      return ERRNO.SUCCESS;
    },
    fd_datasync(): number {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_flags(): number {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_rights(): number {
      return ERRNO.SUCCESS;
    },
    fd_advise(): number {
      return ERRNO.SUCCESS;
    },
    path_create_directory(): number {
      return ERRNO.SUCCESS;
    },
  };

  return {
    imports: { wasi_snapshot_preview1: preview1 },
    setMemory,
    get exitCode() {
      return exited;
    },
    takeOutput() {
      const stdout = outBuf.join('');
      const stderr = errBuf.join('');
      outBuf.length = 0;
      errBuf.length = 0;
      return { stdout, stderr };
    },
  };
}

export interface RunResult {
  exitCode: number;
  stdout: string;
  stderr: string;
}

/** Instantiate a wasm module with this shim and run `_start`. A normal
 *  `_start` return is exit 0 (wasi-sdk programs only call proc_exit for a
 *  non-zero status). Never throws for a normal compiler run — a nonzero
 *  exit is an outcome, not an exception. */
export async function runWasm(bytes: Uint8Array, options: WasiOptions): Promise<RunResult> {
  return runModule(await WebAssembly.compile(bytes), options);
}

/** Same as runWasm, but takes a pre-compiled module (the compiler module is
 *  compiled once per process and instantiated per request). */
export function runModule(module: WebAssembly.Module, options: WasiOptions): RunResult {
  const wasi = createWasi(options);
  const instance = new WebAssembly.Instance(module, wasi.imports);
  const exports = instance.exports as Record<string, unknown>;
  const mem = exports.memory as WebAssembly.Memory | undefined;
  if (mem) wasi.setMemory(mem);
  try {
    (exports._start as () => void)();
  } catch (e) {
    if (!(e instanceof WasiExit)) throw e;
  }
  const { stdout, stderr } = wasi.takeOutput();
  return { exitCode: wasi.exitCode ?? 0, stdout, stderr };
}
