// fixpoint.test.js — the canonical form is a fixpoint THROUGH THE
// PLUGIN PATH: format(format(x)) === format(x). Run over the whole
// pinned subset (starting from the corpus's own, often non-canonical,
// spelling), hand-mangled variants of the subset's grammar, and the
// bridge's edge spellings (very long lines, CRLF). Sources outside the
// retained subset are the refusal path's fixtures — those live in
// plugin.test.js.
import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import prettier from "prettier";
import plugin from "../src/index.js";
import { corpusSubset, fmtSelfFixtures, worktreeRoot } from "./subset.js";

const viaPrettier = (text) =>
  prettier.format(text, { parser: "rho", plugins: [plugin] });

for (const f of [...corpusSubset, ...fmtSelfFixtures]) {
  it(`fixpoint: ${f}`, async () => {
    const src = readFileSync(join(worktreeRoot, f), "utf8");
    const once = await viaPrettier(src);
    const twice = await viaPrettier(once);
    expect(twice).toBe(once);
  });
}

describe("fixpoint from mangled spellings", () => {
  const longLit = "ab".repeat(1024); // a 2 KiB single-line literal
  const cases = {
    "cramped spacing": 'fn main()->i32{let x=i+1;printf("{}",x);return 0;}',
    "wild whitespace": "\n\nfn   main ( ) -> i32 {\n\n\n  let y =   2 ;\n  return y ;\n}\n\n\n",
    "one line everything":
      "fn f(n:i64)->i64{if n<2{return n;}return f(n-1)+f(n-2);}fn main()->i32{printf(\"{}\\n\",f(10));return 0;}",
    "odd indentations":
      "fn main() -> i32 {\n      let mut i = 0;\n              while i < 3 {\n                                  i += 1;\n      }\n  return i;\n}",
    "string escapes intact":
      'fn main() -> i32 { let s: string = "a\\rb\\0c\\x41\\u{e9}"; printf("[{}] {}\n", s, len(s) as i64); return 0; }',
    "verbatim strings intact":
      'fn main() -> i32 { let v: string = """ab""cd"""; printf("[{}] {}\n", v, len(v) as i64); return 0; }',
    "very long single line": `fn main() -> i32 { printf("${longLit}\\n"); return 0; }`,
  };
  for (const [name, src] of Object.entries(cases)) {
    it(name, async () => {
      const once = await viaPrettier(src);
      const twice = await viaPrettier(once);
      expect(twice).toBe(once);
    });
  }
  it("very long single line keeps its literal verbatim", async () => {
    const once = await viaPrettier(cases["very long single line"]);
    expect(once).toContain(longLit);
  });
});

describe("fixpoint from CRLF spellings", () => {
  // CR is lexer whitespace (rho/lex.rho skips 32/9/13/10), and the
  // printer emits only LF — so a CRLF file canonicalizes to LF and the
  // canonical form must be a fixpoint like any other.
  const crlf = "fn main() -> i32 {\r\n  let x: i32 = 41;\r\n  return x + 1;\r\n}\r\n";
  it("CRLF input is fixpoint-stable", async () => {
    const once = await viaPrettier(crlf);
    const twice = await viaPrettier(once);
    expect(twice).toBe(once);
  });
  it("CRLF input canonicalizes to LF (no CR survives the lexer)", async () => {
    const once = await viaPrettier(crlf);
    expect(once).not.toContain("\r");
    expect(once).toContain("\n");
  });
});
