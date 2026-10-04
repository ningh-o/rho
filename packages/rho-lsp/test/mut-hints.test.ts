// Pins: design §18's style-hint clause — "Declaring `mut` without ever
// writing through it is a hint (LSP), never an error" (TODO.md, the
// ratified law). Every hint here is severity 4 (LSP Hint) with the
// stable code "mut-never-written"; nothing in this scanner can ever
// produce an error. Also pins ecosystem.md §4's shape item: the mut
// view that is never written through narrows — as a hint only.

import { describe, expect, it } from "vitest";
import { MUT_HINT_CODE, MUT_HINT_SOURCE, mutHints } from "../src/mut-hints.js";

describe("declared mut, never written through -> hint", () => {
  it("hints on a let mut that is only read", () => {
    const text = 'fn main() -> i32 {\n  let mut total: i64 = 0;\n  printf("{}\\n", total);\n  return 0;\n}\n';
    const hints = mutHints(text);
    expect(hints).toHaveLength(1);
    const h = hints[0] as (typeof hints)[number];
    expect(h.severity).toBe(4); // LSP Hint — never an error, by the law
    expect(h.code).toBe(MUT_HINT_CODE);
    expect(h.source).toBe(MUT_HINT_SOURCE);
    expect(h.message).toContain("'total'");
    // the range covers exactly the `mut` keyword on its line
    const lines = text.split("\n");
    const start = h.range.start;
    expect(lines[start.line]?.slice(start.character, start.character + 3)).toBe("mut");
    expect(h.range.end).toEqual({ line: start.line, character: start.character + 3 });
  });

  it("hints on static mut, mut params, and mut self", () => {
    const text = [
      "static mut COUNTER: i32 = 0;",
      "fn tick(mut step: i32) {}",
      "struct T { v: i32, }",
      "impl T { fn bump(mut self) { } }",
      "fn main() -> i32 {",
      "  tick(1);",
      "  return 0;",
      "}",
      "",
    ].join("\n");
    const hints = mutHints(text);
    // `mut self` never written: hinted; statics/params only read: hinted
    expect(hints.map((h) => h.range.start.line).sort((a, b) => a - b)).toEqual([0, 1, 3]);
  });
});

describe("written through -> no hint", () => {
  const cases: [string, string][] = [
    ["rebind store", "  total = total + 1;"],
    ["compound store", "  total += 1;"],
    ["element store", "  total[0] = 1;"],
    ["field store", "  total.f = 1;"],
    ["nested element+field compound", "  total[0].f += 2;"],
  ];
  for (const [name, line] of cases) {
    it(`no hint after a ${name}`, () => {
      const text = `fn main() -> i64 {\n  let mut total: i64 = 0;\n${line}\n  return total;\n}\n`;
      expect(mutHints(text)).toEqual([]);
    });
  }

  it("no hint when the binding is passed with a mut argument marker (the callee may write)", () => {
    const text = 'fn main() -> i32 {\n  let mut s = make_watch();\n  consume(mut s);\n  return 0;\n}\n';
    expect(mutHints(text)).toEqual([]);
  });

  it("no hint when the binding's only use is a method call (a mut-receiver call may write, §18)", () => {
    const text = "fn main() -> i32 {\n  let mut r = make_rect();\n  r.scale(2);\n  return 0;\n}\n";
    expect(mutHints(text)).toEqual([]);
  });

  it("a plain (non-method) call does not suppress: the hint still fires", () => {
    const text = "fn main() -> i32 {\n  let mut q = make_q();\n  view(q);\n  return 0;\n}\n";
    expect(mutHints(text)).toHaveLength(1);
  });

  it("does not count the declaration initializer as a write", () => {
    const text = "fn main() -> i32 {\n  let mut x = 1;\n  return x;\n}\n";
    expect(mutHints(text)).toHaveLength(1);
  });

  it("hints the never-written half of the §18 rebind idiom only when nothing later writes", () => {
    // let mut p = p; with a later store through p: no hint
    const written = "fn f(p: *T) {\n  let mut p = p;\n  p.v = 1;\n}\n";
    expect(mutHints(written)).toEqual([]);
    // ...and a never-written rebind idiom is honestly hinted
    const idle = "fn f(p: *T) {\n  let mut p = p;\n  use_p(p);\n}\n";
    expect(mutHints(idle)).toHaveLength(1);
  });
});

describe("documented conservative suppressions", () => {
  it("a `mut x` spelling inside a comment suppresses (never the reverse)", () => {
    const text = "fn main() {\n  let mut x = 1;\n  // someday: mut x goes here\n}\n";
    expect(mutHints(text)).toEqual([]);
  });

  it("a `let mut` spelling inside a string literal registers no ghost declaration", () => {
    const text = 'fn main() -> i32 {\n  const s = "let mut ghost = 0;";\n  return 0;\n}\n';
    expect(mutHints(text)).toEqual([]);
  });

  it("KNOWN LIMITATION: a `let mut` spelling inside a triple-quoted string that itself contains a quote leaks a ghost hint", () => {
    // STRING_LIT models only the "…" form, so a triple-quoted verbatim
    // string containing a `"` (the very reason that form exists) blanks
    // as short pieces and its middle reaches the scan as code: the
    // `let mut ghost` below registers a ghost declaration and its hint
    // FIRES. Pinned AS-IS — the current behavior, stated honestly in
    // the source header; §18 bounds the damage (a hint, never an
    // error). Closing it means modeling triple-quote lexing in
    // mut-hints.ts.
    const text = 'fn main() -> i32 {\n  const s = """say "let mut ghost=1" hmm""";\n  return 0;\n}\n';
    const hints = mutHints(text);
    expect(hints).toHaveLength(1);
    expect(hints[0]?.message).toContain("'ghost'");
  });

  it("a `let mut` spelling in a trailing comment registers no ghost declaration", () => {
    const text = "fn main() -> i32 {\n  help(); // let mut ghost = 0;\n  return 0;\n}\n";
    expect(mutHints(text)).toEqual([]);
  });

  it("the aliasing limitation holds: a write through an alias still hints", () => {
    // `let y = x` copies the view; y.f = 1 writes the object x views,
    // but the scan does not track aliases — the hint fires. §18 says
    // hint, never error; this false positive is documented here, in
    // the source, and in the README.
    const text = "fn main() {\n  let mut c = new Cell { v: 0 };\n  let alias = c;\n  alias.v = 1;\n}\n";
    const hints = mutHints(text);
    expect(hints).toHaveLength(1);
    expect(hints[0]?.message).toContain("'c'");
  });
});

describe("determinism", () => {
  it("hints come out in declaration order, byte-identical across runs", () => {
    const text = "fn main() {\n  let mut a = 1;\n  let mut b = 2;\n  let mut c = 3;\n}\n";
    const first = mutHints(text);
    const second = mutHints(text);
    expect(first).toEqual(second);
    expect(first.map((h) => h.range.start.line)).toEqual([1, 2, 3]);
  });

  it("no declarations -> no hints", () => {
    expect(mutHints("fn main() {}\n")).toEqual([]);
  });
});
