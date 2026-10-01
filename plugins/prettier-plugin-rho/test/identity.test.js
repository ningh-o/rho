// identity.test.js — the acceptance law (docs/ecosystem.md §3):
// formatting every program in the pinned subset through the plugin is
// byte-identical to `build/rho fmt` in this worktree. Both sides run
// live: the plugin path (prettier.format over the embedded fmt wasm)
// against boot's own fmt CLI on the same file. Comment-bearing files
// ride the same law since the comment replay landed in the rho-side
// formatter (the 2026-10-02 port; the file-header/above-decl/
// same-line-tail/block-close shapes all replay byte-identically).
import { describe, expect, it } from "vitest";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import prettier from "prettier";
import plugin from "../src/index.js";
import { corpusSubset, fmtSelfFixtures, worktreeRoot } from "./subset.js";

const boot = join(worktreeRoot, "build", "rho");

const bootFmt = (relPath) =>
  execFileSync(boot, ["fmt", join(worktreeRoot, relPath)], {
    timeout: 30_000,
  });

const viaPrettier = async (text, opts = {}) =>
  prettier.format(text, {
    parser: "rho",
    plugins: [plugin],
    ...opts,
  });

for (const f of [...corpusSubset, ...fmtSelfFixtures]) {
  it(`byte-identical to build/rho fmt: ${f}`, async () => {
    const src = readFileSync(join(worktreeRoot, f), "utf8");
    const want = bootFmt(f);
    const got = await viaPrettier(src);
    expect(Buffer.from(got, "utf8").equals(want), `${f} must match`).toBe(true);
  });
}

it("the pinned subset is non-empty (an empty law proves nothing)", () => {
  expect(corpusSubset.length + fmtSelfFixtures.length).toBeGreaterThan(0);
});

it("the subset exercises the comment replay (a comment-free law would not have caught the drop)", () => {
  const withComments = [...corpusSubset, ...fmtSelfFixtures].filter((f) =>
    readFileSync(join(worktreeRoot, f), "utf8").includes("//"),
  );
  expect(withComments.length).toBeGreaterThan(0);
});
