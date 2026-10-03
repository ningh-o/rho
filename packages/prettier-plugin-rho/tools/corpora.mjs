// corpora.mjs — the corpora the acceptance tests ride, and the boot
// binary when it is present.
//
// The lists ride tools/glob.mjs (the same implementation the bake
// tool uses — one glob, one zsh order). The programs tier is the
// TOP-LEVEL *.rho of tests/suites/programs/ — exactly what
// tests/run-corpus-diff.sh walks (its PINNED floor, 96 today); the
// geom/, web/, pk/ subdirectories are that differential's module
// trees, not programs, and the .out beside them is a golden.

import { readFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { globRho } from "./glob.mjs";

const toolsDir = dirname(fileURLToPath(import.meta.url));
export const pkgDir = join(toolsDir, "..");
export const worktree = join(pkgDir, "..", "..");

// tests/run-corpus-diff.sh's PINNED base: the programs tier, zsh order
export function programList() {
  return globRho(worktree, "tests/suites/programs/*.rho");
}

// the repo's own fmt-law suite (tests/run-fmt-self.sh): the subset
// grammar the self-hosted formatter is pinned on, zsh order
export function fmtSelfList() {
  return globRho(worktree, "tests/fmt-self/*.rho");
}

function readText(path) {
  const bytes = readFileSync(path);
  const text = bytes.toString("utf8");
  // the byte-parity claims are only meaningful over losslessly
  // decodable sources; assert the roundtrip once, here
  if (!Buffer.from(text, "utf8").equals(bytes)) {
    throw new Error(`${path} is not valid UTF-8`);
  }
  return text;
}

export function readProgram(rel) {
  return readText(join(worktree, rel));
}

export function readFmtSelf(rel) {
  return readText(join(worktree, rel));
}

// boot's CLI (./build/rho) — present when the worktree is built; the
// `rho fmt` comparison legs ride it and skip otherwise
export function bootPath() {
  const boot = join(worktree, "build", "rho");
  return existsSync(boot) ? boot : null;
}
