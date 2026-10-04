// Pins: the diagnostics-alignment contract (ecosystem.md §4:
// "diagnostics byte-identical to `rho check`"). The parser must keep
// the compiler's messages VERBATIM and in the compiler's order; the
// summary line is bookkeeping, never a diagnostic; unrecognized lines
// are kept in `raw` (logged, never silently dropped, never invented
// into diagnostics). Faces pinned here match boot/util.c's diags_print
// (`file:line:col: error|note: msg`) and the self-hosted checker's
// positionless lines (`check: ...`, `parse: N unresolved form(s)`,
// `mods: unresolved module path(s)`).

import { describe, expect, it } from "vitest";
import { parseCompilerDiags } from "../src/diag-parser.js";

describe("positioned face (boot's rho check)", () => {
  it("keeps file, 1-based line and byte column, severity, and the verbatim message", () => {
    const stderr = "tests/check/neg_unknown_name.rho:2:29: error: unknown name 'nope'\n";
    const { diags, raw } = parseCompilerDiags(stderr);
    expect(raw).toEqual([]);
    expect(diags).toEqual([
      {
        kind: "positioned",
        message: "tests/check/neg_unknown_name.rho:2:29: error: unknown name 'nope'",
        file: "tests/check/neg_unknown_name.rho",
        line1: 2,
        byteCol1: 29,
        severity: "error",
      },
    ]);
  });

  it("distinguishes notes from errors", () => {
    const { diags } = parseCompilerDiags("m.rho:3:1: note: overload here\n");
    expect(diags[0]?.severity).toBe("note");
  });
});

describe("positionless faces (the self-hosted wasm face today)", () => {
  it("keeps check lines verbatim, in order", () => {
    const stderr = "check: unknown fn 'mystery'\ncheck: unknown name 'y'\ncheck: 2 error(s)\n";
    const { diags, raw } = parseCompilerDiags(stderr);
    expect(raw).toEqual([]);
    expect(diags.map((d) => d.message)).toEqual(["check: unknown fn 'mystery'", "check: unknown name 'y'"]);
    expect(diags.every((d) => d.kind === "check")).toBe(true);
  });

  it("never turns the summary line into a diagnostic", () => {
    const { diags } = parseCompilerDiags("check: 3 error(s)\n");
    expect(diags).toEqual([]);
  });

  it("recognizes the parse-count and mods faces", () => {
    const { diags } = parseCompilerDiags("parse: 2 unresolved form(s)\nmods: unresolved module path(s)\n");
    expect(diags.map((d) => d.kind)).toEqual(["parse", "mods"]);
  });
});

describe("the never-invent law", () => {
  it("routes unrecognized lines to raw, preserving them verbatim", () => {
    const stderr = "rho: cannot open missing/mod.rho\nsomething odd happened\n";
    const { diags, raw } = parseCompilerDiags(stderr);
    expect(diags).toEqual([]);
    expect(raw).toEqual(["rho: cannot open missing/mod.rho", "something odd happened"]);
  });

  it("tolerates CRLF and blank lines; order is the compiler's order", () => {
    const stderr = "check: a\r\n\r\nm.rho:1:1: error: b\r\ncheck: 1 error(s)\r\n";
    const { diags, raw } = parseCompilerDiags(stderr);
    expect(diags.map((d) => d.message)).toEqual(["check: a", "m.rho:1:1: error: b"]);
    expect(diags.map((d) => d.kind)).toEqual(["check", "positioned"]);
    expect(raw).toEqual([]);
  });

  it("parses empty stderr to no diagnostics", () => {
    expect(parseCompilerDiags("")).toEqual({ diags: [], raw: [] });
  });
});
