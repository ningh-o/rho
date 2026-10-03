// build-artifact.mjs — produce the prettier plugin's compiler artifact
// from the rho worktree and pin its generation.
//
// The artifact is the self-hosted mirror's app face — built the same
// way the chain builds itself (§7's chain law: MODS_APP left empty, the
// bake is byte-identical to the chain's own builds), then shrunk with
// the ledgered site-asset shrinker:
//
//   ./build/rho build libs/compiler/main.rho -o m-app.wasm --set "MODS_APP="
//   wasmtime (m-app.wasm, fmt face) == ./build/rho fmt   # fmt probe
//   wasm-opt -Oz --enable-bulk-memory --enable-multivalue m-app.wasm
//   wasmtime (the optimized artifact, fmt face) == boot fmt  # again
//
// WHY AN EMPTY MODS_APP (not the site's std bake): the app face runs
// parse.load_mods over every baked module BEFORE it reads /mode, and
// load_mods MERGES the resolved modules' declarations into the Prog —
// so with std baked, the fmt face formats the whole std tree appended
// after the user's program (proven: a std-using probe came back 2128
// bytes against boot fmt's 449). Boot fmt resolves a file's modules
// from disk as a gate, but formats ONLY the file's own code. The
// std-baked app face therefore cannot be byte-identical to `rho fmt`
// on any std-using program, and the empty bake is the only one under
// which the fmt face and boot fmt agree. fmt is syntax-level law: the
// fmt face never runs the checker and never emits, so it needs no
// modules at all — programs tier included (the three programs that use
// geom/web/pk format byte-identically to boot fmt through this
// artifact; the package's parity test pins that over all 96).
//
// No wabt is vendored here: the fmt face emits canonical TEXT, not
// WAT, so nothing in this package assembles. The wabt/wat2wasm version
// on the bake machine is recorded for the gate-toolchain record only.
//
// The generation record pins: the rho commit, sha256 over every boot /
// libs/compiler input, the bake tools' versions, and the artifact
// bytes. One generation, one package release (docs/ecosystem.md
// version policy) — generation.json is the package's ONLY generation
// pin.
//
// Determinism law: running this tool twice over the same inputs
// produces byte-identical artifacts (asserted below, two chain builds
// through wasm-opt).
//
// Usage: node tools/build-artifact.mjs [--check]
//   --check: rebuild and verify the committed artifact matches
//            (byte-identical wasm + identical generation record).
// Requires in the worktree root: ./build/rho (make all), wasmtime,
// wasm-opt — the same machine tools the repo's gate uses.

import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

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
["wasmtime", "wasm-opt"].forEach(need);
if (!existsSync(boot)) {
  console.error("build-artifact: ./build/rho missing — run `make all` in the worktree root first");
  process.exit(1);
}

// scratch under the worktree's tmp/ area (the parallel-work convention)
mkdirSync(join(worktree, "tmp"), { recursive: true });
const t = mkdtempSync(join(worktree, "tmp", "eco-prettier-bake-"));
const cleanup = () => rmSync(t, { recursive: true, force: true });
process.on("exit", cleanup);
process.on("SIGINT", () => process.exit(130));

// --set "MODS_APP=" is byte-identical to the const's default (asserted
// by the chain law's own determinism check below) — spelled out so the
// recipe shows the bake IS the app face, not an accident of omission
const build = (out) => {
  execFileSync(
    boot,
    ["build", "libs/compiler/main.rho", "-o", out, "--set", "MODS_APP="],
    { cwd: worktree, timeout: 300_000, stdio: ["ignore", "ignore", "inherit"] },
  );
};

