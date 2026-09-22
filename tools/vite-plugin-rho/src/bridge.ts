// The bridge: how a .rho module's `pub fn`s become callable JS functions.
//
// rho's wasm backend exports only `memory` and `_start` — user functions are
// internal. Rather than guess function indices, the plugin compiles a tiny
// generated *bridge program* alongside the user module. The bridge declares
// one wrapper per exported fn with a UNIQUE FINGERPRINT SIGNATURE: four
// leading nonce parameters (base-4 over i32/i64/f32/f64) no other function in
// the module carries. After the build, the plugin reads the wasm type and
// function sections, finds the function whose signature equals the expected
// fingerprint (asserting exactly one match), and splices a real export for it
// into the export section.
//
// The compiler itself type-checks every wrapper (`return m.fib(n);` must
// check), so a stale ABI model fails the build loudly instead of
// mis-binding at runtime.

import type { FuncType, ValType } from './wasm.js';

// ------------------------------------------------------------- signatures --

export interface RhoParam {
  name: string;
  type: string;
}

export interface RhoSignature {
  name: string;
  params: RhoParam[];
  ret: string | null; // null = void
  /** Generic fn — cannot be called uninstantiated; excluded from exports. */
  generic: boolean;
  /** Unsupported param/return type — excluded from exports (comment only). */
  unsupportedReason: string | null;
}

/**
 * Parse the top-level `pub fn` declarations out of `rho fmt` output — the
 * compiler's own canonical rendering of the typed surface. One declaration
 * per line: `pub fn name(a: i32, b: string) -> bool {`.
 */
