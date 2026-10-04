// Pins: docs/ecosystem.md section 1 — "deterministic inputs (the
// corpus itself)". Discovery is name-sorted in plain code-unit order
// (readdir order is OS-defined; the sort makes it tree-defined), the
// filter is an honest refusal on zero matches, and the corpus's
// headers parse exactly as the corpus runner reads them — the `// set:`
// law of section 17 and the first `// exit:` header that grades every
// exec run. Listing runs against a throwaway directory under the OS
// temp dir — never against shared paths.

import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { byName, listCorpus, parseExitHeader, parseSetHeaders } from "../src/corpus.ts";

test("corpus: byName is plain code-unit order — never localeCompare", () => {
  const names = ["t01_x", "050_y", "n01_z", "001_a"];
  const sorted = [...names].sort(byName);
  assert.deepEqual(sorted, ["001_a", "050_y", "n01_z", "t01_x"]);
});

test("corpus: parseSetHeaders reads the // set: header law", () => {
  const text = [
    "// exit: 0",
    "// set: WIDTH=8",
    "// set: NAME=wide",
    "const X: i32 = 1;",
    "// set:RATE=0.5", // no space after "set:" — not a header
  ].join("\n");
  assert.deepEqual(parseSetHeaders(text), ["WIDTH=8", "NAME=wide"]);
});

test("corpus: parseSetHeaders returns empty without headers", () => {
  assert.deepEqual(parseSetHeaders("// exit: 0\nfn main() { }\n"), []);
});

test("corpus: parseExitHeader reads the // exit: header law — first header wins", () => {
  // The runner takes the first header (sed ... | head -1), so bench
  // parses the same one — corpus/045_shift_signed.rho carries two.
  const text = [
    "// exit: 42",
    "fn main() -> i32 { return 42; }",
    "// exit: 0", // a later repeat must not win
  ].join("\n");
  assert.equal(parseExitHeader(text), 42);
});

test("corpus: parseExitHeader defaults to 0 — no header, no half-match", () => {
  // Exactly the runner's `[ -z "$want_exit" ] && want_exit=0`: no
  // header designs exit 0. `// exit:0` (no space) is not a header,
  // and a non-integer value reads as absent (the divergence recorded
  // in tools/bench/README.md, Honest limits).
  assert.equal(parseExitHeader("fn main() { }\n"), 0);
  assert.equal(parseExitHeader("// exit:0\nfn main() { }\n"), 0);
  assert.equal(parseExitHeader("// exit: 42 is the answer\nfn main() {}\n"), 0);
});

test("corpus: listing is name-sorted and reads the set/exit headers", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-corpus-"));
  try {
    writeFileSync(
      join(dir, "002_b.rho"),
      "// exit: 7\n// set: WIDTH=2\nfn main() -> i32 { return 0; }\n",
    );
    writeFileSync(join(dir, "001_a.rho"), "// exit: 0\nfn main() {}\n");
    writeFileSync(join(dir, "notes.md"), "not a program\n"); // skipped
    mkdirSync(join(dir, "pk")); // package interiors are never roots
    writeFileSync(join(dir, "pk", "003_c.rho"), "fn main() {}\n");

    const listed = listCorpus(dir, "");
    if (typeof listed === "string") assert.fail(listed);
    assert.deepEqual(
      listed.map((p) => p.name),
      ["001_a", "002_b"], // readdir order is OS-defined; the output is not
    );
    assert.deepEqual(listed[1]?.sets, ["WIDTH=2"]);
    assert.equal(listed[0]?.exitCode, 0);
    assert.equal(listed[1]?.exitCode, 7); // the designed exec grading
    // relPath rides the spelled directory (join) — the CLI spawns it
    // against the repo root, never the cwd
    assert.equal(listed[0]?.relPath, join(dir, "001_a.rho"));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("corpus: the filter keeps matching names", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-corpus-"));
  try {
    writeFileSync(join(dir, "001_hello.rho"), "fn main() {}\n");
    writeFileSync(join(dir, "002_arith.rho"), "fn main() {}\n");
    const listed = listCorpus(dir, "hello");
    if (typeof listed === "string") assert.fail(listed);
    assert.deepEqual(
      listed.map((p) => p.name),
      ["001_hello"],
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("corpus: a filter matching nothing is an honest refusal string", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-corpus-"));
  try {
    writeFileSync(join(dir, "001_hello.rho"), "fn main() {}\n");
    const refused = listCorpus(dir, "nomatch");
    assert.equal(typeof refused, "string");
    assert.match(String(refused), /no corpus programs match filter 'nomatch'/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("corpus: a missing corpus directory is a refusal string, not a crash", () => {
  // The refusal string rides cli.ts's exit-2 refuse() path; an
  // unhandled readdirSync ENOENT would die with a stack trace and the
  // wrong exit code instead.
  const refused = listCorpus(join(tmpdir(), "rho-bench-nowhere"), "");
  assert.equal(typeof refused, "string");
  assert.match(String(refused), /cannot read corpus directory/);
});

test("corpus: a *.rho entry that is a directory is a refusal string, not a crash", () => {
  const dir = mkdtempSync(join(tmpdir(), "rho-bench-corpus-"));
  try {
    writeFileSync(join(dir, "001_a.rho"), "fn main() {}\n");
    mkdirSync(join(dir, "002_bad.rho")); // EISDIR on read
    const refused = listCorpus(dir, "");
    assert.equal(typeof refused, "string");
    assert.match(String(refused), /002_bad\.rho is not readable as a file/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
