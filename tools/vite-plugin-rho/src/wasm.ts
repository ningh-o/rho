// WebAssembly section reader/writer — just enough to (a) read the type,
// import, function and export sections of a rho-emitted module and (b) splice
// extra function exports into the export section. rho modules are plain
// wasm32 with no custom sections; the reader relies on nothing beyond the
// core spec (LEB128 + section framing).

export type ValType = 'i32' | 'i64' | 'f32' | 'f64';

export interface FuncType {
  params: ValType[];
  ret: ValType | null;
}

export interface WasmExport {
  name: string;
  kind: number; // 0 func, 1 table, 2 memory, 3 global
  index: number;
}

export interface ParsedModule {
  types: FuncType[];
  /** Number of imported functions (they occupy the front of the index space). */
  importedFuncs: number;
  /** Type index per defined function, in index order. */
  funcTypeIndices: number[];
  exports: WasmExport[];
}

function readUleb(bytes: Uint8Array, off: number): { n: number; next: number } {
  let result = 0n;
  let shift = 0n;
  let o = off;
  for (;;) {
    const b = bytes[o++];
    result |= BigInt(b & 0x7f) << shift;
    if ((b & 0x80) === 0) break;
    shift += 7n;
  }
  return { n: Number(result), next: o };
}

interface RawSection {
  id: number;
  start: number; // payload start
  size: number; // payload size
  headerStart: number; // includes the id byte
}

export function parseSections(bytes: Uint8Array): RawSection[] {
  if (
    bytes[0] !== 0x00 || bytes[1] !== 0x61 || bytes[2] !== 0x73 || bytes[3] !== 0x6d
  ) {
    throw new Error('not a wasm module (bad magic)');
  }
  let o = 8;
  const sections: RawSection[] = [];
  while (o < bytes.length) {
    const headerStart = o;
    const id = bytes[o++];
    const { n, next } = readUleb(bytes, o);
    sections.push({ id, start: next, size: n, headerStart });
    o = next + n;
  }
  return sections;
}

const VT: Record<number, ValType> = {
  0x7f: 'i32',
  0x7e: 'i64',
  0x7d: 'f32',
  0x7c: 'f64',
};

function parseTypeSection(bytes: Uint8Array, sec: RawSection): FuncType[] {
  let o = sec.start;
  const { n: count, next } = readUleb(bytes, o);
  o = next;
  const types: FuncType[] = [];
  for (let i = 0; i < count; i++) {
    if (bytes[o++] !== 0x60) throw new Error('type section: non-func type');
    const p = readUleb(bytes, o);
    o = p.next;
    const params: ValType[] = [];
    for (let k = 0; k < p.n; k++) {
      const vt = VT[bytes[o++]];
      if (!vt) throw new Error('type section: unsupported value type');
      params.push(vt);
    }
    const r = readUleb(bytes, o);
    o = r.next;
    const ret: ValType | null = r.n === 1 ? VT[bytes[o++]] : null;
    types.push({ params, ret });
  }
  return types;
}

function parseFuncSection(bytes: Uint8Array, sec: RawSection): number[] {
  let o = sec.start;
  const { n: count, next } = readUleb(bytes, o);
  o = next;
  const idx: number[] = [];
  for (let i = 0; i < count; i++) {
    const t = readUleb(bytes, o);
    o = t.next;
    idx.push(t.n);
  }
  return idx;
}

/** Count imported functions while walking past every import entry. */
function countImportedFuncs(bytes: Uint8Array, sec: RawSection): number {
  let o = sec.start;
  const { n: count, next } = readUleb(bytes, o);
  o = next;
  let funcs = 0;
  for (let i = 0; i < count; i++) {
    const m = readUleb(bytes, o);
    o = m.next + m.n;
    const nm = readUleb(bytes, o);
    o = nm.next + nm.n;
    const kind = bytes[o++];
    if (kind === 0x00) {
      funcs++;
      const t = readUleb(bytes, o);
      o = t.next;
    } else if (kind === 0x01) {
      o += 1; // elemtype
      const lim = readUleb(bytes, o);
      o = lim.next;
    } else if (kind === 0x02) {
      o += 1; // memtype limits flag
      const lim = readUleb(bytes, o);
      o = lim.next;
    } else if (kind === 0x03) {
      o += 1;
      const lim = readUleb(bytes, o);
      o = lim.next;
    } else {
      throw new Error('import section: unknown kind ' + kind);
    }
  }
  return funcs;
}

