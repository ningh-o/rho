// plugin.test.js — the vite contract: a real vite build importing a
// .rho module, in both emit modes. The wasm that arrives in the
// bundle is the plugin's own compile output — the same bytes the
// parity law pins. The consumer module uses the imported value the
// way real embedders do (fetch + instantiateStreaming), because vite
// tree-shakes an entry's never-used exports.

import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build } from "vite";
import rho from "../src/index.js";
import { compileRho, warmCompiler } from "../src/compiler.js";

const PROGRAM = `use std.io;
fn main() -> i32 {
  printf("bundled\\n");
  return 7;
}
`;

const ENTRY = `import wasmUrl from "./prog.rho";
export async function boot() {
  const { instance } = await WebAssembly.instantiateStreaming(fetch(wasmUrl));
  return instance.exports;
}
globalThis.__rhoProgramUrl = wasmUrl;
`;

function makeApp(mode) {
  const root = mkdtempSync(join(tmpdir(), "vite-plugin-rho-"));
  mkdirSync(join(root, "src"), { recursive: true });
  writeFileSync(join(root, "src", "prog.rho"), PROGRAM);
  writeFileSync(
    join(root, "src", "entry.js"),
    mode === "refuse"
      ? 'import "./prog.rho";\n'
      : ENTRY,
  );
  if (mode === "refuse") writeFileSync(join(root, "src", "prog.rho"), "fn main() -> i32 { return oops; }\n");
  return root;
}

async function buildWith(plugin, mode) {
  const root = makeApp(mode);
  try {
    const result = await build({
      root,
      configFile: false,
      logLevel: "error",
      appType: "custom",
      plugins: [plugin],
      build: {
        write: false,
        target: "esnext",
        rollupOptions: { input: { entry: join(root, "src", "entry.js") } },
      },
    });
    return (Array.isArray(result) ? result[0] : result).output;
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

const WASM_MAGIC = new Uint8Array([0x00, 0x61, 0x73, 0x6d]);

test("vite build (asset mode): the .rho import resolves to the compiled wasm asset", { timeout: 120_000 }, async () => {
  await warmCompiler();
  const output = await buildWith(rho());
  const entry = output.find((o) => o.type === "chunk" && o.isEntry);
  assert.ok(entry, "an entry chunk was built");
  const assets = output.filter((o) => o.type === "asset" && o.fileName.endsWith(".wasm"));
  assert.equal(assets.length, 1, "exactly one .rho compiles to one wasm asset");
  const bytes = new Uint8Array(assets[0].source);
  assert.deepEqual(bytes.slice(0, 4), WASM_MAGIC, "the asset is a wasm module");
  // and it IS the plugin's own compile output
  const direct = await compileRho(PROGRAM);
  assert.ok(direct.ok);
  assert.equal(Buffer.compare(Buffer.from(bytes), Buffer.from(direct.bytes)), 0);
  // the chunk wires the asset through rollup's file-URL form
  assert.match(entry.code, /new URL\("prog-[^"]+\.wasm",\s*import\.meta\.url\)/);
});

test("vite build (inline mode): the .rho import default-exports a wasm data URL", { timeout: 120_000 }, async () => {
  await warmCompiler();
  const output = await buildWith(rho({ inline: true }));
  const entry = output.find((o) => o.type === "chunk" && o.isEntry);
  assert.ok(entry, "an entry chunk was built");
  const m = /"data:application\/wasm;base64,([A-Za-z0-9+/=]+)"/.exec(entry.code);
  assert.ok(m, "the chunk carries the base64 wasm data URL");
  const bytes = Buffer.from(m[1], "base64");
  assert.equal(bytes.subarray(0, 4).toString("hex"), "0061736d", "the decoded data URL is a wasm module");
  const direct = await compileRho(PROGRAM);
  assert.equal(Buffer.compare(bytes, Buffer.from(direct.bytes)), 0);
});

test("vite build: a refusing .rho fails the build with the compiler's diagnostics", { timeout: 120_000 }, async () => {
  await warmCompiler();
  await assert.rejects(
    () => buildWith(rho(), "refuse"),
    (e) => /rho build refused/.test(String(e?.message ?? e)) && /unknown name/.test(String(e?.message ?? e)),
    "the build must refuse with the compiler's own stderr",
  );
});
