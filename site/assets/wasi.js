// Minimal WASI preview1 shim for the rho playground.
//
// Two roles:
//  - run the rho compiler itself (needs args + an in-memory filesystem:
//    read /main.rho, write /out.wasm)
//  - run the compiled rho program (needs fd_write + proc_exit only)
//
// Everything is synchronous: the modules we load are compiled with no
// async imports, so instantiation never suspends.

// A tiny in-memory filesystem: Map<path, {bytes: Uint8Array|null (dir)}>.
export function createFS() {
  const root = new Map(); // "/" is a dir; entries keyed by absolute path
  root.set("/", { bytes: null });
  function norm(p) {
    const parts = [];
    for (const seg of String(p).split("/")) {
      if (!seg || seg === ".") continue;
      if (seg === "..") parts.pop();
      else parts.push(seg);
    }
    return "/" + parts.join("/");
  }
  return {
    norm,
    write(path, data) {
      root.set(norm(path), { bytes: new Uint8Array(data) });
    },
    read(path) {
      const e = root.get(norm(path));
      return e && e.bytes ? e.bytes : null;
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
  2: 8, // ENOENT -> noent
  BADF: 8,
  NOMEM: 48,
  FAULT: 21,
  NOTDIR: 54,
  ISDIR: 31,
  INVAL: 28,
  NOENT: 44,
  ACCESS: 63,
};

// fd table: 0 stdin, 1 stdout, 2 stderr, 3+ = preopen ("/"), then open files
export function createWasi({ args = [], fs = null, onStdout = null, onStderr = null, onExit = null, print = null }) {
  let exited = null;
  let outBuf = [];
  let errBuf = [];

  function flushText(fd) {
    const buf = fd === 1 ? outBuf : errBuf;
    if (!buf.length) return;
    const text = buf.join("");
    buf.length = 0;
    const sink = fd === 1 ? onStdout : onStderr;
    if (sink) sink(text);
    else if (print) print(fd === 1 ? "out" : "err", text);
  }

  let view = null;
  let memory = null;
  function setMemory(mem) {
    memory = mem;
    view = new DataView(mem.buffer);
  }
  // re-create the view when memory grows (detaches the old buffer)
  function checkView() {
    if (!view || view.buffer.byteLength !== memory.buffer.byteLength) {
      view = new DataView(memory.buffer);
    }
  }

  function readStr(ptr, len) {
    checkView();
    return new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len));
  }
  function writeStr(ptr, s) {
    checkView();
    new Uint8Array(memory.buffer, ptr, s.length).set(new TextEncoder().encode(s));
  }

  const openFiles = new Map(); // fd -> {path}
  let nextFd = 4;

  const imports = {
    args_sizes_get(argcPtr, argvBufSizePtr) {
      checkView();
      let total = 0;
      for (const a of args) total += new TextEncoder().encode(a).length + 1;
      view.setUint32(argcPtr, args.length, true);
      view.setUint32(argvBufSizePtr, total, true);
      return 0;
    },
    args_get(argvPtr, argvBufPtr) {
      checkView();
      for (let i = 0; i < args.length; i++) {
        view.setUint32(argvPtr + i * 4, argvBufPtr, true);
        writeStr(argvBufPtr, args[i] + "\0");
        argvBufPtr += args[i].length + 1;
      }
      return 0;
    },
    environ_sizes_get(envCountPtr, envBufSizePtr) {
      checkView();
      view.setUint32(envCountPtr, 0, true);
      view.setUint32(envBufSizePtr, 0, true);
      return 0;
    },
    environ_get() {
      return 0;
    },
    clock_time_get(clockId, precision, timePtr) {
      checkView();
      view.setBigUint64(timePtr, BigInt(Math.floor(performance.now() * 1e6)), true);
      return 0;
    },
    random_get(bufPtr, bufLen) {
      checkView();
      const bytes = new Uint8Array(memory.buffer, bufPtr, bufLen);
      crypto.getRandomValues(bytes);
      return 0;
    },
    proc_exit(code) {
      exited = code >>> 0;
      if (onExit) onExit(exited);
      throw new WasiExit(exited);
    },
    fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
      checkView();
      let written = 0;
      for (let i = 0; i < iovsLen; i++) {
        const ptr = view.getUint32(iovsPtr + i * 8, true);
        const len = view.getUint32(iovsPtr + i * 8 + 4, true);
        if (len === 0) continue;
        if (fd === 1) {
          outBuf.push(readStr(ptr, len));
        } else if (fd === 2) {
          errBuf.push(readStr(ptr, len));
        } else {
          // a file opened via path_open: append the raw bytes
          const entry = openFiles.get(fd);
          if (!entry) return ERRNO.BADF;
          const prev = fs.read(entry.path) || new Uint8Array(0);
          const next = new Uint8Array(prev.length + len);
          next.set(prev, 0);
          next.set(new Uint8Array(memory.buffer, ptr, len), prev.length);
          fs.write(entry.path, next);
        }
        written += len;
      }
      view.setUint32(nwrittenPtr, written, true);
      return 0;
    },
    fd_read(fd, iovsPtr, iovsLen, nreadPtr) {
      checkView();
      view.setUint32(nreadPtr, 0, true);
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      const bytes = fs.read(entry.path) || new Uint8Array(0);
      let total = 0;
      for (let i = 0; i < iovsLen && entry.pos < bytes.length; i++) {
        const ptr = view.getUint32(iovsPtr + i * 8, true);
        const len = view.getUint32(iovsPtr + i * 8 + 4, true);
        const n = Math.min(len, bytes.length - entry.pos);
        new Uint8Array(memory.buffer, ptr, n).set(bytes.subarray(entry.pos, entry.pos + n));
        entry.pos += n;
        total += n;
      }
      view.setUint32(nreadPtr, total, true);
      return 0;
    },
    fd_close() {
      return 0;
    },
    fd_fdstat_get(fd, statPtr) {
      checkView();
      // character device for 0-2, regular file otherwise
      const isTty = fd <= 2;
      view.setUint8(statPtr, isTty ? 2 : 4);
      view.setUint16(statPtr + 2, 0, true);
      view.setUint32(statPtr + 8, 0, true);
      view.setBigUint64(statPtr + 16, BigInt(isTty ? 0 : 0o600), true);
      view.setBigUint64(statPtr + 24, BigInt(isTty ? 0 : 0o600), true);
      return 0;
    },
    fd_seek(fd, offsetBig, whence, newOffsetPtr) {
      checkView();
      const entry = openFiles.get(fd);
      if (!entry) return ERRNO.BADF;
      const bytes = fs.read(entry.path) || new Uint8Array(0);
      const offset = Number(offsetBig);
      let pos = entry.pos;
      if (whence === 0) pos = offset; // SEEK_SET
      else if (whence === 1) pos += offset; // SEEK_CUR
      else pos = bytes.length + offset; // SEEK_END
      if (pos < 0) return 28; // INVAL
      entry.pos = pos;
      view.setBigUint64(newOffsetPtr, BigInt(pos), true);
      return 0;
    },
    fd_prestat_get(fd, prestatPtr) {
      checkView();
      if (fd === 3 && fs) {
        view.setUint8(prestatPtr, 0); // tag: prestat_dir
        view.setUint32(prestatPtr + 4, 1, true); // path len of "/"
        return 0;
      }
      return 8; // BADF: end of preopens
    },
    fd_prestat_dir_name(fd, pathPtr, pathLen) {
      if (fd === 3 && fs) {
        writeStr(pathPtr, "/");
        return 0;
      }
      return 8;
    },
    path_open(dirfd, dirflags, pathPtr, pathLen, oflags, rightsBase, rightsInheriting, fdflags, openedFdPtr) {
      if (!fs) return 44;
      checkView();
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith("/") ? path : "/" + path;
      const creat = (oflags & 1) !== 0;
      const trunc = (oflags & 8) !== 0;
      if (!fs.exists(abs)) {
        if (!creat) return 44;
        fs.write(abs, new Uint8Array(0));
      } else if (trunc) {
        fs.write(abs, new Uint8Array(0));
      }
      const fd = nextFd++;
      openFiles.set(fd, { path: abs, pos: 0 });
      view.setUint32(openedFdPtr, fd, true);
      return 0;
    },
    path_filestat_get(dirfd, flags, pathPtr, pathLen, bufPtr) {
      if (!fs) return 44;
      checkView();
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith("/") ? path : "/" + path;
      if (!fs.exists(abs)) return 44;
      // filestat: dev, ino, filetype(4=regular), nlink, size, atime...
      view.setBigUint64(bufPtr, 0n, true);
      view.setBigUint64(bufPtr + 8, 0n, true);
      view.setUint8(bufPtr + 16, 4);
      view.setBigUint64(bufPtr + 24, 1n, true);
      const bytes = fs.read(abs);
      view.setBigUint64(bufPtr + 40, BigInt(bytes ? bytes.length : 0), true);
      for (let off = 48; off < 96; off += 8) view.setBigUint64(bufPtr + off, 0n, true);
      return 0;
    },
    fd_filestat_get(fd, bufPtr) {
      if (!fs) return 44;
      checkView();
      const entry = openFiles.get(fd);
      if (!entry) return 8;
      const bytes = fs.read(entry.path);
      view.setBigUint64(bufPtr, 0n, true);
      view.setBigUint64(bufPtr + 8, 0n, true);
      view.setUint8(bufPtr + 16, 4);
      view.setBigUint64(bufPtr + 24, 1n, true);
      view.setBigUint64(bufPtr + 40, BigInt(bytes ? bytes.length : 0), true);
      for (let off = 48; off < 96; off += 8) view.setBigUint64(bufPtr + off, 0n, true);
      return 0;
    },
    path_unlink_file(dirfd, pathPtr, pathLen) {
      if (!fs) return 44;
      const path = readStr(pathPtr, pathLen);
      const abs = path.startsWith("/") ? path : "/" + path;
      if (!fs.exists(abs)) return 44;
      fs.unlink(abs);
      return 0;
    },
    fd_sync() {
      return 0;
    },
    fd_datasync() {
      return 0;
    },
    fd_fdstat_set_flags() {
      return 0;
    },
    fd_fdstat_set_rights() {
      return 0;
    },
    fd_advise() {
      return 0;
    },
    path_create_directory() {
      return 0;
    },
    clock_res_get(clockId, resolutionPtr) {
      checkView();
      view.setBigUint64(resolutionPtr, 1000n, true);
      return 0;
    },
  };

  return {
    wasi_snapshot_preview1: imports,
    setMemory,
    get exitCode() {
      return exited;
    },
  takeOutput() {
    // deliver whatever accumulated to the streaming callbacks, then drain
    const stdout = outBuf.join("");
    const stderr = errBuf.join("");
    outBuf.length = 0;
    errBuf.length = 0;
    if (onStdout && stdout) onStdout(stdout);
    if (onStderr && stderr) onStderr(stderr);
    return { stdout, stderr };
  },
  };
}

export class WasiExit extends Error {
  constructor(code) {
    super("WASI exit " + code);
    this.code = code;
  }
}

// Instantiate a wasm module (Uint8Array) with this shim and run _start.
// Returns { exitCode, stdout, stderr }. A normal _start return is exit 0
// (wasi-sdk programs only call proc_exit for non-zero status).
export async function runWasm(bytes, options) {
  const wasi = createWasi(options);
  const module = await WebAssembly.compile(bytes);
  const needsMemory = WebAssembly.Module.imports(module).some((i) => i.name === "memory" && i.module === "env");
  const imports = { wasi_snapshot_preview1: wasi.wasi_snapshot_preview1 };
  if (needsMemory) imports.env = { memory: options.memory };
  const instance = await WebAssembly.instantiate(module, imports);
  const mem = instance.exports.memory || options.memory;
  if (mem) wasi.setMemory(mem);
  try {
    instance.exports._start();
  } catch (e) {
    if (!(e instanceof WasiExit)) throw e;
  }
  const { stdout, stderr } = wasi.takeOutput();
  return { exitCode: wasi.exitCode ?? 0, stdout, stderr };
}