function parseExportSection(bytes: Uint8Array, sec: RawSection): WasmExport[] {
  let o = sec.start;
  const { n: count, next } = readUleb(bytes, o);
  o = next;
  const list: WasmExport[] = [];
  const decoder = new TextDecoder();
  for (let i = 0; i < count; i++) {
    const l = readUleb(bytes, o);
    o = l.next;
    const name = decoder.decode(bytes.subarray(o, o + l.n));
    o += l.n;
    const kind = bytes[o++];
    const ix = readUleb(bytes, o);
    o = ix.next;
    list.push({ name, kind, index: ix.n });
  }
  return list;
}

export function parseModule(bytes: Uint8Array): ParsedModule {
  const sections = parseSections(bytes);
  const find = (id: number) => sections.find((s) => s.id === id);
  const typeSec = find(1);
  const importSec = find(2);
  const funcSec = find(3);
  const exportSec = find(7);
  if (!typeSec || !funcSec) throw new Error('wasm: missing type or function section');
  return {
    types: parseTypeSection(bytes, typeSec),
    importedFuncs: importSec ? countImportedFuncs(bytes, importSec) : 0,
    funcTypeIndices: parseFuncSection(bytes, funcSec),
    exports: exportSec ? parseExportSection(bytes, exportSec) : [],
  };
}

/** All defined-function indices whose signature equals `type`. */
export function functionsOfType(mod: ParsedModule, type: FuncType): number[] {
  const typeIndex = mod.types.findIndex(
    (t) =>
      t.ret === type.ret &&
      t.params.length === type.params.length &&
      t.params.every((p, i) => p === type.params[i]),
  );
  if (typeIndex < 0) return [];
  const matches: number[] = [];
  mod.funcTypeIndices.forEach((t, i) => {
    if (t === typeIndex) matches.push(mod.importedFuncs + i);
  });
  return matches;
}

function writeUleb(n: number): number[] {
  const out: number[] = [];
  let v = n;
  do {
    let b = v & 0x7f;
    v >>>= 7;
    if (v !== 0) b |= 0x80;
    out.push(b);
  } while (v !== 0);
  return out;
}

/** Return a copy of `bytes` with one more function export appended. */
export function withExport(
  bytes: Uint8Array,
  name: string,
  funcIndex: number,
): Uint8Array {
  const sections = parseSections(bytes);
  const exportSec = sections.find((s) => s.id === 7);
  if (!exportSec) throw new Error('wasm: missing export section');
  const parsed = parseExportSection(bytes, exportSec);
  if (parsed.some((e) => e.name === name)) {
    throw new Error(`wasm: export "${name}" already exists`);
  }

  const encoder = new TextEncoder();
  const nameBytes = encoder.encode(name);
  const entry = [...writeUleb(nameBytes.length), ...nameBytes, 0x00, ...writeUleb(funcIndex)];

  const body = [...writeUleb(parsed.length + 1), ...parsed.map((e) => {
    const nb = encoder.encode(e.name);
    return [...writeUleb(nb.length), ...nb, e.kind, ...writeUleb(e.index)];
  }).flat(), ...entry];
  const newSection = [7, ...writeUleb(body.length), ...body]; // id + size + payload

  // old section footprint = id byte + size LEB + payload
  const oldSizeLebLen = exportSec.start - (exportSec.headerStart + 1);
  const headerStart = exportSec.headerStart;
  const oldTotal = 1 + oldSizeLebLen + exportSec.size;

  const out = new Uint8Array(bytes.length - oldTotal + newSection.length);
  out.set(bytes.subarray(0, headerStart), 0);
  out.set(newSection, headerStart);
  out.set(bytes.subarray(exportSec.start + exportSec.size), headerStart + newSection.length);
  return out;
}
