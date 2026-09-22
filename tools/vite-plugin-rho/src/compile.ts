// The compilation pipeline, shared by the Vite plugin and the rho-dts CLI.
// Pure node logic: source in, { wasm, companion, dts } out — no Vite types.

import { readFile, readdir, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';

import { discoverExports, generateBridgeSource, parseSignatures } from './bridge.js';
import { renderCompanion, type CompanionExport } from './companion.js';
import { createDriver, RhoCompileError, type CompilerSource, type Driver } from './driver.js';
import { generateDts, nonceArgsJs } from './dts.js';
import { createFS } from './wasi.js';
import { parseModule, withExport } from './wasm.js';

export interface CompileOptions {
  /** The .rho file to compile (the module's root file). */
  filePath: string;
  compiler: CompilerSource;
  /**
   * Extra .rho files or directories whose .rho files are visible to `use`
   * resolution. The compiled file's own directory is always visible.
   */
  sources?: string[];
  /** Pre-created driver (tests reuse one to skip re-instantiating). */
  driver?: Driver;
}

export interface CompiledRhoModule {
  /** The patched wasm module (exports injected), ready to ship. */
  wasm: Uint8Array;
  /** The companion ESM source (expects `__RHO_WASM_IMPORT__` replacement). */
  companion: string;
  /** The .d.rho.ts declaration content. */
  dts: string;
  /** Parsed signatures of every top-level pub fn, sorted by name. */
  signatures: ReturnType<typeof parseSignatures>;
  /** Names actually re-exported. */
  exportNames: string[];
  /** The rho compiler's reported version. */
  version: string;
}

const MODULE_NAME_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

/** Load a driver once per options object. */
export async function getDriver(options: CompileOptions): Promise<Driver> {
  if (options.driver) return options.driver;
  return createDriver({ compiler: options.compiler });
}

/** Collect the VFS-visible .rho files: the file itself, its siblings, plus
 *  anything the `sources` option adds (a file, or a directory's *.rho). */
async function collectSources(filePath: string, extra?: string[]): Promise<string[]> {
  const out = new Set<string>([path.resolve(filePath)]);
  const dir = path.dirname(path.resolve(filePath));
  try {
    for (const entry of await readdir(dir)) {
      if (entry.endsWith('.rho')) out.add(path.join(dir, entry));
    }
  } catch {
    // unreadable dir: the compile itself will diagnose what's missing
  }
  for (const src of extra ?? []) {
    const abs = path.resolve(src);
    const s = await stat(abs).then(
      (r) => r,
      () => null,
    );
    if (!s) continue;
    if (s.isDirectory()) {
      for (const entry of await readdir(abs)) {
        if (entry.endsWith('.rho')) out.add(path.join(abs, entry));
      }
    } else {
      out.add(abs);
    }
  }
  return [...out];
}

const I32_TYPES = new Set(['bool', 'i8', 'i16', 'i32', 'u8', 'u16', 'u32']);
const I64_TYPES = new Set(['i64', 'u64', 'usize', 'isize']);

function companionKind(t: string): CompanionExport['params'][number] {
  const type = t.trim();
  if (type === 'string') return 'str';
  if (type === 'bool') return 'bool';
  if (I64_TYPES.has(type)) return 'i64';
  if (type === 'f32') return 'f32';
  if (type === 'f64') return 'f64';
  if (I32_TYPES.has(type)) return 'i32';
  return 'raw';
}

export async function compileRhoModule(options: CompileOptions): Promise<CompiledRhoModule> {
  const absPath = path.resolve(options.filePath);
  const driver = await getDriver(options);
  const version = await driver.version();

  const base = path.basename(absPath, '.rho');
  if (!MODULE_NAME_RE.test(base)) {
    throw new RhoCompileError(
      `invalid rho module name "${base}" (from ${absPath}): the file's basename must be a rho identifier so the bridge can \`use\` it`,
      '',
    );
  }

  // ---- virtual filesystem: user module + siblings + bridge --------------
  const fs = createFS();
  const sources = await collectSources(absPath, options.sources);
  const userPath = `/${base}.rho`;
  for (const src of sources) {
    fs.write(`/${path.basename(src)}`, new Uint8Array(await readFile(src)));
  }

  // ---- signatures: `rho fmt` over the user module (the compiler's own
  //      typed surface), filtered to what the bridge can wrap -------------
  const fmtOutput = await driver.fmt(userPath, fs);
  const all = parseSignatures(fmtOutput).sort((a, b) =>
    a.name < b.name ? -1 : a.name > b.name ? 1 : 0,
  );
  const supported = all.filter((s) => !s.generic && !s.unsupportedReason);

  // ---- bridge build -------------------------------------------------------
  fs.write(
    '/__rho_bridge__.rho',
    new TextEncoder().encode(generateBridgeSource({ moduleName: base, signatures: supported })),
  );
  const raw = await driver.buildWasm('/__rho_bridge__.rho', fs);

  // ---- discovery + export injection --------------------------------------
  const found = discoverExports(parseModule(raw), supported);
  let patched = raw;
  for (const fn of found) {
    patched = withExport(patched, `__rho_export_${fn.k}`, fn.index);
  }

  // ---- companion + dts ----------------------------------------------------
  const companions: CompanionExport[] = found.map((fn) => {
    const hiddenOut = Boolean(fn.sig.ret) && fn.abi.ret === null;
    return {
      name: fn.name,
      k: fn.k,
      params: fn.sig.params.map((p) => companionKind(p.type)),
      ret: (fn.sig.ret ? companionKind(fn.sig.ret) : 'void') as CompanionExport['ret'],
      hiddenOut,
      nonceArgs: nonceArgsJs(fn.k),
    };
  });

  return {
    wasm: patched,
    companion: renderCompanion(companions),
    dts: generateDts({ signatures: all, exportNames: found.map((f) => f.name) }),
    signatures: all,
    exportNames: found.map((f) => f.name),
    version,
  };
}

/** Write the .d.rho.ts sidecar next to a .rho file (change-guarded). */
export async function writeDtsSidecar(filePath: string, dts: string): Promise<boolean> {
  const target = path.resolve(filePath).replace(/\.rho$/, '.d.rho.ts');
  let prev: string | null = null;
  try {
    prev = await readFile(target, 'utf8');
  } catch {
    prev = null;
  }
  if (prev === dts) return false;
  await writeFile(target, dts, 'utf8');
  return true;
}
