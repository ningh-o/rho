// Pins: the honest-scope law for hover/definition/completion — the
// compiler exports no query surface today, so these ride the minimal
// declaration scanner (symbols.ts) and must answer "what the text
// itself declares", with every documented blind spot answering NOTHING
// rather than a guess (ecosystem.md §4's degrade-don't-lie, applied to
// features). The KEYWORDS list is pinned to boot/lex.c's k_keywords
// table and PRELUDE to boot/prelude.rho's public surface, both as read
// 2026-09-27.

import { describe, expect, it } from "vitest";
import { completionsAt, definitionAt, hoverAt, indexSymbols, KEYWORDS, PRELUDE, wordAt } from "../src/symbols.js";

const DOC = `use fmt.tools;
const LIMIT: i32 = 10;
static mut HITS: i32 = 0;
struct Pt { x: i32, y: i32, }
enum Shape { Dot, Box(Pt), }
trait Show { fn to_str(self) -> string, }
pub fn scale(v: *Pt, by: i32) -> *Pt { return v; }
fn Pt.to_str(self: *Pt) -> string { return "pt"; }
fn main() -> i32 {
  let mut total: i32 = 0;
  let total2 = LIMIT + total;
  return total2;
}
`;

describe("indexSymbols", () => {
  it("indexes module-level declarations and body lets", () => {
    const syms = indexSymbols(DOC);
    const kinds = syms.map((s) => s.kind);
    expect(kinds).toEqual(["use", "const", "static", "struct", "enum", "trait", "fn", "method", "fn", "let", "let"]);
    expect(syms[6]?.name).toBe("scale");
    expect(syms[7]?.name).toBe("to_str");
    expect(syms[9]?.name).toBe("total");
    expect(syms[10]?.name).toBe("total2");
  });

  it("records each declaration's full line as the hover text", () => {
    const syms = indexSymbols(DOC);
    expect(syms[6]?.lineText).toBe("pub fn scale(v: *Pt, by: i32) -> *Pt { return v; }");
    expect(syms[3]?.lineText).toBe("struct Pt { x: i32, y: i32, }");
  });

  it("skips comments and blank lines", () => {
    const syms = indexSymbols("// fn not_a_decl()\n\nfn real() {}\n");
    expect(syms.map((s) => s.name)).toEqual(["real"]);
  });
});

describe("hover", () => {
  it("shows the declaration line of a known name, in a rho fence", () => {
    const line = DOC.split("\n").findIndex((l) => l.includes("return total2;"));
    const hover = hoverAt(DOC, line, 9); // on `total2`
    expect(hover).toBe("```rho\nlet total2 = LIMIT + total;\n```");
  });

  it("shows the method declaration for a method name", () => {
    const hover = hoverAt(DOC, 7, 6); // on `to_str` inside `fn Pt.to_str(`
    expect(hover).toBe("```rho\nfn Pt.to_str(self: *Pt) -> string { return \"pt\"; }\n```");
  });

  it("answers NOTHING for a struct field (documented blind spot)", () => {
    const hover = hoverAt(DOC, 3, 12); // the `x` of `struct Pt { x: ...`
    expect(hover).toBeUndefined();
  });

  it("answers NOTHING off-identifier", () => {
    // line 8 is `fn main() -> i32 {`; character 10 is the `-` of `->`
    // (the character before it is a space, the character at it is not
    // an identifier char) — wordAt itself answers NO word here, which
    // is the mechanism the pin is about. (The old pin sat on the space
    // after `fn`, where wordAt still returns the word "fn" and hover
    // died later, on the symbol lookup — the wrong mechanism.)
    const line8 = DOC.split("\n")[8] as string;
    const offset = DOC.indexOf(line8) + line8.indexOf("->");
    expect(wordAt(DOC, offset)).toBeUndefined();
    expect(hoverAt(DOC, 8, 10)).toBeUndefined();
  });
});

describe("definition", () => {
  it("jumps to the declaration and the range covers the name", () => {
    const def = definitionAt(DOC, 10, 26); // on `total` in `let total2 = LIMIT + total;`
    expect(def).toBeDefined();
    if (def === undefined) return;
    expect(DOC.slice(def.start, def.end)).toBe("total");
    // the nearest let ABOVE the use wins (the declared heuristic)
    const defLine = DOC.split("\n")[def.line];
    expect(defLine).toContain("let mut total: i32 = 0;");
  });

  it("prefers the nearest-above binding over a later shadow of the same name", () => {
    const text = "fn a() { let v = 1; let w = v; }\nfn b() { let v = 2; }\n";
    const def = definitionAt(text, 0, 28); // `v` inside `let w = v`
    if (def === undefined) throw new Error("no definition");
    expect(text.slice(def.start, def.end)).toBe("v");
    expect(text.slice(0, def.start)).not.toContain("let v = 2");
  });

  it("answers NOTHING for an unknown name", () => {
    // the keyword `return` is not a declared name anywhere in the doc
    expect(definitionAt(DOC, 11, 4)).toBeUndefined();
  });
});

describe("completion", () => {
  it("merges declarations, the prelude, and keywords; sorted by label", () => {
    // the cursor sits on the line AFTER `let mut total` (line 10, the
    // usage in `let total2 = LIMIT + total`): completion offers lets
    // declared strictly ABOVE the cursor line only — the exclusion law
    // the next test pins
    const items = completionsAt(DOC, 10);
    const labels = items.map((i) => i.label);
    expect(labels).toEqual([...labels].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)));
    for (const expected of ["scale", "to_str", "Pt", "Shape", "Show", "LIMIT", "HITS", "total", "printf", "Option", "Result", "make", "let", "match"]) {
      expect(labels).toContain(expected);
    }
  });

  it("excludes body lets declared at or below the cursor line", () => {
    const items = completionsAt(DOC, 8); // before the lets
    expect(items.map((i) => i.label)).not.toContain("total");
    const itemsLater = completionsAt(DOC, 12); // after them
    expect(itemsLater.map((i) => i.label)).toContain("total");
  });

  it("deduplicates names shared between the document and the prelude", () => {
    const doc = "fn len() -> usize { return 0; }\n";
    const items = completionsAt(doc, 0);
    expect(items.filter((i) => i.label === "len")).toHaveLength(1);
    expect(items.find((i) => i.label === "len")?.detail).toContain("fn len()");
  });

  it("pins the keyword set to boot/lex.c's table", () => {
    // "Self" sits where k_keywords itself carries it (between "use"
    // and "while" — boot/lex.c:29, the T3.12 K_SELF addition)
    expect(KEYWORDS).toEqual([
      "as", "break", "const", "continue", "defer", "dyn", "else", "enum",
      "extern", "false", "fn", "for", "if", "impl", "let", "loop", "match",
      "mut", "new", "null", "pub", "return", "static", "struct", "test",
      "trait", "true", "use", "Self", "while",
      "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "usize", "f32",
      "f64", "bool", "string",
    ]);
  });

  it("pins the prelude surface to boot/prelude.rho's public items", () => {
    expect(PRELUDE.map((p) => p.label)).toEqual([
      "Show", "Option", "Result", "Option.Some", "Option.None",
      "Result.Ok", "Result.Err", "printf", "eprintf", "format", "len",
      "make", "panic", "assert", "assert_eq", "exit",
    ]);
  });
});
