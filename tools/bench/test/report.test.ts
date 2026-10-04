// Pins: docs/ecosystem.md section 1 — "Same tree, same machine,
// byte-identical numbers", operationalized as the ask's four
// properties: results sorted, fixed width, no timestamps, no
// machine-local absolute paths (design section 5 applied to the
// harness itself). The golden below is hand-computed: every row line
// is exactly 63 columns for this fixture, and the DNF cells — the cap
// cell, the starvation sentence, the exit cell — are exactly as wide
// as the measured cells.

import test from "node:test";
import assert from "node:assert/strict";

import {
  renderReport,
  type BenchReport,
  type LegReport,
  type Row,
} from "../src/report.ts";

function ok(name: string, ms: number): Row {
  return { name, status: "ok", ms, capMs: 30000, exitCode: 0, reason: "" };
}
function dnfCap(name: string): Row {
  return { name, status: "dnf", ms: 0, capMs: 30000, exitCode: -1, reason: "" };
}
function dnfStarved(name: string): Row {
  return {
    name,
    status: "dnf",
    ms: 0,
    capMs: 30000,
    exitCode: -1,
    reason: "stage starved by a dead upstream stage",
  };
}
function err(name: string, code: number): Row {
  return { name, status: "err", ms: 0, capMs: 30000, exitCode: code, reason: "" };
}

const compileLeg: LegReport = {
  title: "compile (boot build, median of 2)",
  rows: [
    ok("001_hello", 12.34),
    ok("015_panic_oob", 9.8),
    dnfCap("t06_params_widen"),
    dnfStarved("t07_starved"),
    err("t08_x", 1),
  ],
  skipped: false,
  skipReason: "",
};

const skippedChain: LegReport = {
  title:
    "chain (mirror build -> mirror run -> child assemble, median of 1)",
  rows: [],
  skipped: true,
  skipReason:
    "pass --chain (off until T3.1 leg 4, the self chain, is green)",
};

// The golden. Hand-computed against the render rules: name column = 16
// (the longest name), status column = 3, time cell = 38 (the widest
// cell — the starvation sentence), two-space gutters, two-space
// indent. Column table for the row lines: indent 0-1, name 2-17,
// gutter 18-19, status 20-22, gutter 23-24, cell 25-62 — every row
// line is 63 columns.
const GOLDEN = [
  "bench: wasmtime 40.0.0, wat2wasm 1.0.39, iters=2, exec-iters=2, cap=30000ms, chain-cap=600000ms",
  "== leg: compile (boot build, median of 2)",
  "  001_hello         OK                                  12.3 ms",
  "  015_panic_oob     OK                                   9.8 ms",
  "  t06_params_widen  DNF                            > 30000.0 ms",
  "  t07_starved       DNF  stage starved by a dead upstream stage",
  "  t08_x             ERR                                  exit 1",
  "== leg: chain (mirror build -> mirror run -> child assemble, median of 1) -- skipped: pass --chain (off until T3.1 leg 4, the self chain, is green)",
].join("\n");

function fixture(): BenchReport {
  return {
    banner:
      "bench: wasmtime 40.0.0, wat2wasm 1.0.39, iters=2, exec-iters=2, " +
      "cap=30000ms, chain-cap=600000ms",
    legs: [compileLeg, skippedChain],
  };
}

test("report: the rendered round matches the golden byte for byte", () => {
  assert.equal(renderReport(fixture()), GOLDEN);
});

test("report: fixed width — every row line of the golden leg is 63 columns", () => {
  const lines = renderReport(fixture()).split("\n");
  const rowLines = lines.slice(2, 7);
  assert.deepEqual(
    rowLines.map((l) => l.length),
    [63, 63, 63, 63, 63],
  );
});

test("report: measured cells carry exactly one decimal, never scientific", () => {
  const wide: LegReport = {
    title: "compile (boot build, median of 1)",
    rows: [ok("001_hello", 123456.75)],
    skipped: false,
    skipReason: "",
  };
  const out = renderReport({
    banner: "bench",
    legs: [wide],
  });
  assert.match(out, /123456\.8 ms/);
  assert.doesNotMatch(out, /e\+|E\+/);
});

test("report: no timestamps anywhere in the output", () => {
  const out = renderReport(fixture());
  assert.doesNotMatch(out, /\d{4}-\d{2}-\d{2}/); // ISO date
  assert.doesNotMatch(out, /\d{2}:\d{2}:\d{2}/); // clock
  assert.doesNotMatch(out, /\b(20\d{2})\b/); // a year
});

test("report: no absolute paths in the output", () => {
  const out = renderReport(fixture());
  for (const banned of ["/Users/", "/home/", "/tmp/", "/var/", "/private/"]) {
    assert.ok(!out.includes(banned), `output must not contain ${banned}`);
  }
  assert.ok(!out.includes(process.cwd()), "output must not contain the cwd");
});

test("report: pure ASCII — byte-diffable across environments", () => {
  const out = renderReport(fixture());
  for (const ch of out) {
    const code = ch.codePointAt(0) ?? 0;
    // the line feed rides every row boundary; everything else prints
    assert.ok(code === 10 || (code >= 32 && code <= 126), `non-ASCII char: ${ch}`);
  }
});

test("report: the renderer preserves row order — sorting is upstream's duty", () => {
  // The renderer must NOT re-sort: sorting is single-sourced in
  // corpus.listCorpus, and a silent second sort could mask an
  // upstream ordering bug.
  const unsorted: LegReport = {
    title: "compile (boot build, median of 1)",
    rows: [ok("002_arith", 2), ok("001_hello", 1)],
    skipped: false,
    skipReason: "",
  };
  const out = renderReport({ banner: "bench", legs: [unsorted] });
  const helloAt = out.indexOf("001_hello");
  const arithAt = out.indexOf("002_arith");
  // the input arrived [002, 001]; preservation prints 002 first
  assert.ok(arithAt > 0 && helloAt > arithAt);
});

test("report: a cap-hit DNF prints the governing cap, never the median", () => {
  const leg: LegReport = {
    title: "compile (boot build, median of 1)",
    rows: [ok("a", 1), dnfCap("b")],
    skipped: false,
    skipReason: "",
  };
  const lines = renderReport({ banner: "bench", legs: [leg] }).split("\n");
  const rowA = lines[2];
  const rowB = lines[3];
  assert.ok(rowA !== undefined && rowB !== undefined);
  assert.equal(rowA.length, rowB.length);
  assert.match(rowB, /DNF {2}> 30000\.0 ms$/);
});

test("report: a starved DNF prints the starvation sentence, never a cap it did not hit", () => {
  // The row never ran — no cap was exceeded — so "> cap ms" would
  // misattribute a hang. tools/bench/README.md promises this exact
  // sentence; the cell is byte-pinned here.
  const leg: LegReport = {
    title: "exec (wasmtime run, median of 1)",
    rows: [ok("a", 1), dnfStarved("b")],
    skipped: false,
    skipReason: "",
  };
  const out = renderReport({ banner: "bench", legs: [leg] });
  assert.match(out, /DNF {2}stage starved by a dead upstream stage$/m);
  assert.doesNotMatch(out, /> 30000\.0 ms/);
});

test("report: an empty round renders the banner and nothing else", () => {
  assert.equal(
    renderReport({ banner: "bench: empty", legs: [] }),
    "bench: empty",
  );
});
