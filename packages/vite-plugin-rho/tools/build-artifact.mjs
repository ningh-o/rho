// build-artifact.mjs — produce the vite plugin's compiler artifact from
// the rho worktree and pin its generation.
//
// The artifact is the self-hosted mirror in its app configuration —
// exactly the site's recipe (tools/build-app-artifact.sh + the
// build-site.sh shrinker step):
//
//   ./build/rho build libs/compiler/main.rho -o m-app.wasm \
//       --set "MODS_APP=<the std tree>"
//   wasmtime m-app.wasm        # greet probe through the runtime face
//   wasm-opt -Oz --enable-bulk-memory --enable-multivalue m-app.wasm
//   wasmtime (the optimized artifact)   # greet probe again
//
// MODS_APP bakes the reserved std/ tree in the same glob order the
// repo's zsh recipes use (std/*.rho, then std/*/*.rho, then
// std/*/*/*.rho — each level sorted); the app face resolves a
// program's `use std.io;` against it at run time. The emitted WAT of
// the optimized artifact is byte-identical to the unoptimized one
// (wasm-opt preserves the compiler's semantics; asserted by the
// package's parity test over the whole programs tier).
//
// The generation record pins: the rho commit, sha256 over every boot /
// libs/compiler / std input, the vendored wabt assembler, and the
// artifact bytes. One generation, one package release
// (docs/ecosystem.md version policy) — and generation.json is the
// package's ONLY generation pin.
//
// Determinism law: running this tool twice over the same inputs
// produces byte-identical artifacts (asserted below).
//
// Usage: node tools/build-artifact.mjs [--check]
//   --check: rebuild and verify the committed artifact matches
//            (byte-identical wasm + identical generation record).
// Requires in the worktree root: ./build/rho (make all), wasmtime,
// wat2wasm, wasm-opt — the same machine tools the repo's gate uses.

import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { globRho } from "./glob.mjs";

const pkgDir = join(dirname(fileURLToPath(import.meta.url)), "..");
const worktree = join(pkgDir, "..", "..");
const boot = join(worktree, "build", "rho");

const sha = (p) => createHash("sha256").update(readFileSync(p)).digest("hex");

function need(cmd) {
  try {
    execFileSync("which", [cmd], { stdio: "ignore" });
  } catch {
    console.error(`build-artifact: ${cmd} not found on PATH — the bake needs the repo's gate tools`);
    process.exit(1);
  }
}
["wasmtime", "wat2wasm", "wasm-opt"].forEach(need);
if (!existsSync(boot)) {
  console.error("build-artifact: ./build/rho missing — run `make all` in the worktree root first");
  process.exit(1);
}

// the std tree, in the recipes' glob order (zsh sorts each level;
// load order is part of the artifact's identity — the module table's
// registration order rides it)
const stdGlobs = ["std/*.rho", "std/*/*.rho", "std/*/*/*.rho"];
const stdFiles = [];
for (const g of stdGlobs) {
  stdFiles.push(...globRho(worktree, g));
}
if (stdFiles.length === 0) {
  console.error("build-artifact: the std tree is empty — bake from the rho worktree");
  process.exit(1);
}
let cmods = "";
for (const rel of stdFiles) {
  // the recipe's "$(cat f)" semantics: trailing newlines stripped,
  // exactly one added — MODS_APP must be byte-identical to what
  // tools/build-app-artifact.sh bakes
  const text = readFileSync(join(worktree, rel), "utf8").replace(/\n+$/, "");
  cmods += `@MOD@ ${rel}\n${text}\n`;
}

// scratch under the worktree's tmp/ area (the parallel-work convention)
mkdirSync(join(worktree, "tmp"), { recursive: true });
const t = mkdtempSync(join(worktree, "tmp", "eco-vite-bake-"));
const cleanup = () => rmSync(t, { recursive: true, force: true });
process.on("exit", cleanup);
process.on("SIGINT", () => process.exit(130));

const build = (out) => {
  execFileSync(
    boot,
    ["build", "libs/compiler/main.rho", "-o", out, "--set", `MODS_APP=${cmods}`],
    { cwd: worktree, timeout: 300_000, stdio: ["ignore", "ignore", "inherit"] },
  );
};

// the greet probe (build-app-artifact.sh's law): a std.io program
// through the runtime face, compiled end-to-end
const greetSource = `use std.io;
fn main() -> i32 {
  let got = io.read_line();
  let name: string = match got {
    Result.Ok(inner) => match inner {
      Option.Some(s) => s,
      Option.None => "",
    },
    Result.Err(_) => "",
  };
  printf("hi, {}\\n", name);
  return 0;
}
`;
function probe(artifact) {
  const room = join(t, "probe-" + createHash("sha1").update(artifact).digest("hex").slice(0, 8));
  mkdirSync(room, { recursive: true });
  writeFileSync(join(room, "main.rho"), greetSource);
  let wat;
  try {
    wat = execFileSync("wasmtime", ["run", "--dir", ".", artifact], {
      cwd: room, timeout: 60_000, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"],
    });
  } catch (e) {
    console.error(`FAIL: the artifact refused the greet probe (${artifact})`);
    console.error(String(e.stderr || e).split("\n").slice(0, 5).join("\n"));
    process.exit(1);
  }
  const watPath = join(room, "probe.wat");
  writeFileSync(watPath, wat);
  const prog = join(room, "probe.wasm");
  execFileSync("wat2wasm", [watPath, "-o", prog], { timeout: 60_000, stdio: "ignore" });
  const got = execFileSync("wasmtime", [prog], {
    input: "rho\n", timeout: 60_000, encoding: "utf8", stdio: ["pipe", "pipe", "ignore"],
  });
  if (got !== "hi, rho\n" && got.trimEnd() !== "hi, rho") {
    console.error(`FAIL: the probe ran but printed [${got}]`);
    process.exit(1);
  }
  rmSync(room, { recursive: true, force: true });
}

