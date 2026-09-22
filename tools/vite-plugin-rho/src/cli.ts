#!/usr/bin/env node
// rho-dts: write .d.rho.ts sidecars for .rho modules without running Vite.
//
//   rho-dts [--compiler path/to/rho.wasm] src/ src/lib.rho
//
// Each .rho file under the given paths gets a `<name>.d.rho.ts` next to it,
// ready for `tsc` with `allowArbitraryExtensions` (or just for editors).
// The compiler path defaults to $RHO_BOOT_WASM, then ./rho/build/rho.wasm.

import { realpathSync } from 'node:fs';
import { readdir, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { pathToFileURL } from 'node:url';

import { compileRhoModule } from './compile.js';
import { createDriver, type CompilerSource } from './driver.js';

async function* walkRho(entry: string): AsyncGenerator<string> {
  const s = await stat(entry);
  if (s.isDirectory()) {
    for (const item of await readdir(entry, { withFileTypes: true })) {
      if (item.isFile() && item.name.endsWith('.rho')) {
        yield path.join(entry, item.name);
      }
    }
  } else if (entry.endsWith('.rho')) {
    yield entry;
  }
}

async function resolveCompiler(argvCompiler?: string): Promise<CompilerSource> {
  const candidates = [
    argvCompiler,
    process.env.RHO_BOOT_WASM,
    path.resolve('rho/build/rho.wasm'),
  ].filter(Boolean) as string[];
  for (const c of candidates) {
    if (await stat(c).then(
      (s) => s.isFile(),
      () => false,
    )) {
      return c;
    }
  }
  throw new Error(
    'rho-dts: cannot find rho.wasm — pass --compiler <path>, set $RHO_BOOT_WASM, or build it: make -C rho build/rho.wasm',
  );
}

export async function main(argv: string[]): Promise<number> {
  let compilerArg: string | undefined;
  const paths: string[] = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--compiler') compilerArg = argv[++i];
    else paths.push(argv[i]);
  }
  if (paths.length === 0) paths.push('.');
  const compiler = await resolveCompiler(compilerArg);
  const driver = await createDriver({ compiler });

  let count = 0;
  for (const p of paths) {
    for await (const file of walkRho(path.resolve(p))) {
      const mod = await compileRhoModule({ filePath: file, compiler, driver });
      const target = file.replace(/\.rho$/, '.d.rho.ts');
      await writeFile(target, mod.dts, 'utf8');
      process.stdout.write(`wrote ${target} (${mod.exportNames.length} exports)\n`);
      count++;
    }
  }
  if (count === 0) process.stdout.write('rho-dts: no .rho files found\n');
  return 0;
}

// CLI entry: npm installs the bin as a symlink, so compare real paths
function isMain(): boolean {
  if (!process.argv[1]) return false;
  try {
    return pathToFileURL(realpathSync(process.argv[1])).href === import.meta.url;
  } catch {
    return false;
  }
}

if (isMain()) {
  main(process.argv.slice(2)).catch((e) => {
    console.error(e instanceof Error ? e.message : e);
    process.exit(1);
  });
}