// the fmt probe (the bake's acceptance leg): the artifact's fmt face
// must print boot fmt's canonical bytes. The probe source rides the
// SAME subset grammar the repo's own fmt-parity law pins
// (tests/run-fmt-self.sh: consts/statics/fns, if/while, strings with
// escapes, casts, comment replay incl. same-line tails) — the
// self-hosted fmt is a subset-grammar formatter by law, and the probe
// must live inside its pinned scope, not outside it.
const probeSource = `// a bake-time probe — the fmt face must match boot fmt byte for byte
const BASE: i64 = 10;
static mut HITS: i64 = 0;

fn scaled(mult: i64) -> i64 {
  return BASE * mult + HITS; // the tail comment rides above
}

fn main() -> i32 {
  let mut acc: i64 = 0;
  let tag: string = "rho\\ttabbed\\n";
  while acc < 3 {
    acc = acc + 1;
    if acc == 2 {
      HITS = HITS + 1;
    }
  }
  printf("{} {} {}\\n", acc, scaled(acc), len(tag));
  return 0;
}
`;
function probe(artifact) {
  // boot's canonical form (from the worktree root — the same way the
  // corpus recipes run it)
  const probeRho = join(t, "probe.rho");
  writeFileSync(probeRho, probeSource);
  const expected = execFileSync(boot, ["fmt", probeRho], {
    cwd: worktree, timeout: 60_000, encoding: "buffer", stdio: ["ignore", "pipe", "pipe"],
  });

  // the artifact's fmt face through the real wasmtime runtime: a room
  // holding /main.rho and /mode (exactly "fmt", no trailing newline —
  // the site's compiler.js writes it the same way)
  const room = join(t, "room-" + createHash("sha1").update(artifact).digest("hex").slice(0, 8));
  mkdirSync(room, { recursive: true });
  writeFileSync(join(room, "main.rho"), probeSource);
  writeFileSync(join(room, "mode"), "fmt");
  let got;
  try {
    got = execFileSync("wasmtime", ["run", "--dir", ".", artifact], {
      cwd: room, timeout: 60_000, encoding: "buffer", stdio: ["ignore", "pipe", "pipe"],
    });
  } catch (e) {
    console.error(`FAIL: the artifact's fmt face refused the probe (${artifact})`);
    console.error(String(e.stderr || e).split("\n").slice(0, 5).join("\n"));
    process.exit(1);
  }
  if (!got.equals(expected)) {
    console.error(`FAIL: the fmt face and boot fmt differ (${artifact})`);
    console.error(`  boot fmt: ${expected.length} bytes, fmt face: ${got.length} bytes`);
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

// determinism check: rebuild and compare (the whole chain, boot + opt)
const mApp2 = join(t, "m-app2.wasm");
build(mApp2);
if (!readFileSync(mApp).equals(readFileSync(mApp2))) {
  console.error("determinism alarm: two chain builds differ byte-wise");
  process.exit(1);
}
const optimized2 = join(t, "rho-compiler2.wasm");
execFileSync(
  "wasm-opt",
  ["-Oz", "--enable-bulk-memory", "--enable-multivalue", mApp2, "-o", optimized2],
  { timeout: 300_000, stdio: "ignore" },
);
if (!readFileSync(optimized).equals(readFileSync(optimized2))) {
  console.error("determinism alarm: two optimized builds differ byte-wise");
  process.exit(1);
}

// the generation inputs: the rho commit, every boot source, and the
// compiler's own library — the artifact rides nothing else (MODS_APP
// is empty; the fmt face bakes no module tree)
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

const generation = createHash("sha256")
  .update(
    Object.keys(genInputs)
      .sort()
      .map((k) => `${k} ${genInputs[k]}`)
      .join("\n"),
  )
  .digest("hex");

const artifact = readFileSync(optimized);
const toolVersion = (cmd) => {
  try {
    return execFileSync(cmd, ["--version"]).toString().trim();
  } catch {
    return "absent";
  }
};

const record = {
  generation,
  generationShort: generation.slice(0, 12),
  rhoCommit,
  chain:
    "boot (build/rho) + libs/compiler mirror, app face with EMPTY MODS_APP (the §7 chain-law shape: byte-identical to the chain's own builds) — the std-baked site recipe is NOT usable for fmt: load_mods merges the baked modules into the Prog before /mode is read, so the fmt face would format the whole std tree after the user's program, while boot fmt formats the file's own code only — then wasm-opt -Oz --enable-bulk-memory --enable-multivalue (tools/build-site.sh's shrinker step)",
  build:
    "./build/rho build libs/compiler/main.rho -o m-app.wasm --set 'MODS_APP=' && wasm-opt -Oz --enable-bulk-memory --enable-multivalue  # in the worktree root",
  artifact: "rho-compiler.wasm",
  artifactSha256: createHash("sha256").update(artifact).digest("hex"),
  artifactBytes: artifact.length,
  bakeTools: {
    "wasm-opt (binaryen)": toolVersion("wasm-opt"),
    wasmtime: toolVersion("wasmtime"),
    "wat2wasm (wabt)": toolVersion("wat2wasm"),
    note:
      "no wabt is vendored or used at run time — the fmt face emits canonical text, never WAT; the wat2wasm version is recorded only because the repo's gate toolchain carries it",
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
