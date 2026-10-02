// wat.js — the WAT → wasm bridge. The compiler artifact this site ships
// is the self-hosted mirror, whose build face emits canonical WAT text on
// stdout (the repo's own chain assembles it with wat2wasm); the browser
// assembles the same text with wabt's wasm-backed assembler. One assembler
// concept, the same bytes.
//
// wabt is lazy: the module (self-contained, wasm embedded) instantiates on
// first use and stays cached.
import wabtInit from "./vendor/wabt.mjs";

let mod = null;
let pending = null;

function getWabt() {
  if (mod) return Promise.resolve(mod);
  if (!pending) {
    pending = Promise.resolve(wabtInit()).then((m) => {
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
