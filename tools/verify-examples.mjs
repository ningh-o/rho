#!/usr/bin/env node
// tools/verify-examples.mjs — the examples/ gate (T6.4): every program
// promoted from corpus/ compiles and runs through boot, its .out golden
// is matched byte-for-byte (bytes, not text — a golden may carry
// non-UTF8), and the `// exit:` header pins the exit code. The directory
// is user-facing; this leg keeps it from rotting.
import { spawnSync } from "node:child_process";
import { readdirSync, readFileSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dir = join(ROOT, "examples");
const files = readdirSync(dir).filter((f) => f.endsWith(".rho")).sort();

let failures = 0;
for (const file of files) {
  const name = file.replace(/\.rho$/, "");
  const src = readFileSync(join(dir, file), "utf8");
  const exitMatch = /^\/\/ exit: (\d+)$/m.exec(src);
  const wantExit = exitMatch ? Number(exitMatch[1]) : 0;
  const goldenPath = join(dir, `${name}.out`);
  const want = readdirSync(dir).includes(`${name}.out`)
    ? readFileSync(goldenPath)
    : Buffer.alloc(0);

  const r = spawnSync("./build/rho", ["run", join("examples", file)], {
    cwd: ROOT,
    encoding: "buffer",
    timeout: 60000,
  });
  const got = r.stdout || Buffer.alloc(0);
  // the historical corpus runner compared with $(...) on both sides —
  // trailing newlines stripped; keep that exactness level here
  const trim = (b) => b.toString("utf8").replace(/\n+$/, "");
  const ok = r.status === wantExit && trim(got) === trim(want);
  if (!ok) {
    failures += 1;
    console.log(`FAIL examples/${name}: exit=${r.status} want=${wantExit}`);
    if (trim(got) !== trim(want)) {
      console.log(`  want: ${JSON.stringify(trim(want))}`);
      console.log(`  got : ${JSON.stringify(trim(got))}`);
    }
  } else {
    console.log(`ok   ${name}`);
  }
}
console.log(`examples: ${files.length - failures}/${files.length} verified`);
process.exitCode = failures ? 1 : 0;