export function parseSignatures(fmtOutput: string): RhoSignature[] {
  const sigs: RhoSignature[] = [];
  for (const rawLine of fmtOutput.split('\n')) {
    const line = rawLine.trimEnd();
    if (!line.startsWith('pub fn ')) continue;
    const m = /^pub fn ([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\((.*)\)(?:\s*->\s*(.+?))?\s*\{\s*$/.exec(
      line,
    );
    if (!m) continue;
    const [, name, tparams, paramText, retText] = m;
    const params: RhoParam[] = [];
    const body = paramText.trim();
    if (body.length > 0) {
      for (const part of splitTopLevel(body, ',')) {
        const pm = /^([A-Za-z_][A-Za-z0-9_]*|\_):\s*(.+)$/.exec(part.trim());
        if (!pm) {
          params.push({ name: '?', type: part.trim() });
        } else {
          params.push({ name: pm[1], type: pm[2].trim() });
        }
      }
    }
    const ret = retText ? retText.trim() : null;
    sigs.push({
      name,
      params,
      ret,
      generic: Boolean(tparams),
      unsupportedReason: unsupportedReasonFor(params, ret),
    });
  }
  return sigs;
}

/** Split on `sep` at paren/bracket depth zero (fn(I, J) types contain commas). */
function splitTopLevel(text: string, sep: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let cur = '';
  for (const ch of text) {
    if (ch === '(' || ch === '[') depth++;
    else if (ch === ')' || ch === ']') depth--;
    if (ch === sep && depth === 0) {
      parts.push(cur);
      cur = '';
    } else {
      cur += ch;
    }
  }
  parts.push(cur);
  return parts;
}

// -------------------------------------------------------------------- ABI --
//
// Validated against the wasm backend (rho/boot/src/emit_wasm.c, w_ir_type +
// w_vt + w_type_of_fn) and pinned by probe + tests:
//   - bool and i8..u32 travel as i32; i64/u64/usize/isize as i64;
//     f32/f64 as themselves;
//   - strings, slices, arrays, structs, enums, fn values and dyn travel as a
//     single i32 pointer;
//   - an aggregate RETURN takes a hidden out-pointer as the FIRST signature
//     parameter and produces no wasm result;
//   - *T / weak T are plain i32 addresses in both directions.

const PRIM_I32 = new Set(['bool', 'i8', 'i16', 'i32', 'u8', 'u16', 'u32']);
const PRIM_I64 = new Set(['i64', 'u64', 'usize', 'isize']);

/** wasm-level ABI of one rho type. */
export function abiOfRhoType(type: string): {
  kind: 'i32' | 'i64' | 'f32' | 'f64' | 'str' | 'agg';
} {
  const t = type.trim();
  if (PRIM_I32.has(t)) return { kind: 'i32' };
  if (PRIM_I64.has(t)) return { kind: 'i64' };
  if (t === 'f32') return { kind: 'f32' };
  if (t === 'f64') return { kind: 'f64' };
  if (t === 'string') return { kind: 'str' };
  return { kind: 'agg' };
}

/** True if the type is a *T or weak T (scalar pointer ABI, synthesizable). */
export function isPointerLike(type: string): boolean {
  const t = type.trim();
  return t.startsWith('*') || t.startsWith('weak ');
}

/** True for []T slices — aggregate ABI but synthesizable via make([]T, 0). */
export function isSlice(type: string): boolean {
  return type.trim().startsWith('[]');
}

/** The wasm signature a bridge wrapper carries for this fn (nonce excluded). */
export function fnAbi(sig: RhoSignature): { params: ValType[]; ret: ValType | null } {
  const params: ValType[] = [];
  for (const p of sig.params) {
    const abi = abiOfRhoType(p.type);
    params.push(
      abi.kind === 'i64' ? 'i64'
        : abi.kind === 'f32' ? 'f32'
          : abi.kind === 'f64' ? 'f64'
            : 'i32', // i32 scalars, strings, pointers, aggregates
    );
  }
  if (sig.ret) {
    if (isPointerLike(sig.ret)) return { params, ret: 'i32' }; // *T / weak T: scalar address result
    const abi = abiOfRhoType(sig.ret);
    if (abi.kind === 'i64') return { params, ret: 'i64' };
    if (abi.kind === 'f32') return { params, ret: 'f32' };
    if (abi.kind === 'f64') return { params, ret: 'f64' };
    if (abi.kind === 'i32') return { params, ret: 'i32' };
    // aggregate return (string, slice, struct...): hidden out-ptr, no result
    return { params: ['i32', ...params], ret: null };
  }
  return { params, ret: null };
}

const VALTYPES: ValType[] = ['i32', 'i64', 'f32', 'f64'];

/** The four nonce parameter types that fingerprint export #k. */
export function nonceFor(k: number): ValType[] {
  return [0, 1, 2, 3].map((slot) => VALTYPES[Math.floor(k / 4 ** slot) % 4]);
}

/** Why a signature cannot be re-exported (null = supported). */
function unsupportedReasonFor(params: RhoParam[], ret: string | null): string | null {
  for (const p of params) {
    const reason = unsupportedType(p.type, 'parameter');
    if (reason) return reason;
  }
  return ret ? unsupportedType(ret, 'return') : null;
}

function unsupportedType(type: string, role: string): string | null {
  const t = type.trim();
  if (isPointerLike(t) || isSlice(t)) return null; // raw but supported
  if (PRIM_I32.has(t) || PRIM_I64.has(t) || t === 'f32' || t === 'f64' || t === 'string') {
    return null;
  }
  if (t.startsWith('[') && !t.startsWith('[]')) {
    return `${role} type "${t}": fixed-size arrays are not supported`;
  }
  if (t.startsWith('fn(') || t.startsWith('fn<')) {
    return `${role} type "${t}": function values are not supported`;
  }
  if (t.startsWith('dyn ')) {
    return `${role} type "${t}": trait objects are not supported`;
  }
  return `${role} type "${t}": only primitives, string, slices and pointers are supported`;
}

// ------------------------------------------------------- bridge generation --

export interface BridgeOptions {
  /** Module namespace the bridge `use`s (the user file's basename). */
  moduleName: string;
  /** Supported signatures, in export order (sorted by name upstream). */
  signatures: RhoSignature[];
}

const NONCE_LITERALS: Record<ValType, string> = {
  i32: '0',
  i64: '0',
  f32: '0.0',
  f64: '0.0',
};

/** A dummy argument literal usable in the unreachable retention call. */
function dummyLiteral(type: string, moduleName: string): string | null {
  const abi = abiOfRhoType(type);
  if (abi.kind === 'i32') return type === 'bool' ? 'false' : '0';
  if (abi.kind === 'i64') return '0';
  if (abi.kind === 'f32' || abi.kind === 'f64') return '0.0';
  if (abi.kind === 'str') return '""';
  if (isSlice(type)) {
    return `make([]${qualify(type.trim().slice(2), moduleName)}, 0)`;
  }
  if (isPointerLike(type)) return 'null';
  return null;
}

/** Qualify a user-declared type with its module namespace (prims stay bare). */
function qualify(type: string, moduleName: string): string {
  const t = type.trim();
  if (
    PRIM_I32.has(t) || PRIM_I64.has(t) ||
    t === 'f32' || t === 'f64' || t === 'string'
  ) {
    return t;
  }
  return `${moduleName}.${t}`;
}

/**
 * Generate the bridge source. The user's module is `use`d; one wrapper per
 * supported export carries the fingerprint; `main` calls every wrapper from
 * an unreachable branch (a mutable-static read the compiler cannot fold) so
 * dead-code elimination keeps them — and only them plus what they call.
 */
export function generateBridgeSource(options: BridgeOptions): string {
  const { moduleName } = options;
  // single source of truth: only supported, non-generic signatures are wrapped
  const signatures = options.signatures.filter((s) => !s.generic && !s.unsupportedReason);
  const lines: string[] = [];
  lines.push(`// Generated by vite-plugin-rho — do not edit.`);
  lines.push(`use ${moduleName};`);
  lines.push('');
  lines.push('static mut KEEP: i32 = 0;');
  lines.push('');

  signatures.forEach((sig, k) => {
    const nonce = nonceFor(k);
    const nonceParams: string[] = nonce.map((vt, i) => `__n${i}: ${vt}`);
    const realParams = sig.params.map((p) => `${p.name}: ${qualify(p.type, moduleName)}`);
    const allParams = [...nonceParams, ...realParams].filter(Boolean).join(', ');
    const forwardArgs = sig.params.map((p) => p.name).join(', ');
    const ret = sig.ret ? ` -> ${qualify(sig.ret, moduleName)}` : '';
    lines.push(`fn __rho_export_${k}(${allParams})${ret} {`);
    if (sig.ret) {
      lines.push(`  return ${moduleName}.${sig.name}(${forwardArgs});`);
    } else {
      lines.push(`  ${moduleName}.${sig.name}(${forwardArgs});`);
    }
    lines.push('}');
    lines.push('');
  });

  lines.push('fn main() -> i32 {');
  lines.push('  if (KEEP != 0) {');
  signatures.forEach((sig, k) => {
    const args: string[] = [];
    nonceFor(k).forEach((vt) => args.push(NONCE_LITERALS[vt]));
    for (const p of sig.params) {
      const dummy = dummyLiteral(p.type, moduleName);
      if (dummy === null) args.push('?'); // unreachable: unsupported fns are filtered
      else args.push(dummy);
    }
    const call = `__rho_export_${k}(${args.join(', ')})`;
    if (sig.ret) lines.push(`    let __keep${k} = ${call};`);
    else lines.push(`    ${call};`);
  });
  lines.push('  }');
  lines.push('  return 0;');
  lines.push('}');
  lines.push('');
  return lines.join('\n');
}

// -------------------------------------------------------- wasm discovery --

export interface DiscoveredExport {
  name: string;
  /** Export ordinal (index into the sorted supported signature list). */
  k: number;
  /** Function index in the built module. */
  index: number;
  sig: RhoSignature;
  nonce: ValType[];
  abi: { params: ValType[]; ret: ValType | null };
}

/**
 * Locate every wrapper in the built bridge module by its fingerprint type.
 * Throws when a fingerprint is missing (stripped?) or ambiguous (collides
 * with another function's signature) — the loud failure the design relies on.
 */
export function discoverExports(
  mod: { types: FuncType[]; importedFuncs: number; funcTypeIndices: number[] },
  signatures: RhoSignature[],
): DiscoveredExport[] {
  const found: DiscoveredExport[] = [];
  signatures.forEach((sig, k) => {
    const nonce = nonceFor(k);
    const abi = fnAbi(sig);
    // the hidden out-ptr for aggregate returns is the FIRST signature
    // parameter (emit_wasm.c w_type_of_fn), ahead of even the nonce params
    const hiddenOut = Boolean(sig.ret) && abi.ret === null;
    const realParams = hiddenOut ? abi.params.slice(1) : abi.params;
    const expected: FuncType = {
      params: [...(hiddenOut ? (['i32'] as ValType[]) : []), ...nonce, ...realParams],
      ret: abi.ret,
    };
    const matches: number[] = [];
    mod.types.forEach((t, ti) => {
      if (
        t.ret === expected.ret &&
        t.params.length === expected.params.length &&
        t.params.every((p, i) => p === expected.params[i])
      ) {
        mod.funcTypeIndices.forEach((ft, fi) => {
          if (ft === ti) matches.push(mod.importedFuncs + fi);
        });
      }
    });
    if (matches.length === 0) {
      throw new Error(
        `vite-plugin-rho: export "${sig.name}" — its bridge wrapper was not found in the built module (signature ${JSON.stringify(expected)}). The compiler output shape changed; please report this.`,
      );
    }
    if (matches.length > 1) {
      throw new Error(
        `vite-plugin-rho: export "${sig.name}" — ${matches.length} functions share its fingerprint signature; cannot disambiguate.`,
      );
    }
    found.push({ name: sig.name, k, index: matches[0], sig, nonce, abi });
  });
  return found;
}

function isScalarAbi(type: string): boolean {
  const k = abiOfRhoType(type).kind;
  return k === 'i32' || k === 'i64' || k === 'f32' || k === 'f64';
}
