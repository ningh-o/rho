// wat.js — the WAT -> wasm bridge, Node side.
//
// The compiler artifact emits canonical WAT text on stdout (the repo's
// own chain assembles it with wat2wasm); the plugin assembles the same
// text with the same wabt generation the site vendors
// (site/assets/vendor/wabt.mjs — copied verbatim into vendor/, its
// sha256 pinned in assets/generation.json). One assembler concept,
// the same bytes; the package's parity test proves byte identity
// against the CLI's wat2wasm over the whole programs tier.
//
// The vendored file is UMD: under Node ESM its emscripten glue calls
// `require("fs")`, which only exists on the CJS branch. So the Node
// load evaluates the file as CJS (its `module.exports` branch) with a
// real `require` — the file itself stays byte-verbatim with the site's.
//
// wabt is lazy: the module instantiates on first use and stays cached.

import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname } from "node:path";
import { fileURLToPath } from "node:url";

let mod = null;
let pending = null;

function loadWabt() {
  // the vendored UMD bundle evaluated on the CJS branch: strip the ESM
  // export tail, hand the glue a real require (node builtins) plus the
  // module/exports/__dirname/__filename frame so it takes
  // `module.exports = WabtModule` and resolves its script directory
  const path = fileURLToPath(new URL("../vendor/wabt.mjs", import.meta.url));
  let code = readFileSync(path, "utf8");
  code = code.replace(/export default WabtModule;\s*$/, "");
  const req = createRequire(path);
  const cjs = { exports: {} };
  new Function("require", "module", "exports", "__dirname", "__filename", code)(
    req, cjs, cjs.exports, dirname(path), path,
  );
  return cjs.exports;
}

function getWabt() {
  if (mod) return Promise.resolve(mod);
  if (!pending) {
    pending = Promise.resolve(loadWabt())
      .then((init) => init())
      .then((m) => {
        mod = m;
        return m;
      });
  }
  return pending;
}

// Assemble canonical WAT text into wasm bytes. Throws with wabt's
// diagnostic on malformed text — the compiler's own build already refused
// bad rho, so this only fires on a compiler bug, and the message names it.
export async function assembleWat(wat) {
  const m = await getWabt();
  const parsed = m.parseWat("out.wat", wat);
  const { buffer } = parsed.toBinary({
    log: false,
    write_debug_names: false,
  });
  return new Uint8Array(buffer);
}
