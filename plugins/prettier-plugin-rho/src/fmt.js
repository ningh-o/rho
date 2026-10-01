// fmt.js — run the embedded rho fmt wasm.
//
// The artifact (assets/fmt.wasm) is the compiler's own fmt pipeline —
// boot (the seed compiler) compiled the vendored libs/compiler modules
// plus the fmtmain entry; see rho/ and tools/build-fmt.mjs. The plugin
// carries no formatting logic of its own: it writes the source into the
// wasm's pinned input slot, runs _start, and returns the canonical
// text from stdout verbatim.
//
// Runtime bridge: the kernel imports only fd_write + proc_exit (no
// args, no stdin — the kernel grows only with a mechanism), so the
// source rides linear memory. assets/generation.json pins the slot's
// data offset; the host writes the source bytes plus a NUL terminator
// there before calling _start. The module rescans nothing else: a raw
// NUL byte cannot appear in rho source (the escape spelling \0 is two
// ASCII bytes), so the terminator is unambiguous.
//
// The fixpoint gate: this generation's pipeline still ACCEPTS shapes it
// does not retain (structs, enums/match, floats, closures — the
// mirror's parse-retention gaps, TODO.md T2.2+) and mangles them
// silently. It never emits the canonical form for them, so the plugin
// runs the fmt twice and refuses the source unless the formatted form
// re-parses and formats back to the same bytes. Refusals throw
// FmtError carrying the compiler's own parse diagnosis — the caller
// (and prettier --write) never sees mangled text.

import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const pkgDir = join(dirname(fileURLToPath(import.meta.url)), "..");

export const generation = JSON.parse(
  readFileSync(join(pkgDir, "assets", "generation.json"), "utf8"),
);

// capacity is the USABLE source bytes: the raw slot literal is one
// byte longer (rawBytes); that last byte is reserved — the host
// writes the NUL there at run time (the build-time literal carries
// no NUL) — so a capacity-sized input plus its terminator always
// lands inside the slot's own bytes, never in the alignment padding
// behind it.
const { offset, capacity } = generation.inputSlot;

const moduleCache = new WebAssembly.Module(
  readFileSync(join(pkgDir, "assets", "fmt.wasm")),
);

export class FmtError extends Error {}

// one fresh instance per run: the module keeps parse state in its
// bump heap and proc_exit leaves globals set, so reuse would leak
// memory across runs (a format is two runs — see the fixpoint gate);
// a fresh instance is the deterministic unit
function instantiate() {
  const stdout = [];
  const stderr = [];
  let exitCode = null;
  const write = (fd, iovs, iovsLen, nWritten) => {
    const mem = new DataView(instance.exports.memory.buffer);
    const u8 = new Uint8Array(instance.exports.memory.buffer);
    let total = 0;
    for (let i = 0; i < iovsLen; i++) {
      const p = mem.getUint32(iovs + i * 8, true);
      const len = mem.getUint32(iovs + i * 8 + 4, true);
      (fd === 2 ? stderr : stdout).push(u8.slice(p, p + len));
      total += len;
    }
    mem.setUint32(nWritten, total, true);
    return 0;
  };
  // proc_exit ends every rho program (the kernel tail exits with
  // main's return); it surfaces here as the run's exit code
  const procExit = (code) => {
    exitCode = code;
    throw new ProcExit(code);
  };
  // the vendored compiler sources grew the std era, and the module's
  // import list now carries the kernel's whole wasi tail — the fmt
  // path itself never reads stdin, opens files, or closes handles,
  // so the three ride as structural stubs (a zero-byte read = EOF;
  // badf for the file faces, the kernel's own unopened-fd answer)
  const fdRead = (fd, iovs, iovsLen, nWritten) => {
    new DataView(instance.exports.memory.buffer).setUint32(
      nWritten,
      0,
      true,
    );
    return 0;
  };
  const fdClose = () => 8;
  const pathOpen = () => 8;
  const instance = new WebAssembly.Instance(moduleCache, {
    wasi_snapshot_preview1: {
      fd_write: write,
      fd_read: fdRead,
      fd_close: fdClose,
      path_open: pathOpen,
      proc_exit: procExit,
    },
  });
  return {
    instance,
    run() {
      try {
        instance.exports._start();
      } catch (e) {
        if (!(e instanceof ProcExit)) throw e;
      }
      return {
        code: exitCode,
        stdout: Buffer.concat(stdout),
        stderr: Buffer.concat(stderr),
      };
    },
  };
}

class ProcExit extends Error {
  constructor(code) {
    super(`proc_exit(${code})`);
  }
}

// one fmt pass: write `input` plus the NUL terminator into the pinned
// slot and run _start. `what` names the bytes in the capacity error
// ("input" for the first pass, "formatted output" for the gate's pass).
function runFmt(input, what) {
  if (input.length > capacity) {
    throw new FmtError(
      `${what} is ${input.length} bytes; this generation's fmt wasm carries a ` +
        `${capacity}-byte input slot (generation ${generation.generationShort})`,
    );
  }
  const run = instantiate();
  const mem = new Uint8Array(run.instance.exports.memory.buffer);
  mem.set(input, offset);
  mem[offset + input.length] = 0; // the terminator the scan looks for
  return run.run();
}

// rho's whitespace is exactly these four bytes (rho/lex.rho skips
// space, tab, CR, LF); blank source has nothing to format
function isBlank(input) {
  for (const b of input) {
    if (b !== 0x20 && b !== 0x09 && b !== 0x0d && b !== 0x0a) return false;
  }
  return true;
}

// format rho source; returns the canonical text byte-for-byte as the
// compiler's fmt would print it (its stdout, decoded utf-8) — but only
// if that text survives its own fmt pass (the fixpoint gate above).
// Known-lost shapes are refused with a parse diagnosis, never returned
// mangled; blank input returns "" without touching the wasm (the
// pipeline would synthesize a main skeleton for it — rho/fmt.rho:609 —
// and a formatter must not invent code).
export function formatRho(text) {
  const input = Buffer.from(text, "utf8");
  if (input.includes(0)) {
    throw new FmtError(
      "rho source contains a raw NUL byte; the \\0 escape spelling is the valid form",
    );
  }
  if (isBlank(input)) {
    return "";
  }
  const first = runFmt(input, "input");
  if (first.code !== 0) {
    throw new FmtError(
      `rho fmt failed (exit ${first.code}): ${first.stderr
        .toString("utf8")
        .trim()}`,
    );
  }
  const again = runFmt(first.stdout, "formatted output");
  if (again.code !== 0) {
    throw new FmtError(
      "rho fmt refused this source: its formatted form does not re-parse, " +
        "so the pipeline dropped shapes it does not retain yet (known: " +
        "structs, enums/match, floats, closures, ...). The compiler's own " +
        "diagnosis of the formatted form: " +
        again.stderr.toString("utf8").trim(),
    );
  }
  if (!again.stdout.equals(first.stdout)) {
    throw new FmtError(
      "rho fmt refused this source: two consecutive passes disagree " +
        "(the pipeline is not fixpoint-stable here), so no canonical " +
        "form can be trusted for it",
    );
  }
  return first.stdout.toString("utf8");
}
