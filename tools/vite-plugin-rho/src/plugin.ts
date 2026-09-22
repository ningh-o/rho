// rhoPlugin(): the official Vite integration for the rho language.
//
// `import { fib } from './math.rho'` becomes a real, typed module:
//   - load(.rho): compiles the module (see compile.ts), returns a companion
//     ESM that instantiates the wasm and re-exports the module's `pub fn`s;
//   - the wasm itself travels as a second module (`<file>.rho?rho-wasm`):
//     a data-URL module in dev (module-graph invalidation just works), an
//     emitted asset in build (`import.meta.ROLLUP_FILE_URL_*`);
//   - a `.d.rho.ts` sidecar lands next to each source so TypeScript (with
//     `allowArbitraryExtensions`) and editors see real signatures;
//   - HMR: editing a .rho drops the compile cache and invalidates both
//     modules; the browser re-imports and re-instantiates.

import fs from 'node:fs';
import path from 'node:path';

import type { Plugin, ViteDevServer } from 'vite';

import { compileRhoModule, getDriver, writeDtsSidecar, type CompiledRhoModule } from './compile.js';
import { RhoCompileError, type CompilerSource } from './driver.js';

export interface RhoPluginOptions {
  /**
   * The rho compiler as a wasm32-wasi module (rho.wasm). Build it from
   * the rho repo with `make build/rho.wasm`, then pass its path — or
   * raw bytes, or any fetchable URL.
   */
  compiler: CompilerSource;
  /**
   * Extra .rho files or directories whose .rho files are visible to `use`
   * resolution, on top of each compiled module's own directory.
   */
  sources?: string[];
  /** Write `<name>.d.rho.ts` sidecars next to compiled modules. Default true. */
  dts?: boolean;
}

const WASM_QUERY = /\?rho-wasm$/;
const VIRTUAL_PREFIX = '\0vite-plugin-rho:wasm:';

interface CacheEntry {
  source: string;
  compiled: CompiledRhoModule;
}

export function rhoPlugin(options: RhoPluginOptions): Plugin {
  const dts = options.dts !== false;
  const cache = new Map<string, CacheEntry>();
  let root = process.cwd();
  let driverWarm = false;

  async function compiled(filePath: string): Promise<CompiledRhoModule> {
    const abs = await toRealFile(filePath);
    const source = await fs.promises.readFile(abs, 'utf8');
    const hit = cache.get(abs);
    if (hit && hit.source === source) return hit.compiled;
    const compiledModule = await compileRhoModule({
      filePath: abs,
      compiler: options.compiler,
      sources: options.sources,
    });
    cache.set(abs, { source, compiled: compiledModule });
    return compiledModule;
  }

  function invalidate(filePath: string): void {
    for (const cand of idCandidates(filePath)) cache.delete(cand);
  }

  /**
   * Some vite/rolldown hosts hand load() an absolute path missing its leading
   * slash; probe the plausible candidates and take the one that exists.
   */
  function idCandidates(id: string): string[] {
    const withRoot = path.isAbsolute(id) ? id : path.resolve(root, id);
    return [withRoot, '/' + withRoot.replace(/^\/+/, ''), path.resolve(root, '/' + id)];
  }

  async function toRealFile(id: string): Promise<string> {
    for (const cand of idCandidates(id)) {
      if (fs.existsSync(cand)) return cand;
    }
    throw new Error(`vite-plugin-rho: cannot resolve module id "${id}"`);
  }

  return {
    name: 'vite-plugin-rho',
    enforce: 'pre',

    configResolved(config) {
      root = config.root;
    },

    async resolveId(id, importer) {
      if (WASM_QUERY.test(id)) {
        const base = id.replace(WASM_QUERY, '');
        let resolved: string;
        if (path.isAbsolute(base)) {
          resolved = path.normalize(base);
        } else if (base.startsWith('.')) {
          resolved = path.resolve(importer ? path.dirname(importer) : root, base);
        } else {
          resolved = path.resolve(root, base);
        }
        return VIRTUAL_PREFIX + resolved;
      }
      return null;
    },

    async load(id) {
      if (id.startsWith(VIRTUAL_PREFIX)) {
        const file = id.slice(VIRTUAL_PREFIX.length);
        const mod = await compiled(file);
        if (this.meta.watchMode) {
          // dev: the wasm rides the module graph as a data URL, so vite's
          // invalidation machinery re-serves it after every edit
          const b64 = Buffer.from(mod.wasm).toString('base64');
          return `export default "data:application/wasm;base64,${b64}";`;
        }
        // build: emit the wasm as a real asset and reference its URL
        const refId = this.emitFile({
          type: 'asset',
          name: path.basename(file, '.rho') + '.rho.wasm',
          source: mod.wasm,
        });
        return `export default import.meta.ROLLUP_FILE_URL_${refId};`;
      }

      // dev ids carry queries (?import, ?t=<timestamp>) — the wasm virtual
      // module keeps its own ?rho-wasm marker upstream of this point
      const [bareId] = id.split('?', 1);
      if (bareId.endsWith('.rho') && !bareId.includes('\0')) {
        let file: string | null = null;
        for (const cand of idCandidates(bareId)) {
          if (fs.existsSync(cand)) {
            file = cand;
            break;
          }
        }
        if (file) {
          const mod = await compiled(file);
          if (dts) await writeDtsSidecar(file, mod.dts);
          return mod.companion.replace('__RHO_WASM_IMPORT__', JSON.stringify(file + '?rho-wasm'));
        }
      }
      return null;
    },

    handleHotUpdate(ctx) {
      if (!ctx.file.endsWith('.rho')) return;
      invalidate(ctx.file);
      const affected = ctx.modules.filter(
        (m) => m.id === ctx.file || (m.id && m.id.startsWith(VIRTUAL_PREFIX) && m.id.endsWith(ctx.file)),
      );
      // ensure the ?rho-wasm module is in the returned set even if rollup
      // has not linked it under its virtual id
      const virtualId = VIRTUAL_PREFIX + path.resolve(ctx.file);
      if (!affected.some((m) => m.id === virtualId)) {
        const virtual = ctx.server.moduleGraph.getModuleById(virtualId);
        if (virtual) affected.push(virtual);
      }
      return affected;
    },
  };
}

export type { CompilerSource, RhoCompileError, ViteDevServer };
export { compileRhoModule, getDriver };
