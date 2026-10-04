// positions.ts — the one place where the compiler's position convention
// and the wire convention meet. The compiler speaks 1-based lines and
// 1-based BYTE columns (boot/lex.c: `advance()` bumps the column by one
// per byte and resets it at \n; \r is an ordinary byte that counts; a
// tab counts as one byte). LSP positions are 0-based lines with 0-based
// offsets in UTF-16 code units. The conversion lives here and nowhere
// else — conversion, never heuristics: the compiler reports only start
// points (no end positions anywhere in the AST), so diagnostics get
// zero-width ranges; inventing token extents is a shell-side semantic
// guess and is not done.
//
// Reference law: the archive branch's docs/language-service.md §3
// (required reading, ecosystem.md §4) — read, never copied; the byte
// accounting was re-verified against the current tree's boot/lex.c.
//
// Pins: test/positions.test.ts — CJK, emoji (surrogate pairs), combining
// marks, clamping, CRLF, and the round-trip both directions.

/** UTF-8 byte length of one code point. */
function utf8Len(cp: number): number {
  if (cp < 0x80) return 1;
  if (cp < 0x800) return 2;
  if (cp < 0x10000) return 3;
  return 4;
}

/** UTF-16 code-unit length of one code point. */
function utf16Len(cp: number): number {
  return cp >= 0x10000 ? 2 : 1;
}

/**
 * Index of line start offsets (JS string offsets) for a document.
 * Only \n starts a new line; \r is ordinary content (the lexer counts
 * it toward the column, so the conversion must see it too).
 */
export function lineStarts(text: string): number[] {
  const starts = [0];
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) === 0x0a) starts.push(i + 1);
  }
  return starts;
}

/**
 * Convert the compiler's 1-based line + 1-based byte column into an LSP
 * position (0-based line, 0-based UTF-16 character) against the exact
 * text snapshot the compiler checked. Clamps out-of-range lines and
 * columns at the line end — a skewed diagnostic lands at the line's
 * last character, never past it, never inverted.
 */
export function bytePosToLsp(text: string, starts: number[], line1: number, byteCol1: number): { line: number; character: number } {
  const lineIdx = Math.min(Math.max(line1, 1), starts.length) - 1;
  const start = starts[lineIdx] as number;
  const end = lineIdx + 1 < starts.length ? (starts[lineIdx + 1] as number) : text.length;
  // Slice off the trailing \n (and a preceding \r if the file is CRLF —
  // both are line terminators for the editor, but the \r counts toward
  // the compiler's column, so the walk below must include it).
  let lineText = text.slice(start, end);
  if (lineText.endsWith("\n")) lineText = lineText.slice(0, -1);

  const bytesBefore = Math.max(byteCol1 - 1, 0);
  let consumed = 0;
  let units = 0;
  let i = 0;
  while (i < lineText.length) {
    const cp = lineText.codePointAt(i) as number;
    const bLen = utf8Len(cp);
    if (consumed + bLen > bytesBefore) break; // target byte lands inside/at this code point
    consumed += bLen;
    units += utf16Len(cp);
    i += cp >= 0x10000 ? 2 : 1;
  }
  return { line: lineIdx, character: units };
}

/**
 * Convert an LSP position into a JS string offset in the document.
 * Inverse of bytePosToLsp for the UTF-16 face; clamps like it.
 */
export function lspPosToOffset(text: string, starts: number[], line0: number, character0: number): number {
  const lineIdx = Math.min(Math.max(line0, 0), starts.length - 1);
  const start = starts[lineIdx] as number;
  const end = lineIdx + 1 < starts.length ? (starts[lineIdx + 1] as number) : text.length;
  let lineText = text.slice(start, end);
  if (lineText.endsWith("\n")) lineText = lineText.slice(0, -1);

  let units = 0;
  let i = 0;
  while (i < lineText.length && units < character0) {
    const cp = lineText.codePointAt(i) as number;
    const uLen = utf16Len(cp);
    if (units + uLen > character0) break; // position lands inside a surrogate pair
    units += uLen;
    i += uLen;
  }
  return start + i;
}

/** A zero-width range at a converted position (the compiler emits start points only). */
export function pointRange(pos: { line: number; character: number }): { start: { line: number; character: number }; end: { line: number; character: number } } {
  return { start: pos, end: pos };
}

/**
 * Convert a JS string offset into an LSP position (UTF-16 face).
 * Used for scanner answers (definition ranges, hint ranges).
 */
export function jsOffsetToLsp(text: string, starts: number[], offset: number): { line: number; character: number } {
  let lo = 0;
  let hi = starts.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if ((starts[mid] as number) <= offset) lo = mid;
    else hi = mid - 1;
  }
  const lineIdx = lo;
  const lineStart = starts[lineIdx] as number;
  const lineEnd = lineIdx + 1 < starts.length ? (starts[lineIdx + 1] as number) : text.length;
  let lineText = text.slice(lineStart, lineEnd);
  if (lineText.endsWith("\n")) lineText = lineText.slice(0, -1);
  return { line: lineIdx, character: utf16UnitsOf(lineText, offset - lineStart) };
}

/** UTF-16 code-unit offset for a JS offset within a line's text. */
function utf16UnitsOf(lineText: string, jsOffset: number): number {
  let units = 0;
  let i = 0;
  while (i < jsOffset && i < lineText.length) {
    const cp = lineText.codePointAt(i) as number;
    const width = cp >= 0x10000 ? 2 : 1;
    units += width;
    i += width;
  }
  return units;
}

/** The LSP end position of a whole document (whole-document TextEdits). */
export function endPosition(text: string, starts: number[]): { line: number; character: number } {
  const lastLine = starts.length - 1;
  const start = starts[lastLine] as number;
  let lineText = text.slice(start);
  if (lineText.endsWith("\n")) lineText = lineText.slice(0, -1);
  return { line: lastLine, character: utf16UnitsOf(lineText, lineText.length) };
}
