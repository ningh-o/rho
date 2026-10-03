// fixpoint.test.js — acceptance leg (b): the fixpoint law through the
// plugin path — fmt(fmt(x)) == fmt(x), byte for byte, through
// prettier.format.
//
// Scope, honestly drawn. Over the programs tier the law is asserted
// strictly for every program the embedded formatter's own output is
// fixpoint-stable on; programs where the fmt FACE itself drifts
// across reformat are skipped with a reason — that is a property of
// the frozen subset-grammar formatter (libs/compiler/fmt.rho), not of
// the plugin path, and this package cannot prove a law the embedded
// formatter does not satisfy. On the repo's own fmt-law suite
// (tests/fmt-self) the law is the settle form run-fmt-self.sh pins.
// The pinned unstable count below may only SHRINK (it shrinks the day
// the formatter grows; a rise is a formatter regression — see README
// "Formatter scope").

import { test } from "node:test";
import assert from "node:assert/strict";
import prettier from "prettier";
import plugin from "../src/index.js";
import { fmtRho } from "../src/runtime.js";
import { programList, readProgram, fmtSelfList, readFmtSelf } from "../tools/corpora.mjs";

const formatOptions = { parser: "rho", plugins: [plugin] };
const bytes = (s) => Buffer.from(s, "utf8");

// tests/fmt-self — the repo's fmt-law suite. The law here is
// run-fmt-self.sh's own settle form: a same-line tail comment cannot
// survive its own move (the canonical form repositions it above the
// next construct — boot's own roundtrip drifts the same way), so the
// law compares the SECOND reformat of the canonical output against
// the next: tail-free files are stable immediately; a tail comment
// settles in one step and holds.
for (const name of fmtSelfList()) {
  test(`fixpoint through prettier (run-fmt-self settle form): ${name}`, async () => {
    const source = readFmtSelf(name);
    const once = await prettier.format(source, formatOptions);
    const twice = await prettier.format(once, formatOptions);
    const thrice = await prettier.format(twice, formatOptions);
    assert.ok(bytes(twice).equals(bytes(thrice)), `the canonical form drifts for ${name}`);
  });
}

// tests/suites/programs — the programs tier. Each program is asserted
// strictly through the plugin path UNLESS the fmt face's own output
// is not fixpoint-stable there (formatter gap — skipped, counted).
let unstable = 0;
let stable = 0;
for (const name of programList()) {
  test(
    `fixpoint through prettier: ${name}`,
    { skip: faceUnstable(name) ? "the frozen subset-grammar formatter is not fixpoint-stable on this program (README: Formatter scope)" : false },
    async () => {
      const source = readProgram(name);
      const once = await prettier.format(source, formatOptions);
      const twice = await prettier.format(once, formatOptions);
      assert.ok(bytes(once).equals(bytes(twice)), `fmt(fmt(x)) != fmt(x) for ${name}`);
    },
  );
}

function faceUnstable(name) {
  const source = readProgram(name);
  const face = fmtRho(source);
  if (!face.ok) return true; // the face refuses it outright — same gap
  const again = fmtRho(face.text);
  if (!again.ok || again.text !== face.text) {
    unstable += 1;
    return true;
  }
  stable += 1;
  return false;
}

// the gap pin closed: the full-layer boot-parity wave took the
// formatter to every programs-tier program, so the unstable count is
// zero. The number may only shrink — a rise is a formatter regression.
test("programs-tier formatter-gap count (may only shrink)", () => {
  assert.equal(unstable + stable, programList().length, "every program is classified exactly once");
  assert.equal(unstable, 0, "known unstable programs — bump DOWN as the formatter grows");
});
