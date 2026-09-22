// Integration tests: the real rho compiler (rho.wasm) end to end.
// Skips with a clear message only when the compiler binary is absent.

import { existsSync } from 'node:fs';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { afterAll, beforeAll, describe, expect, it } from 'vitest';

import { compileRhoModule } from '../src/compile';
import { RhoCompileError } from '../src/driver';

const COMPILER = path.resolve(fileURLToPath(new URL('../../../build/rho.wasm', import.meta.url)));
const HAS_COMPILER = existsSync(COMPILER);

describe.runIf(HAS_COMPILER)('compileRhoModule (real compiler)', () => {
  let tmp: string;

  beforeAll(async () => {
    tmp = await mkdtemp(path.join(os.tmpdir(), 'rho-plugin-test-'));
    await writeFile(path.join(tmp, 'math.rho'), await readFile2('fixtures/math.rho'));
    await writeFile(path.join(tmp, 'helper.rho'), await readFile2('fixtures/helper.rho'));
  });

  afterAll(async () => {
    await rm(tmp, { recursive: true, force: true });
  });

  it('compiles a module and reports its version', async () => {
    const mod = await compileRhoModule({ filePath: path.join(tmp, 'math.rho'), compiler: COMPILER });
    expect(mod.version).toMatch(/^rho \d+\.\d+\.\d+$/);
    expect(mod.wasm[0]).toBe(0x00); // \0asm magic
    expect(Buffer.from(mod.wasm.subarray(0, 4)).toString('binary')).toBe('\0asm');
  });

  it('re-exports the supported pub fns, excluding generics and struct-typed fns', async () => {
    const mod = await compileRhoModule({ filePath: path.join(tmp, 'math.rho'), compiler: COMPILER });
    expect(mod.exportNames).toEqual([
      'add', 'concat', 'double', 'fib', 'greet', 'half64', 'is_even', 'scale',
    ]);
    const excluded = mod.signatures.filter((s) => !mod.exportNames.includes(s.name));
    expect(excluded.map((s) => s.name).sort()).toEqual(['norm', 'twice']);
  });

  it('patches the wasm export section with one entry per export', async () => {
    const { parseModule } = await import('../src/wasm');
    const mod = await compileRhoModule({ filePath: path.join(tmp, 'math.rho'), compiler: COMPILER });
    const parsed = parseModule(mod.wasm);
    const names = parsed.exports.map((e) => e.name);
    expect(names).toContain('memory');
    expect(names).toContain('_start');
    for (let k = 0; k < mod.exportNames.length; k++) {
      expect(names).toContain(`__rho_export_${k}`);
    }
    expect(names).toHaveLength(2 + mod.exportNames.length);
  });

  it('generates a companion module and a .d.rho.ts', async () => {
    const mod = await compileRhoModule({ filePath: path.join(tmp, 'math.rho'), compiler: COMPILER });
    expect(mod.companion).toContain('export function fib(');
    expect(mod.companion).toContain('__RHO_WASM_IMPORT__');
    expect(mod.dts).toContain('export declare function fib(n: number): number;');
    expect(mod.dts).toContain('export declare function concat(a: string, b: string): string;');
    expect(mod.dts).toContain('export declare function is_even(n: number): boolean;');
    expect(mod.dts).toContain('export declare function half64(n: bigint): bigint;');
    expect(mod.dts).toContain('export declare function greet(): string;');
    // generic + struct-typed fns are documented comments, not declarations
    expect(mod.dts).toContain('// twice(...) — not exported');
    expect(mod.dts).toContain('// norm(...) — not exported');
    expect(mod.dts).not.toContain('export declare function twice');
    expect(mod.dts).not.toContain('export declare function norm');
  });

  it('the companion actually runs: calls fib, concat, is_even, half64, greet through the patched wasm', async () => {
    const mod = await compileRhoModule({ filePath: path.join(tmp, 'math.rho'), compiler: COMPILER });
    // materialize the companion as a real ESM module in a temp dir
    const dir = await mkdtemp(path.join(os.tmpdir(), 'rho-companion-'));
    const wasmUrl = 'data:application/wasm;base64,' + Buffer.from(mod.wasm).toString('base64');
    await writeFile(path.join(dir, 'wasm-url.mjs'), `export default ${JSON.stringify(wasmUrl)};\n`);
    await writeFile(
      path.join(dir, 'math.mjs'),
      mod.companion.replace('__RHO_WASM_IMPORT__', JSON.stringify('./wasm-url.mjs')),
    );
    const m = await import(path.join(dir, 'math.mjs'));
    expect(m.fib(10)).toBe(55);
    expect(m.fib(20)).toBe(6765);
    expect(m.add(2, 3)).toBe(5);
    expect(m.concat('hello, ', 'rho')).toBe('hello, rho');
    expect(m.concat('', '')).toBe('');
    expect(m.is_even(4)).toBe(true);
    expect(m.is_even(7)).toBe(false);
    expect(m.half64(40n)).toBe(20n);
    expect(m.scale(1.5, 2.0)).toBeCloseTo(3.0);
    expect(m.greet()).toBe('hello from rho');
    expect(m.double(21)).toBe(42); // `use helper;` — cross-module resolution
    await rm(dir, { recursive: true, force: true });
  }, 60000);

  it('surfaces compiler diagnostics as RhoCompileError', async () => {
    await writeFile(
      path.join(tmp, 'broken.rho'),
      'pub fn f(a: i32) -> string {\n  return a;\n}\n',
    );
    await expect(
      compileRhoModule({ filePath: path.join(tmp, 'broken.rho'), compiler: COMPILER }),
    ).rejects.toBeInstanceOf(RhoCompileError);
  });

  it('rejects non-identifier module basenames', async () => {
    await writeFile(path.join(tmp, 'my-lib.rho'), 'pub fn a() -> i32 {\n  return 1;\n}\n');
    await expect(
      compileRhoModule({ filePath: path.join(tmp, 'my-lib.rho'), compiler: COMPILER }),
    ).rejects.toThrow(/must be a rho identifier/);
  });
});

async function readFile2(rel: string): Promise<string> {
  const { readFile } = await import('node:fs/promises');
  return readFile(fileURLToPath(new URL(rel, import.meta.url)), 'utf8');
}
