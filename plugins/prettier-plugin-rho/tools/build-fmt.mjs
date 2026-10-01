// build-fmt.mjs — produce the fmt wasm artifact from the bootstrap
// chain and pin its generation.
//
// The chain (run from the worktree root, where ./build/rho exists):
//
//   ./build/rho build plugins/prettier-plugin-rho/rho/fmtmain.rho \
//       -o <tmp>/fmt.wasm
//
// boot compiles the vendored libs/compiler modules (rho/lex.rho,
// rho/parse.rho, rho/fmt.rho — verbatim copies, hash-verified below)
// together with the fmtmain entry into a wasm module whose formatting
// truth is the compiler's own fmt pipeline. The emitted WAT is kept
// beside the output; this tool reads the input slot's data offset and
// size out of it (rawBytes = the folded literal's full length;
// capacity = usable source bytes = rawBytes - 1, one byte reserved for
// the NUL terminator), hashes the generation, and writes:
//
//   assets/fmt.wasm          the artifact
//   assets/generation.json   the pinned generation record
//
// Determinism law: running this tool twice over the same inputs
// produces byte-identical artifacts (asserted below).
//
// Usage: node tools/build-fmt.mjs [--check]
//   --check: rebuild and verify the committed artifact matches
//            (byte-identical wasm + identical generation hash +
//            identical inputSlot record).

import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";


const pkgDir = join(dirname(fileURLToPath(import.meta.url)), "..");
const worktree = join(pkgDir, "..", "..");
const boot = join(worktree, "build", "rho");
const entry = join(pkgDir, "rho", "fmtmain.rho");
const marker = "RHO_FMT_INPUT_SLOT_";

const sha = (p) => createHash("sha256").update(readFileSync(p)).digest("hex");

// the vendored modules must be verbatim copies of libs/compiler —
// the compiler's fmt pipeline IS the formatting truth; a drifted copy
// would silently fork it
const vendored = ["lex.rho", "parse.rho", "fmt.rho"];
const genInputs = {};
for (const f of vendored) {
  const mine = sha(join(pkgDir, "rho", f));
  const theirs = sha(join(worktree, "libs", "compiler", f));
  if (mine !== theirs) {
    console.error(`vendor drift: rho/${f} differs from libs/compiler/${f}`);
    console.error("re-copy from libs/compiler and rebuild the artifact");
    process.exit(1);
  }
  genInputs[`rho/${f}`] = mine;
}
genInputs["rho/fmtmain.rho"] = sha(entry);
// the boot generation that produced the artifact
for (const f of execFileSync("ls", ["boot"], { cwd: worktree })
  .toString()
  .split("\n")
  .filter((f) => /\.(c|h)$/.test(f))
  .sort()) {
  genInputs[`boot/${f}`] = sha(join(worktree, "boot", f));
}
genInputs["boot/wat/kernel.wat"] = sha(join(worktree, "boot", "wat", "kernel.wat"));

const generation = createHash("sha256")
  .update(
    Object.keys(genInputs)
      .sort()
      .map((k) => `${k} ${genInputs[k]}`)
      .join("\n"),
  )
  .digest("hex");

// build through the chain, time-capped; scratch lives under the
// worktree's tmp/eco-prettier-* area (the parallel-work convention)
mkdirSync(join(worktree, "tmp"), { recursive: true });
const t = mkdtempSync(join(worktree, "tmp", "eco-prettier-fmtbuild-"));
const cleanup = () => rmSync(t, { recursive: true, force: true });
process.on("exit", cleanup);
process.on("SIGINT", () => process.exit(130));
const outWasm = join(t, "fmt.wasm");
execFileSync(boot, ["build", entry, "-o", outWasm], {
  cwd: worktree,
  timeout: 300_000,
  stdio: ["ignore", "ignore", "inherit"],
});

// the input slot: one folded data literal whose bytes the host
// overwrites at runtime (source + NUL terminator). Find its offset and
// capacity in the WAT boot kept beside the output.
const wat = readFileSync(`${outWasm}.wat`, "utf8");
const dataRe = new RegExp('\\(data \\(i32\\.const (\\d+)\\) "' + marker + '(A*)"');
const m = wat.match(dataRe);
if (!m) {
  console.error("input slot literal not found in the emitted WAT");
  process.exit(1);
}
const offset = Number(m[1]);
const rawBytes = marker.length + m[2].length; // the folded literal's full length
const capacity = rawBytes - 1; // usable source bytes; the last raw byte is reserved (the host writes the NUL at run time)

// determinism check: rebuild and compare
const outWasm2 = join(t, "fmt2.wasm");
execFileSync(boot, ["build", entry, "-o", outWasm2], {
  cwd: worktree,
  timeout: 300_000,
  stdio: ["ignore", "ignore", "inherit"],
});
if (!readFileSync(outWasm).equals(readFileSync(outWasm2))) {
  console.error("determinism alarm: two chain builds differ byte-wise");
  process.exit(1);
}

const artifact = readFileSync(outWasm);
const record = {
  generation,
  generationShort: generation.slice(0, 12),
  chain: "boot (build/rho) + libs/compiler fmt pipeline (vendored: lex, parse, fmt) + rho/fmtmain.rho",
  build: "./build/rho build plugins/prettier-plugin-rho/rho/fmtmain.rho -o fmt.wasm  # in the worktree root",
  inputSlot: {
    offset,
    rawBytes,
    capacity,
    terminator: "NUL",
    bridge: "host writes source bytes + NUL at offset, then calls _start",
    semantics:
      "capacity = usable source bytes = rawBytes - 1: the raw data literal is the full folded const (marker + fill) and carries no NUL; its last byte is reserved — the host writes the NUL there at run time — so source + terminator never write past the literal",
  },
  inputs: genInputs,
};

if (process.argv.includes("--check")) {
  const committed = readFileSync(join(pkgDir, "assets", "fmt.wasm"));
  const committedRec = JSON.parse(
    readFileSync(join(pkgDir, "assets", "generation.json"), "utf8"),
  );
  if (!committed.equals(artifact) || committedRec.generation !== generation) {
    console.error("stale artifact: assets/fmt.wasm does not match the current chain");
    console.error("re-run: node tools/build-fmt.mjs");
    process.exit(1);
  }
  if (
    JSON.stringify(committedRec.inputSlot) !== JSON.stringify(record.inputSlot)
  ) {
    console.error("stale record: generation.json's inputSlot does not match the emitted WAT");
    console.error("re-run: node tools/build-fmt.mjs");
    process.exit(1);
  }
  console.log(`artifact check ok (generation ${record.generationShort})`);
} else {
  writeFileSync(join(pkgDir, "assets", "fmt.wasm"), artifact);
  writeFileSync(
    join(pkgDir, "assets", "generation.json"),
    JSON.stringify(record, null, 2) + "\n",
  );
  console.log(
    `fmt.wasm: ${artifact.length} bytes, input slot at ${offset} (usable ${capacity} + NUL, raw ${rawBytes}), generation ${record.generationShort}`,
  );
}
