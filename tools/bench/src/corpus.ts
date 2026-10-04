// corpus.ts — the benchmark's inputs: the corpus itself, verbatim.
//
// Law: docs/ecosystem.md section 1 — "deterministic inputs (the corpus
// itself)". The harness invents no programs. Discovery mirrors the
// corpus runner (tests/run-corpus-repo.sh): the top-level corpus/*.rho
// files (package interiors are module inputs of their root programs,
// never roots themselves), name-sorted so the report order is
// tree-defined, not readdir-defined (readdir order is OS-defined).
//
// The sort is plain code-unit order — never localeCompare, whose order
// is locale-defined and would vary by machine. The acceptance law
// (byte-identical output on one tree + machine) starts here.

import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

export interface CorpusProgram {
  // Bare name, e.g. "001_hello" — the report's row key.
  readonly name: string;
  // Repo-relative path, e.g. "tests/suites/programs/001_hello.rho" —
  // derived from the LISTED directory (the corpus dissolved at the
  // freeze; the programs tier is its successor and bench follows it).
  // Spawned, never printed (the report carries names only).
  readonly relPath: string;
  // The `// set: name=value` header lines, in file order — the same
  // build-parameter law the corpus runner speaks, applied identically
  // so bench measures the same program the corpus graded.
  readonly sets: readonly string[];
  // The program's designed exit code — the first `// exit: <n>` header
  // (the corpus runner grades by it, tests/run-corpus-repo.sh), 0 when
  // the program carries no header. The exec leg grades a run by it.
  readonly exitCode: number;
}

export function listCorpus(
  corpusDir: string,
  filter: string,
): CorpusProgram[] | string {
  // Every failure is a refusal string for the CLI's exit-2 path (the
  // repo's usage-exit convention), never an unhandled throw: a missing
  // corpus directory, a *.rho entry that is a directory (EISDIR), a
  // permission hole. README.md's refusal list names both.
  let entries: string[];
  try {
    entries = readdirSync(corpusDir);
  } catch {
    return `cannot read corpus directory ${corpusDir} — bench expects ` +
      `the repo's tests/suites/programs (run from a full checkout)`;
  }
  // relPath rides the spelled directory verbatim (join) — the CLI
  // spawns absolute paths against the repo root, never the cwd
  const programs: CorpusProgram[] = [];
  for (const entry of entries) {
    if (!entry.endsWith(".rho")) continue;
    if (filter !== "" && !entry.includes(filter)) continue;
    let text: string;
    try {
      text = readFileSync(join(corpusDir, entry), "utf8");
    } catch {
      return `corpus entry ${entry} is not readable as a file ` +
        `(*.rho must be a file, not a directory)`;
    }
    programs.push({
      name: entry.slice(0, -".rho".length),
      relPath: join(corpusDir, entry),
      sets: parseSetHeaders(text),
      exitCode: parseExitHeader(text),
    });
  }
  if (programs.length === 0) {
    return filter === ""
      ? "no corpus programs found (tests/suites/programs is empty?)"
      : `no corpus programs match filter '${filter}'`;
  }
  programs.sort((a, b) => byName(a.name, b.name));
  return programs;
}

// Plain code-unit order; the comparator the whole harness sorts with.
export function byName(a: string, b: string): number {
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

// The `// set:` header law of section 17 (the corpus's own headers):
// lines starting exactly with "// set: ", value = the rest of the
// line, one --set argument per line.
export function parseSetHeaders(text: string): string[] {
  const sets: string[] = [];
  for (const line of text.split("\n")) {
    const m = /^\/\/ set: (.*)$/.exec(line);
    if (m !== null) {
      const v = m[1];
      if (v !== undefined) sets.push(v);
    }
  }
  return sets;
}

// The `// exit:` header law of the corpus runner (tests/run-corpus-repo.sh):
// the first line starting exactly with "// exit: " names the program's
// designed exit code — the runner takes it with `sed -n ... | head -1`
// and grades the run against it, so bench parses the same first header.
// A program without a header designs exit 0, as in the runner. The
// value must be a plain non-negative integer; a header that is not
// (none exists in the corpus today) reads as absent — recorded as a
// known divergence from the runner in tools/bench/README.md.
export function parseExitHeader(text: string): number {
  for (const line of text.split("\n")) {
    const m = /^\/\/ exit: (\d+)$/.exec(line);
    const v = m?.[1];
    if (v !== undefined) return Number.parseInt(v, 10);
  }
  return 0;
}
