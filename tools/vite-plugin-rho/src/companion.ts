// Companion module template — the JS/TS surface generated for each .rho
// module. The template instantiates the compiled wasm with a minimal WASI
// shim and re-exports the module's `pub fn`s as real JS functions, with the
// ABI the plugin computed at build time (nonce fingerprints included).
//
// Runtime contract (validated by the probe in the package tests):
//   - scalars pass through: bool/i8..u32 -> i32, i64/u64/usize -> i64 (BigInt),
//     f32/f64 -> themselves;
//   - a string parameter is marshaled as a {buf, ptr, len} record in rho
//     memory whose `buf` points at an immortal rc header (top bit set) so the
//     callee's reference-count traffic no-ops on it;
//   - a string return arrives through a hidden leading out-pointer; the
//     record's {ptr, len} name the UTF-8 bytes, which are copied into a JS
//     string before the next call.

export interface CompanionExport {
  name: string;
  k: number;
  /** Parameter kinds in call order, including the hidden string-return out slot. */
  params: Array<'i32' | 'i64' | 'f32' | 'f64' | 'bool' | 'str' | 'raw'>;
  ret: 'void' | 'i32' | 'i64' | 'f32' | 'f64' | 'bool' | 'str' | 'raw';
  /** Whether the wasm signature takes a hidden leading out-pointer. */
  hiddenOut: boolean;
  /** Nonce literal arguments (already rendered as JS source). */
  nonceArgs: string;
}

const RUNTIME = `\
// ---- minimal WASI preview1 shim: what rho modules may ask for -----------
const shim = (() => {
  const enc = new TextEncoder();
  const decoder = new TextDecoder();
  let memory = null;
  const view = () => new DataView(memory.buffer);
  const sinks = { out: [], err: [] };
  const emit = (lines, fallback) => {
    if (lines.length) {
      const text = lines.join('');
      lines.length = 0;
      (fallback || console.log)(text);
    }
  };
  const badf = () => 8;
  const api = {
    fd_write: (fd, iovs, len, nwritten) => {
      const dv = view();
      let written = 0;
      const chunks = [];
      for (let i = 0; i < len; i++) {
        const p = dv.getUint32(iovs + i * 8, true);
        const l = dv.getUint32(iovs + i * 8 + 4, true);
        if (l) chunks.push(decoder.decode(new Uint8Array(memory.buffer, p, l)));
        written += l;
      }
      if (fd === 1) sinks.out.push(...chunks);
      else if (fd === 2) sinks.err.push(...chunks);
      else return badf();
      dv.setUint32(nwritten, written, true);
      return 0;
    },
    proc_exit: (code) => {
      throw new Error('rho program exited with code ' + (code >>> 0));
    },
    args_sizes_get: (a, b) => { view().setUint32(a, 0, true); view().setUint32(b, 0, true); return 0; },
    args_get: () => 0,
    environ_sizes_get: (a, b) => { view().setUint32(a, 0, true); view().setUint32(b, 0, true); return 0; },
    environ_get: () => 0,
    clock_time_get: (id, precision, out) => { view().setBigUint64(out, BigInt(Math.floor(Date.now() * 1e6)), true); return 0; },
    clock_res_get: (id, out) => { view().setBigUint64(out, 1000n, true); return 0; },
    random_get: (p, l) => { crypto.getRandomValues(new Uint8Array(memory.buffer, p, l)); return 0; },
    fd_close: () => 0,
    fd_fdstat_get: (fd, st) => { view().setUint8(st, fd <= 2 ? 2 : 4); return 0; },
    fd_read: badf,
    fd_seek: badf,
    path_open: () => 44,
    path_rename: () => 44,
    fd_prestat_get: () => 8,
    fd_filestat_get: () => 44,
    path_filestat_get: () => 44,
  };
  return {
    imports: { wasi_snapshot_preview1: api },
    setMemory: (m) => { memory = m; },
    flush: () => { emit(sinks.out); emit(sinks.err, console.error); },
  };
})();

// ---- instantiate once; scratch lives in pages we grow at the top --------
const bytes = await (await fetch(wasmUrl)).arrayBuffer();
const module = await WebAssembly.compile(bytes);
const { exports } = await WebAssembly.instantiate(module, shim.imports);
shim.setMemory(exports.memory);
const scratch = (() => {
  let bump;
  const top = () => exports.memory.buffer.byteLength;
  const start = () => { exports.memory.grow(1); bump = top() - 65536 + 64; };
  start();
  return {
    alloc: (n) => {
      if (bump + n + 16 > top()) start();
      const p = bump;
      bump = (bump + n + 15) & ~15;
      return p;
    },
  };
})();

const enc = new TextEncoder();
const decoder = new TextDecoder();
const IMMORTAL = 0x8000000000000000n; // rc header the callee will not touch

// Marshal a JS string as a borrowed rho string record; returns its address.
function rhoString(text) {
  const bytesText = enc.encode(text);
  const base = scratch.alloc(24 + 24 + bytesText.length);
  const dv = new DataView(exports.memory.buffer);
  dv.setBigUint64(base, IMMORTAL, true);          // buf: immortal rc header
  dv.setBigUint64(base + 24, BigInt(base), true); // record.buf -> header
  dv.setBigUint64(base + 32, BigInt(base + 48), true); // record.ptr -> bytes
  dv.setBigUint64(base + 40, BigInt(bytesText.length), true); // record.len
  new Uint8Array(exports.memory.buffer, base + 48, bytesText.length).set(bytesText);
  return base + 24;
}

// Read a rho string record produced by a call and copy it into a JS string.
function jsString(recordAddr) {
  const dv = new DataView(exports.memory.buffer);
  const ptr = Number(dv.getBigUint64(recordAddr + 8, true));
  const len = Number(dv.getBigUint64(recordAddr + 16, true));
  return decoder.decode(new Uint8Array(exports.memory.buffer, ptr, len));
}
`;

