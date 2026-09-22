// Unit tests for the pure pieces: signature parsing, ABI modeling, nonce
// fingerprints, and .d.ts rendering.

import { describe, expect, it } from 'vitest';

import { discoverExports, fnAbi, generateBridgeSource, nonceFor, parseSignatures } from '../src/bridge';
import { generateDts, nonceArgsJs } from '../src/dts';

const FMT_SAMPLE = `// module doc
pub fn fib(n: i32) -> i32 {
  return n;
}

pub fn concat(a: string, b: string) -> string {
  return cat(a, b);
}

pub fn many(a: i32, b: f64, c: string, d: []i32, e: *u8, f: bool) -> bool {
  return f;
}

pub fn big(a: i64, b: u64, c: usize) -> usize {
  return a;
}

pub fn voided(s: string) {
  printf("{}\\n", s);
}

pub fn generic[T](x: T) -> T {
  return x;
}

pub fn pointy(p: Point) -> i32 {
  return p.x;
}

fn main() -> i32 {
  return 0;
}
`;

describe('parseSignatures', () => {
  it('parses top-level pub fns from rho fmt output', () => {
    const sigs = parseSignatures(FMT_SAMPLE);
    expect(sigs.map((s) => s.name)).toEqual([
      'fib', 'concat', 'many', 'big', 'voided', 'generic', 'pointy',
    ]);
  });

  it('splits params and return types, void included', () => {
    const sigs = parseSignatures(FMT_SAMPLE);
    expect(sigs[0].params).toEqual([{ name: 'n', type: 'i32' }]);
    expect(sigs[0].ret).toBe('i32');
    expect(sigs[3].ret).toBe('usize');
    expect(sigs[4].ret).toBeNull(); // voided
  });

  it('keeps fn-typed params intact across their internal commas', () => {
    const sigs = parseSignatures('pub fn apply(f: fn(i32, i32) -> i32, a: i32) -> i32 {\n  return a;\n}\n');
    expect(sigs[0].params).toEqual([
      { name: 'f', type: 'fn(i32, i32) -> i32' },
      { name: 'a', type: 'i32' },
    ]);
    expect(sigs[0].unsupportedReason).toContain('function values');
  });

  it('ignores non-pub and indented declarations', () => {
    const sigs = parseSignatures('fn private(a: i32) -> i32 {\n  return a;\n}\n\nstruct S {\n  x: i32,\n}\n');
    expect(sigs).toEqual([]);
  });
});

describe('ABI model', () => {
  it('maps scalars by width', () => {
    expect(fnAbi(parseSignatures('pub fn f(a: i8, b: u32) -> bool {\n  return true;\n}\n')[0])).toEqual({
      params: ['i32', 'i32'],
      ret: 'i32',
    });
    expect(fnAbi(parseSignatures('pub fn f(a: i64, b: usize) -> u64 {\n  return b;\n}\n')[0])).toEqual({
      params: ['i64', 'i64'],
      ret: 'i64',
    });
    expect(fnAbi(parseSignatures('pub fn f(a: f32, b: f64) -> f32 {\n  return a;\n}\n')[0])).toEqual({
      params: ['f32', 'f64'],
      ret: 'f32',
    });
  });

  it('treats strings and pointers as i32, aggregates returns as hidden out-ptr', () => {
    // concat(string, string) -> string: hidden out-ptr FIRST, no wasm result
    expect(fnAbi(parseSignatures('pub fn concat(a: string, b: string) -> string {\n  return a;\n}\n')[0])).toEqual({
      params: ['i32', 'i32', 'i32'],
      ret: null,
    });
    // alloc(usize) -> *u8: pointer return is a scalar i32 result
    expect(fnAbi(parseSignatures('pub fn alloc(n: usize) -> *u8 {\n  return null;\n}\n')[0])).toEqual({
      params: ['i64'],
      ret: 'i32',
    });
    // void fn: no result, no out-ptr
    expect(fnAbi(parseSignatures('pub fn v(s: string) {\n  return;\n}\n')[0])).toEqual({
      params: ['i32'],
      ret: null,
    });
  });
});

describe('nonce fingerprints', () => {
  it('are distinct base-4 encodings over i32/i64/f32/f64', () => {
    const seen = new Set<string>();
    for (let k = 0; k < 256; k++) {
      const key = nonceFor(k).join(',');
      expect(seen.has(key)).toBe(false);
      seen.add(key);
      expect(nonceArgsJs(k)).toMatch(/^[\d,n ]+$/);
    }
    expect(nonceFor(0)).toEqual(['i32', 'i32', 'i32', 'i32']);
    expect(nonceFor(1)).toEqual(['i64', 'i32', 'i32', 'i32']);
    expect(nonceFor(255)).toEqual(['f64', 'f64', 'f64', 'f64']);
  });
});

describe('bridge source', () => {
  it('wraps every supported fn and routes the retention main through them', () => {
    const sigs = parseSignatures(FMT_SAMPLE);
    const src = generateBridgeSource({ moduleName: 'math', signatures: sigs });
    expect(src).toContain('use math;');
    expect(src).toContain('static mut KEEP: i32 = 0;');
    expect(src).toContain('fn __rho_export_0(');
    expect(src).toContain('return math.fib(n);');
    expect(src).toContain('let __keep');
    // generic + struct fns never reach the bridge (caller filters)
    expect(src).not.toContain('math.generic');
    expect(src).not.toContain('math.pointy');
  });
});

describe('generateDts', () => {
  it('renders rich types and documents exclusions', () => {
    const sigs = parseSignatures(FMT_SAMPLE);
    const dts = generateDts({
      signatures: sigs,
      exportNames: ['fib', 'concat', 'many', 'big', 'voided'],
    });
    expect(dts).toContain('export declare function fib(n: number): number;');
    expect(dts).toContain('export declare function concat(a: string, b: string): string;');
    expect(dts).toContain('export declare function big(a: bigint, b: bigint, c: bigint): bigint;');
    expect(dts).toContain('export declare function voided(s: string): void;');
    expect(dts).toContain('export declare function many(a: number, b: number, c: string, d: number, e: number, f: boolean): boolean;');
    expect(dts).toContain('not exported: generic functions');
    expect(dts).toContain('parameter type "Point"'); // pointy: struct param
  });
});

describe('discoverExports', () => {
  it('finds exactly one function per fingerprint and rejects ambiguity', () => {
    const sigs = parseSignatures('pub fn fib(n: i32) -> i32 {\n  return n;\n}\n');
    const fakeModule = {
      types: [
        { params: ['i32'], ret: null },
        { params: ['i32', 'i32', 'i32', 'i32', 'i32'], ret: 'i32' as const }, // nonce(k=0,i32x4)+i32 -> i32
      ],
      importedFuncs: 2,
      funcTypeIndices: [0, 0, 1],
    };
    const found = discoverExports(fakeModule, sigs);
    expect(found).toHaveLength(1);
    expect(found[0].index).toBe(4); // 2 imports + 3rd defined fn
    expect(found[0].k).toBe(0);

    expect(() =>
      discoverExports(
        { ...fakeModule, funcTypeIndices: [1, 1] },
        sigs,
      ),
    ).toThrow(/fingerprint/);
    expect(() =>
      discoverExports({ types: [], importedFuncs: 0, funcTypeIndices: [] }, sigs),
    ).toThrow(/not found/);
  });
});