const mApp = join(t, "m-app.wasm");
build(mApp);
probe(mApp);

const optimized = join(t, "rho-compiler.wasm");
execFileSync(
  "wasm-opt",
  ["-Oz", "--enable-bulk-memory", "--enable-multivalue", mApp, "-o", optimized],
  { timeout: 300_000, stdio: "ignore" },
);
probe(optimized);

// determinism check: rebuild and compare
const mApp2 = join(t, "m-app2.wasm");
build(mApp2);
if (!readFileSync(mApp).equals(readFileSync(mApp2))) {
  console.error("determinism alarm: two chain builds differ byte-wise");
  process.exit(1);
}

// the generation inputs: the rho commit, every boot source, the
// compiler's own library, the baked std tree, and the vendored wabt
// assembler (byte-parity rides the same wabt generation)
const genInputs = {};
let rhoCommit = "unknown";
try {
  rhoCommit = execFileSync("git", ["rev-parse", "HEAD"], { cwd: worktree }).toString().trim();
} catch {}
genInputs["#rhoCommit"] = rhoCommit;
for (const f of execFileSync("ls", ["boot"], { cwd: worktree }).toString().split("\n").sort()) {
  if (/\.(c|h)$/.test(f)) genInputs[`boot/${f}`] = sha(join(worktree, "boot", f));
}
genInputs["boot/wat/kernel.wat"] = sha(join(worktree, "boot", "wat", "kernel.wat"));
for (const f of execFileSync("ls", ["libs/compiler"], { cwd: worktree }).toString().split("\n").sort()) {
  if (/\.rho$/.test(f)) genInputs[`libs/compiler/${f}`] = sha(join(worktree, "libs", "compiler", f));
}
for (const rel of stdFiles) genInputs[rel] = sha(join(worktree, rel));
genInputs["vendor/wabt.mjs"] = sha(join(pkgDir, "vendor", "wabt.mjs"));

const generation = createHash("sha256")
  .update(
    Object.keys(genInputs)
      .sort()
      .map((k) => `${k} ${genInputs[k]}`)
      .join("\n"),
  )
  .digest("hex");

const artifact = readFileSync(optimized);
const wat2wasmVersion = execFileSync("wat2wasm", ["--version"]).toString().trim();

const record = {
  generation,
  generationShort: generation.slice(0, 12),
  rhoCommit,
  chain:
    "boot (build/rho) + libs/compiler mirror, app face — tools/build-app-artifact.sh recipe (MODS_APP = the std tree), then wasm-opt -Oz --enable-bulk-memory --enable-multivalue (tools/build-site.sh's shrinker step)",
  build: "./build/rho build libs/compiler/main.rho -o m-app.wasm --set 'MODS_APP=<std tree>' && wasm-opt -Oz --enable-bulk-memory --enable-multivalue  # in the worktree root",
  artifact: "rho-compiler.wasm",
  artifactSha256: createHash("sha256").update(artifact).digest("hex"),
  artifactBytes: artifact.length,
  cliTools: { wat2wasm: wat2wasmVersion },
  wabt: {
    vendor: "vendor/wabt.mjs",
    sha256: genInputs["vendor/wabt.mjs"],
    note: "assembles the compiler's WAT byte-identically to wat2wasm " + wat2wasmVersion + " (proven over the whole programs tier by test/parity.test.js)",
  },
  inputs: genInputs,
};

if (process.argv.includes("--check")) {
  const committed = readFileSync(join(pkgDir, "assets", "rho-compiler.wasm"));
  const committedRec = JSON.parse(readFileSync(join(pkgDir, "assets", "generation.json"), "utf8"));
  if (!committed.equals(artifact) || committedRec.generation !== generation) {
    console.error("stale artifact: assets/rho-compiler.wasm does not match the current chain");
    console.error("re-run: node tools/build-artifact.mjs");
    process.exit(1);
  }
  if (committedRec.artifactSha256 !== record.artifactSha256 || committedRec.artifactBytes !== record.artifactBytes) {
    console.error("stale record: generation.json does not match the rebuilt artifact");
    console.error("re-run: node tools/build-artifact.mjs");
    process.exit(1);
  }
  console.log(`artifact check ok (generation ${record.generationShort})`);
} else {
  writeFileSync(join(pkgDir, "assets", "rho-compiler.wasm"), artifact);
  writeFileSync(join(pkgDir, "assets", "generation.json"), JSON.stringify(record, null, 2) + "\n");
  console.log(
    `rho-compiler.wasm: ${artifact.length} bytes (generation ${record.generationShort}, rho ${rhoCommit.slice(0, 12)})`,
  );
}
