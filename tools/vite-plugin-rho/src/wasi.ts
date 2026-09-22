// Minimal WASI preview1 shim for vite-plugin-rho.
//
// Ported from the rho playground's shim (apps/rho.ningh.org/app/rho/wasi.ts,
// itself a port of the rho repo's site/assets/wasi.js). It serves two roles,
// same as there:
//   - run the rho compiler itself (needs args + an in-memory filesystem:
//     read /main.rho, write /out.wasm),
//   - run/instantiate compiled rho programs (fd_write + proc_exit and
//     little else).
//
// Everything is synchronous: rho-emitted modules have no async imports, so
// instantiation never suspends.

/** A tiny in-memory filesystem: Map<path, bytes>. */
export interface MemFS {
  norm(p: string): string;
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
    norm,
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

const ERRNO = { SUCCESS: 0, NOENT: 44, BADF: 8, INVAL: 28 } as const;

export interface WasiOptions {
  args?: string[];
  fs?: MemFS | null;
  onStdout?: ((text: string) => void) | null;
  onStderr?: ((text: string) => void) | null;
}

export interface WasiHost {
  imports: Record<string, Record<string, (a: never, b?: never) => number>>;
  setMemory(mem: WebAssembly.Memory): void;
  readonly exitCode: number | null;
  takeOutput(): { stdout: string; stderr: string };
}

/** Thrown by proc_exit; runWasm catches it. */
export class WasiExit extends Error {
  readonly code: number;
  constructor(code: number) {
    super('WASI exit ' + code);
    this.code = code;
  }
}

export function createWasi(options: WasiOptions): WasiHost {
  const { args = [], fs = null } = options;
  const { onStdout = null, onStderr = null } = options;
  let exited: number | null = null;
  const outBuf: string[] = [];
  const errBuf: string[] = [];

  let view: DataView | null = null;
  let memory: WebAssembly.Memory | null = null;
  function setMemory(mem: WebAssembly.Memory) {
    memory = mem;
    view = new DataView(mem.buffer);
  }
  // re-create the view when memory grows (detaches the old buffer)
  function checkView() {
    if (!view || !memory || view.buffer.byteLength !== memory.buffer.byteLength) {
      if (!memory) throw new Error('wasi: memory not set');
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
  let nextFd = 4;

  const preview1 = {
    args_sizes_get(argcPtr: number, argvBufSizePtr: number) {
      checkView();
      let total = 0;
      for (const a of args) total += encoder.encode(a).length + 1;
      view!.setUint32(argcPtr, args.length, true);
      view!.setUint32(argvBufSizePtr, total, true);
      return ERRNO.SUCCESS;
    },
    args_get(argvPtr: number, argvBufPtr: number) {
      checkView();
      for (let i = 0; i < args.length; i++) {
        view!.setUint32(argvPtr + i * 4, argvBufPtr, true);
        writeStr(argvBufPtr, args[i] + '\0');
        argvBufPtr += args[i].length + 1;
      }
      return ERRNO.SUCCESS;
    },
    environ_sizes_get(envCountPtr: number, envBufSizePtr: number) {
      checkView();
      view!.setUint32(envCountPtr, 0, true);
      view!.setUint32(envBufSizePtr, 0, true);
      return ERRNO.SUCCESS;
    },
    environ_get() {
      return ERRNO.SUCCESS;
    },
    clock_time_get(_clockId: number, _precision: number, timePtr: number) {
      checkView();
      view!.setBigUint64(timePtr, BigInt(Math.floor(Date.now() * 1e6)), true);
      return ERRNO.SUCCESS;
    },
    clock_res_get(_clockId: number, resolutionPtr: number) {
      checkView();
      view!.setBigUint64(resolutionPtr, 1000n, true);
      return ERRNO.SUCCESS;
    },
    random_get(bufPtr: number, bufLen: number) {
      checkView();
      const bytes = new Uint8Array(memory!.buffer, bufPtr, bufLen);
      crypto.getRandomValues(bytes);
      return ERRNO.SUCCESS;
    },
    proc_exit(code: number): number {
      exited = code >>> 0;
      throw new WasiExit(exited);
    },
    fd_write(fd: number, iovsPtr: number, iovsLen: number, nwrittenPtr: number) {
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
    fd_read(fd: number, iovsPtr: number, iovsLen: number, nreadPtr: number) {
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
    fd_close() {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_get(fd: number, statPtr: number) {
      checkView();
      // character device for 0-2, regular file otherwise
      const isTty = fd <= 2;
      view!.setUint8(statPtr, isTty ? 2 : 4);
      view!.setUint16(statPtr + 2, 0, true);
      view!.setUint32(statPtr + 8, 0, true);
      view!.setBigUint64(statPtr + 16, 0n, true);
      view!.setBigUint64(statPtr + 24, BigInt(isTty ? 0 : 0o600), true);
      return ERRNO.SUCCESS;
    },
    fd_seek(fd: number, offsetBig: bigint, whence: number, newOffsetPtr: number) {
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
    fd_prestat_get(fd: number, prestatPtr: number) {
      checkView();
      if (fd === 3 && fs) {
        view!.setUint8(prestatPtr, 0); // tag: prestat_dir
        view!.setUint32(prestatPtr + 4, 1, true); // path len of "/"
        return ERRNO.SUCCESS;
      }
      return ERRNO.BADF; // end of preopens
    },
    fd_prestat_dir_name(fd: number, pathPtr: number) {
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
    ) {
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
    path_filestat_get(_dirfd: number, _flags: number, pathPtr: number, pathLen: number, bufPtr: number) {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith('/') ? path : '/' + path;
      if (!fs.exists(abs)) return ERRNO.NOENT;
      return statBody(fs.read(abs)?.length ?? 0, bufPtr);
    },
    fd_filestat_get(fd: number, bufPtr: number) {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      return statBody(fs.read(entry.path)?.length ?? 0, bufPtr);
    },
    path_unlink_file(_dirfd: number, pathPtr: number, pathLen: number) {
      if (!fs) return ERRNO.NOENT;
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith('/') ? path : '/' + path;
      if (!fs.exists(abs)) return ERRNO.NOENT;
      fs.unlink(abs);
      return ERRNO.SUCCESS;
    },
    // two-fd preview1 form (old_fd, old, old_len, new_fd, new, new_len) —
    // the shape the rho prelude's tmp+rename write path calls with
    path_rename(
      _oldFd: number,
      oldPathPtr: number,
      oldPathLen: number,
      _newFd: number,
      newPathPtr: number,
      newPathLen: number,
    ) {
      if (!fs) return ERRNO.NOENT;
      checkView();
      const oldPath = readStr(oldPathPtr, oldPathLen);
      const absOld = oldPath.startsWith('/') ? oldPath : '/' + oldPath;
      const data = fs.read(absOld);
      if (!data) return ERRNO.NOENT;
      const newPath = readStr(newPathPtr, newPathLen);
      const absNew = newPath.startsWith('/') ? newPath : '/' + newPath;
      fs.write(absNew, data);
      fs.unlink(absOld);
      return ERRNO.SUCCESS;
    },
    fd_sync() {
      return ERRNO.SUCCESS;
    },
    fd_datasync() {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_flags() {
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_rights() {
      return ERRNO.SUCCESS;
    },
    fd_advise() {
      return ERRNO.SUCCESS;
    },
    path_create_directory() {
      return ERRNO.SUCCESS;
    },
  };

  // 96-byte wasm32 filestat body: dev, ino, filetype(4=regular), nlink, size...
  function statBody(size: number, bufPtr: number): number {
    view!.setBigUint64(bufPtr, 0n, true);
    view!.setBigUint64(bufPtr + 8, 0n, true);
    view!.setUint8(bufPtr + 16, 4);
    view!.setBigUint64(bufPtr + 24, 1n, true);
    view!.setBigUint64(bufPtr + 40, BigInt(size), true);
    for (let off = 48; off < 96; off += 8) view!.setBigUint64(bufPtr + off, 0n, true);
    return ERRNO.SUCCESS;
  }

  return {
    imports: {
      wasi_snapshot_preview1: preview1 as unknown as Record<
        string,
        (a: never, b?: never) => number
      >,
    },
    setMemory,
    get exitCode() {
      return exited;
    },
    takeOutput() {
      const stdout = outBuf.join('');
      const stderr = errBuf.join('');
      outBuf.length = 0;
      errBuf.length = 0;
      if (onStdout && stdout) onStdout(stdout);
      if (onStderr && stderr) onStderr(stderr);
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
 *  `_start` return is exit 0 (wasi-sdk programs only proc_exit non-zero). */
export async function runWasm(
  bytes: Uint8Array,
  options: WasiOptions,
): Promise<RunResult> {
  const wasi = createWasi(options);
  const module = await WebAssembly.compile(bytes as unknown as BufferSource);
  const instance = await WebAssembly.instantiate(
    module,
    wasi.imports as unknown as WebAssembly.Imports,
  );
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
