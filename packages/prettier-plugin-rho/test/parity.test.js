// parity.test.js — acceptance leg (a): the plugin is a thin client.
//
// For every programs-tier program (tests/run-corpus-diff.sh's PINNED
// base, 96 top-level *.rho), the text prettier emits — through the
// plugin's parser (fmt wasm), printer (verbatim), and prettier's own
// doc-to-string pipeline — must be BYTE-IDENTICAL to the fmt face's
// stdout over the text prettier hands the parser. Nothing may be
// added, trimmed, normalized, or re-laid-out on the plugin's watch.
//
// The one input prettier itself normalizes is line endings: its
// format() runs normalizeEndOfLine over the source BEFORE any parser
// runs (the endOfLine law, universal across languages). The
// expectation below mirrors that exactly — for the one programs-tier
// file carrying a raw CR (066, the escapes fixture), the fmt face is
// compared over the normalized text, and a dedicated test pins that
// normalization as the sole divergence from the raw-text face.
//
// The second describe pins the same law against boot's CLI on the
// repo's own fmt-law suite (tests/fmt-self/*.rho — the scope
// tests/run-fmt-self.sh pins): plugin output == `rho fmt` output,
// byte for byte.

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import prettier from "prettier";
import plugin from "../src/index.js";
import { fmtRho } from "../src/runtime.js";
import { programList, readProgram, fmtSelfList, readFmtSelf, bootPath, worktree } from "../tools/corpora.mjs";

const formatOptions = { parser: "rho", plugins: [plugin] };

// prettier's own input law (src/index.js format()): a source
// containing \r is normalized (\r\n and lone \r -> \n) before any
// parser runs. The plugin never sees the raw bytes.
function prettierInputLaw(source) {
  return source.includes("\r") ? source.replace(/\r\n?/g, "\n") : source;
}

for (const rel of programList()) {
  test(`parity through prettier: ${rel}`, async () => {
    const source = readProgram(rel);
    const expected = fmtRho(prettierInputLaw(source));
    assert.equal(expected.ok, true, `the fmt face refused ${rel}: ${expected.stderr}`);

    const formatted = await prettier.format(source, formatOptions);
    assert.ok(
      Buffer.from(formatted, "utf8").equals(Buffer.from(expected.text, "utf8")),
      `prettier's output differs from the fmt face for ${rel}`,
    );
  });
}

// the raw-CR file(s): prettier normalizes the SOURCE's line endings
// before any parser runs (its universal endOfLine law), so the plugin
// never sees the raw byte. The honest pin: the plugin's output equals
// the fmt face's output over the exact text prettier hands it — the
// plugin adds no divergence of its own. (066 is the one tier file
// carrying a raw CR; see README "Known limitations" for what the
// normalization does to its `// out:` comment.)
for (const rel of programList()) {
  const source = readProgram(rel);
  if (!source.includes("\r")) continue;
  test(`raw CR rides prettier's endOfLine law: ${rel}`, async () => {
    const faceOverPrettierInput = fmtRho(prettierInputLaw(source));
    assert.ok(faceOverPrettierInput.ok);
    const formatted = await prettier.format(source, formatOptions);
    assert.ok(
      Buffer.from(formatted, "utf8").equals(Buffer.from(faceOverPrettierInput.text, "utf8")),
      `the CR divergence is not purely prettier's endOfLine law for ${rel}`,
    );
  });
}

const boot = bootPath();

for (const name of fmtSelfList()) {
  test(
    `parity with rho fmt CLI: ${name}`,
    { skip: boot ? false : "./build/rho is not built — run `make all` in the worktree root" },
    async () => {
      const source = readFmtSelf(name);
      const expected = execFileSync(boot, ["fmt", name], {
        cwd: worktree,
        timeout: 60_000,
        encoding: "buffer",
        stdio: ["ignore", "pipe", "pipe"],
      });
      const formatted = await prettier.format(source, formatOptions);
      assert.ok(
        Buffer.from(formatted, "utf8").equals(expected),
        `prettier's output differs from rho fmt for ${name}`,
      );
    },
  );
}
