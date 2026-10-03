// glob.mjs — the one glob the package needs: repo-relative "*.rho"
// globs with N wildcard directory levels, name-sorted per level —
// the order zsh's glob expansion produces for the recipes
// (tools/build-app-artifact.sh, tests/run-corpus-diff.sh). The baked
// module tree's registration order rides this order, so the bake tool
// and the test harness MUST share one implementation.

import { readdirSync, existsSync } from "node:fs";
import { join } from "node:path";

// globRho(root, "std/*/*.rho") -> ["std/collections/lib.rho", ...]
export function globRho(root, g) {
  const segs = g.split("/");
  const idx = segs.findIndex((s) => s === "*");
  const fixed = segs.slice(0, idx).join("/");
  const out = [];
  const rec = (base, rest, rel) => {
    if (!existsSync(base)) return;
    const entries = readdirSync(base, { withFileTypes: true })
      .filter((e) => (rest.length === 1 ? e.isFile() && e.name.endsWith(".rho") : e.isDirectory()))
      .map((e) => e.name)
      .sort();
    for (const name of entries) {
      if (rest.length === 1) {
        out.push([...rel, name].join("/"));
      } else {
        rec(join(base, name), rest.slice(1), [...rel, name]);
      }
    }
  };
  rec(join(root, fixed), segs.slice(idx), []);
  return out.map((rel) => (fixed ? `${fixed}/${rel}` : rel));
}
