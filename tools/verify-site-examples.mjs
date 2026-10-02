#!/usr/bin/env node
// tools/verify-site-examples.mjs — the site's copy cannot rot: every
// example in site/assets/examples.js runs through BOTH compilers and its
// `expect` is matched against stdout, byte for byte:
//   1. boot (`rho run`) — the language law;
//   2. the self-hosted mirror in its app face — the compiler the site
//      actually ships (a fresh per-program bake, then wat2wasm + wasmtime,
//      the corpus differential's own construction). A site example the
//      mirror cannot compile is a broken page, not a hypothetical.
// Both legs live in `make test`; editing an example into non-compiling or
// dishonest shape fails the same battery the compiler itself must pass.
import { spawnSync } from "node:child_process";
import {
  readFileSync,
  readdirSync,
  statSync,
  rmSync,
  mkdirSync,
  writeFileSync,
} from "node:fs";
import { join, resolve, dirname } from "node:path";
import { pathToFileURL } from "node:url";

const ROOT = resolve(dirname(new URL(import.meta.url).pathname), "..");
const examples = await import(
  pathToFileURL(join(ROOT, "site", "assets", "examples.js")).href
);

// inside the repo: `use std.io;` resolves std/ against the repository
// root ABOVE the example file, exactly like a user's checkout
const tmp = join(ROOT, "build", "site-examples");
rmSync(tmp, { recursive: true, force: true });
mkdirSync(tmp, { recursive: true });

function run(cmd, args, opts = {}) {
  return spawnSync(cmd, args, { cwd: ROOT, encoding: "utf8", timeout: 60000, ...opts });
}

// the mirror leg: bake the compiler with the program frozen in, compile
// it, assemble, run — the app face's three steps. The std tree rides the
// bake (repo-relative paths, the corpus differential's own law) so the
// `use std.io;` examples take the same face the site's compiler ships.
function stdMods() {
  const files = [];
  const walk = (rel) => {
    const full = join(ROOT, rel);
    for (const e of readdirSync(full)) {
      const p = rel ? `${rel}/${e}` : e;
      if (statSync(join(full, e)).isDirectory()) walk(p);
      else if (e.endsWith(".rho")) files.push(p);
    }
  };
  walk("std");
  return files
    .map((p) => `@MOD@ ${p}\n${readFileSync(join(ROOT, p), "utf8")}\n`)
    .join("");
}

function mirrorRun(file) {
  const baked = join(tmp, "mirror-c.wasm");
  const srcText = readFileSync(file, "utf8");
  const bake = run("./build/rho", [
    "build", "libs/compiler/main.rho", "-o", baked,
    "--set", `SRC=${srcText}`, "--set", `MODS=${stdMods()}`,
  ]);
  if (bake.status !== 0) return { stage: "bake", stderr: bake.stderr };
  const mirror = spawnSync("wasmtime", [baked], { cwd: ROOT, encoding: "utf8", timeout: 60000 });
  if (mirror.status !== 0) return { stage: "compile", stderr: mirror.stderr };
  const wat = join(tmp, "mirror-p.wat");
  const prog = join(tmp, "mirror-p.wasm");
  writeFileSync(wat, mirror.stdout);
  const asm = run("wat2wasm", [wat, "-o", prog]);
  if (asm.status !== 0) return { stage: "wat2wasm", stderr: asm.stderr };
  const got = spawnSync("wasmtime", [prog], { cwd: ROOT, encoding: "utf8", timeout: 60000 });
  return { stage: "ok", stdout: got.stdout, rc: got.status };
}

let failures = 0;
let n = 0;

for (const ex of examples.EXAMPLES) {
  n += 1;
  const file = join(tmp, `${ex.id}.rho`);
  writeFileSync(file, ex.code);

  // leg 1 — boot
  const r = run("./build/rho", ["run", file]);
  const out = (r.stdout || "").toString();
  const bootOk = r.status === 0 && (!ex.expect || out === ex.expect);

  // leg 2 — the self-hosted mirror (what the site ships)
  let mirrorOk = false;
  let note = "";
  if (bootOk) {
    const m = mirrorRun(file);
    if (m.stage !== "ok") {
      note = `mirror ${m.stage}: ${(m.stderr || "").split("\n")[0]}`;
    } else if (ex.expect != null && m.stdout !== ex.expect) {
      note = `mirror stdout drift: ${JSON.stringify(m.stdout)}`;
    } else if (m.rc !== 0) {
      note = `mirror rc ${m.rc}`;
    } else {
      mirrorOk = true;
    }
  }

  if (!bootOk || !mirrorOk) {
    failures += 1;
    console.log(`FAIL ${ex.id}${bootOk ? "" : " (boot)"}${mirrorOk ? "" : " (mirror)"}`);
    if (!bootOk) {
      console.log(`  boot: exit=${r.status}`);
      if (ex.expect != null && out !== ex.expect) {
        console.log(`  want: ${JSON.stringify(ex.expect)}`);
        console.log(`  got : ${JSON.stringify(out)}`);
      }
      if (r.stderr) console.log(`  stderr: ${r.stderr.split("\n")[0]}`);
    }
    if (note) console.log(`  ${note}`);
  } else {
    console.log(`ok   ${ex.id}`);
  }
}

rmSync(tmp, { recursive: true, force: true });
console.log(
  `site examples: ${n - failures}/${n} verified on boot AND the self-hosted mirror`,
);
process.exitCode = failures ? 1 : 0;
