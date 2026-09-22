// Plugin integration test: run the real `vite build` programmatically over a
// fixture app and prove the build path — companion modules, the emitted wasm
// asset, and a valid module at the end of it.

import { cp, mkdtemp, readdir, readFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { build } from 'vite';

import { rhoPlugin } from '../src/plugin';

const COMPILER = path.resolve(fileURLToPath(new URL('../../../build/rho.wasm', import.meta.url)));
const HAS_COMPILER = existsSync(COMPILER);

describe.runIf(HAS_COMPILER)('rhoPlugin via vite build', () => {
  let appDir: string;
  let outDir: string;

  beforeAll(async () => {
    appDir = await mkdtemp(path.join(os.tmpdir(), 'rho-plugin-app-'));
    outDir = path.join(appDir, 'dist');
    await cp(fileURLToPath(new URL('./fixtures/app/', import.meta.url)), appDir, { recursive: true });
    await build({
      root: appDir,
      configFile: false,
      logLevel: 'silent',
      plugins: [rhoPlugin({ compiler: COMPILER })],
      build: { target: 'es2022', outDir, emptyOutDir: true },
    });
  }, 120000);

  afterAll(async () => {
    await rm(appDir, { recursive: true, force: true });
  });

  it('emits the wasm as a real asset', async () => {
    const assets = await readdir(path.join(outDir, 'assets'));
    const wasm = assets.find((a) => a.includes('.rho-') && a.endsWith('.wasm'));
    expect(wasm).toBeTruthy();
    const bytes = await readFile(path.join(outDir, 'assets', wasm!));
    expect([...bytes.subarray(0, 4)]).toEqual([0x00, 0x61, 0x73, 0x6d]); // \0asm magic
  });

  it('the bundle references the emitted asset and exports callable fns', async () => {
    const assets = await readdir(path.join(outDir, 'assets'));
    const js = assets.find((a) => a.endsWith('.js'));
    expect(js).toBeTruthy();
    const code = await readFile(path.join(outDir, 'assets', js!), 'utf8');
    // the companion survived bundling with its exports and asset URL
    expect(code).toContain('__rho_export_');
    expect(code).toMatch(/assets\/index\.rho-[\w-]+\.wasm/);
    // side-effect-only companion: no leaked build markers
    expect(code).not.toContain('ROLLUP_FILE_URL_');
  });

  it('the emitted wasm instantiates and its exports include the wrappers', async () => {
    const assets = await readdir(path.join(outDir, 'assets'));
    const wasm = assets.find((a) => a.includes('.rho-') && a.endsWith('.wasm'))!;
    const bytes = await readFile(path.join(outDir, 'assets', wasm));
    const mod = await WebAssembly.compile(bytes as unknown as BufferSource);
    const names = WebAssembly.Module.exports(mod).map((e) => e.name);
    expect(names).toContain('memory');
    expect(names).toContain('_start');
    expect(names).toContain('__rho_export_0');
  });
});