export function renderCompanion(exportFns: CompanionExport[]): string {
  const lines: string[] = [];
  lines.push(`// Generated by vite-plugin-rho from a .rho module — do not edit.`);
  lines.push(`import wasmUrl from __RHO_WASM_IMPORT__;`);
  lines.push('');
  lines.push(RUNTIME);
  lines.push('');
  for (const fn of exportFns) {
    const jsParams: string[] = [];
    const callArgs: string[] = [];
    let idx = 0;
    if (fn.hiddenOut) callArgs.push('outSlot'); // hidden out-ptr is wasm arg 0
    callArgs.push(fn.nonceArgs);
    for (const p of fn.params) {
      const argName = 'a' + idx++;
      jsParams.push(argName);
      if (p === 'str') callArgs.push(`rhoString(${argName})`);
      else callArgs.push(argName);
    }
    const needRet = fn.ret !== 'void' && fn.ret !== 'str' && !(fn.hiddenOut && fn.ret === 'raw');
    lines.push(`const wasmFn_${fn.k} = exports[${JSON.stringify('__rho_export_' + fn.k)}];`);
    lines.push(`export function ${fn.name}(${jsParams.join(', ')}) {`);
    if (fn.hiddenOut) lines.push(`  const outSlot = scratch.alloc(24);`);
    if (needRet) lines.push(`  const __ret = wasmFn_${fn.k}(${callArgs.join(', ')});`);
    else lines.push(`  wasmFn_${fn.k}(${callArgs.join(', ')});`);
    if (fn.ret === 'void') lines.push(`  shim.flush();`);
    else if (fn.ret === 'bool') lines.push(`  shim.flush();\n  return __ret !== 0;`);
    else if (fn.ret === 'str')
      lines.push(`  const __s = jsString(outSlot);\n  shim.flush();\n  return __s;`);
    else if (fn.hiddenOut && fn.ret === 'raw')
      lines.push(`  shim.flush();\n  return outSlot; // raw record: caller reads {buf, ptr, len}`);
    else lines.push(`  shim.flush();\n  return __ret;`);
    lines.push(`}`);
    lines.push('');
  }
  lines.push(`export { exports as rhoExports, exports as default };`);
  return lines.join('\n');
}

export function companionWithImport(source: string, importPath: string): string {
  return source.replace('__RHO_WASM_IMPORT__', importPath);
}
