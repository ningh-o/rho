// index.js — vite-plugin-rho, the vite transform hook for the rho
// language (docs/ecosystem.md §2).
//
// Compiles `.rho` sources to wasm32-wasi program bytes inside vite
// dev/build, retiring hand-pinned wasm artifacts. The plugin embeds
// ONE compiler generation (assets/generation.json is the package's
// only pin — the artifact, its sha256, and every source input);
// every compile runs that generation on the Node runtime face
// (src/compiler.js) and assembles the WAT with the same wabt
// generation the CLI chain uses.
//
// Flags shared with the CLI: the artifact's verb surface is
// main.rho's flag face — build (default), check, fmt — mapped through
// the app face's /mode marker exactly as the CLI drives it.
//
// The transform's output:
//   mode "build" (default)
//     inline: false (default) — the wasm bytes ride a rollup asset and
//             the module default-exports its URL
//             (import.meta.ROLLUP_FILE_URL — the rollup-standard form,
//             resolved by vite build; for dev/SSR use inline)
//     inline: true — the module default-exports a
//             data:application/wasm;base64 URL (environment-free)
//   mode "check" — no emit; a refusal fails the build with the
//             compiler's own diagnostics
//   mode "fmt"  — the module default-exports the canonical formatting
//
// Build cache: keyed by (compiler generation, mode, source bytes) —
// a source compiling twice (two importers, dev re-transforms) compiles
// once. The generation key makes a stale cache impossible: the cache
// dies with the artifact that fed it.

import { createHash } from "node:crypto";
import { compileRho, loadGeneration, warmCompiler } from "./compiler.js";

const WASM_MAGIC = new Uint8Array([0x00, 0x61, 0x73, 0x6d]); // "\0asm"

export default function vitePluginRho(options = {}) {
  const mode = options.mode ?? "build";
  const inline = options.inline ?? false;
  if (!["build", "check", "fmt"].includes(mode)) {
    throw new Error(`vite-plugin-rho: unknown mode ${JSON.stringify(mode)} (build | check | fmt)`);
  }
  // one pin, read once: the generation record names the artifact this
  // plugin compiles with, forever
  const generation = loadGeneration();
  const cache = new Map(); // cacheKey -> { ok, bytes, wat, stderr }

  return {
    name: "vite-plugin-rho",
    // warm the compiler module before the first transform so the first
    // .rho import does not pay the module compile alone
    buildStart() {
      warmCompiler();
    },
    async transform(code, id) {
      const file = id.split("?", 1)[0];
      if (!file || !file.endsWith(".rho")) return null;

      const key = createHash("sha256")
        .update(generation.generation)
        .update("\0")
        .update(mode)
        .update("\0")
        .update(code, "utf8")
        .digest("hex");
      let entry = cache.get(key);
      if (!entry) {
        entry = await compileRho(code, { mode });
        cache.set(key, entry);
      }

      if (!entry.ok) {
        // the CLI refuses; the build refuses — with the compiler's own
        // stderr (file:line:col diagnostics), never a swallowed error
        this.error(`rho ${mode} refused ${file}\n${entry.stderr}`);
      }

      if (mode === "fmt") {
        return { code: `export default ${JSON.stringify(entry.wat)};\n`, map: null };
      }
      if (mode === "check") {
        return { code: "export default null;\n", map: null };
      }

      // mode "build": entry.bytes are the program's wasm
      if (!entry.bytes || entry.bytes.length < 4 || !WASM_MAGIC.every((b, i) => entry.bytes[i] === b)) {
        this.error(`vite-plugin-rho: compiled output of ${file} is not a wasm module (compiler bug — the generation pin is broken)`);
      }
      if (inline) {
        const b64 = Buffer.from(entry.bytes).toString("base64");
        return {
          code: `export default "data:application/wasm;base64,${b64}";\n`,
          map: null,
        };
      }
      const ref = this.emitFile({
        type: "asset",
        name: file.split("/").pop().replace(/\.rho$/, "") + ".wasm",
        source: entry.bytes,
      });
      return { code: `export default import.meta.ROLLUP_FILE_URL_${ref};\n`, map: null };
    },
  };
}

// named exports for programmatic consumers (tests, custom pipelines)
export { compileRho, loadGeneration, warmCompiler };
