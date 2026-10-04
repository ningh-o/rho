// Pins: ecosystem.md §4's diagnostics-acceptance face — a diagnostic is
// only useful where it lands, so the shell's byte-column -> UTF-16
// conversion is pinned on non-ASCII fixtures both directions (the
// archive branch's docs/language-service.md §3.3 rule, required
// reading; the byte accounting re-verified against this tree's
// boot/lex.c `advance()`). Also §8's law there: position fixtures must
// carry CJK, emoji, and combining marks.

import { describe, expect, it } from "vitest";
import { bytePosToLsp, endPosition, jsOffsetToLsp, lineStarts, lspPosToOffset } from "../src/positions.js";

describe("byte column -> LSP position (the §3.3 law)", () => {
  // the archive doc's §3.1 probe, reproduced: the error token `a` sits
  // at 1-based UTF-8 byte column 42 and 1-based UTF-16 column 38
  const probe = 'fn main() -> i32 {\n  let a: string = "中文"; let b: i32 = a;\n  return 0;\n}\n';

  it("lands a byte-column diagnostic exactly on ASCII-after-CJK", () => {
    const starts = lineStarts(probe);
    const pos = bytePosToLsp(probe, starts, 2, 42);
    expect(pos).toEqual({ line: 1, character: 37 });
    // and the text AT that position is the token
    const starts2 = lineStarts(probe);
    const offset = lspPosToOffset(probe, starts2, pos.line, pos.character);
    expect(probe[offset]).toBe("a");
  });

  it("counts CJK as 3 bytes but 1 UTF-16 unit", () => {
    const line = 'let s = "中文";';
    const starts = lineStarts(line + "\n");
    // the closing quote: bytes before = 9 + 3 + 3 = 15 -> col 16;
    // UTF-16 units before = 15 - 2*(3-1) = 11
    expect(bytePosToLsp(line + "\n", starts, 1, 16)).toEqual({ line: 0, character: 11 });
  });

  it("counts an emoji as 4 bytes but 2 UTF-16 units", () => {
    const line = 'let s = "😀";';
    const starts = lineStarts(line + "\n");
    // the closing quote: bytes before = 9 + 4 = 13 -> col 14
    expect(bytePosToLsp(line + "\n", starts, 1, 14)).toEqual({ line: 0, character: 11 });
  });

  it("counts a combining mark as 2 bytes but 1 UTF-16 unit, both directions", () => {
    // U+0301 COMBINING ACUTE ACCENT — the archive §8 law's third
    // mandatory fixture class. It occupies 2 UTF-8 bytes but 1 UTF-16
    // unit, and the walk must count it like any other code point,
    // never merging it into its base letter.
    const line = 'let s = "e\u0301";'; // e + U+0301, kept decomposed via the escape
    const starts = lineStarts(line + "\n");
    // the closing quote: bytes before = 9 + 1 + 2 = 12 -> col 13;
    // UTF-16 units before = 11 (same convention as the CJK/emoji pins)
    expect(bytePosToLsp(line + "\n", starts, 1, 13)).toEqual({ line: 0, character: 11 });
    // the reverse face: the base letter, the mark, and the quote stay
    // three separate positions, each offset round-tripping
    for (const [offset, character] of [
      [9, 9],
      [10, 10],
      [11, 11],
    ] as const) {
      expect(jsOffsetToLsp(line + "\n", starts, offset)).toEqual({ line: 0, character });
      expect(lspPosToOffset(line + "\n", starts, 0, character)).toBe(offset);
    }
  });

  it("clamps a target byte landing INSIDE a multi-byte code point", () => {
    const line = 'let s = "😀";';
    const starts = lineStarts(line + "\n");
    // bytesBefore = 11 lands inside the emoji (bytes 10..13):
    // the position clamps to the emoji's start, never mid-pair
    expect(bytePosToLsp(line + "\n", starts, 1, 12)).toEqual({ line: 0, character: 9 });
  });

  it("clamps out-of-line columns at the line end and out-of-range lines at the last line", () => {
    const text = "ab\nβeta\n";
    const starts = lineStarts(text);
    expect(bytePosToLsp(text, starts, 1, 999)).toEqual({ line: 0, character: 2 });
    // fixtures carry trailing newlines; lineStarts pushes a start for
    // every \n, so the \n after "βeta" opens an EMPTY LAST LINE (LSP
    // line 2, the phantom line). An out-of-range line clamps there:
    expect(bytePosToLsp(text, starts, 99, 1)).toEqual({ line: 2, character: 0 });
    expect(bytePosToLsp(text, starts, 0, 1)).toEqual({ line: 0, character: 0 }); // underflow clamps to line 1
  });

  it("treats a tab as one byte and no expansion", () => {
    const text = "\tlet x = 1;\n";
    const starts = lineStarts(text);
    expect(bytePosToLsp(text, starts, 1, 2)).toEqual({ line: 0, character: 1 });
  });
});

describe("LSP position -> offset (the inverse face)", () => {
  it("round-trips through CJK and emoji", () => {
    const text = 'let a = "中文";\nlet b = "😀😀";\n';
    const starts = lineStarts(text);
    for (const [line0, character0] of [
      [0, 0],
      [0, 9],
      [0, 10],
      [0, 11],
      [1, 9],
      [1, 11],
      [1, 13],
    ] as const) {
      const offset = lspPosToOffset(text, starts, line0, character0);
      const back = jsOffsetToLsp(text, starts, offset);
      expect(back).toEqual({ line: line0, character: character0 });
    }
  });

  it("clamps a position inside a surrogate pair at the pair's start", () => {
    const line = 'let b = "😀";';
    const offset = lspPosToOffset(line + "\n", lineStarts(line + "\n"), 0, 10);
    // character 10 is the middle of the emoji pair: clamps to its start
    expect(offset).toBe(9);
    // string indexing yields ONE UTF-16 unit — the pair reads via slice
    expect((line + "\n").slice(offset, offset + 2)).toBe("😀");
  });
});

describe("line accounting", () => {
  it("only \\n starts a line; \\r is ordinary content the column sees", () => {
    const text = "let a = 1;\r\nlet b;\r";
    const starts = lineStarts(text);
    expect(starts).toEqual([0, 12]);
    // the CR of line 1 sits at byte col 11 -> UTF-16 character 10
    expect(bytePosToLsp(text, starts, 1, 11)).toEqual({ line: 0, character: 10 });
    expect(bytePosToLsp(text, starts, 2, 1)).toEqual({ line: 1, character: 0 });
  });

  it("endPosition serves whole-document TextEdits", () => {
    const text = "fn main() {\n  return 0;\n}\n";
    // fixtures carry trailing newlines; lineStarts pushes a start for
    // every \n, so the \n after "}" opens an EMPTY LAST LINE (LSP line
    // 3, the phantom line) and the whole-document range must end there
    // — ending on "}" instead would eat the file's trailing newline:
    expect(endPosition(text, lineStarts(text))).toEqual({ line: 3, character: 0 });
    expect(endPosition("x", lineStarts("x"))).toEqual({ line: 0, character: 1 });
  });
});
