// rho diagnostics: parse the compiler's stderr lines into structured
// diagnostics and turn them into LSP ranges.
//
// The compiler reports one diagnostic per line (rho/boot/src/util.c:
// flush_diags):
//
//   <file>:<line>:<col>: error: <message>
//
// Lines that do not match (ad-hoc notes, panics) are ignored here and
// surfaced by the caller as log output instead of bogus diagnostics.
// Entries whose file is not the checked document (the embedded prelude
// shows up as `<prelude>:...`) are dropped — they do not point into user
// code.

import type { Range, Diagnostic } from 'vscode-languageserver/node';
import { DiagnosticSeverity } from 'vscode-languageserver/node';

export interface RawDiag {
  file: string;
  /** 1-based line, as the compiler reports it. */
  line: number;
  /** 1-based column, as the compiler reports it. */
  col: number;
  message: string;
}

const DIAG_RE = /^(.*?):(\d+):(\d+): error: (.*)$/;

/** Parse every `file:line:col: error: message` line in compiler stderr. */
export function parseCompilerDiags(stderr: string): RawDiag[] {
  const out: RawDiag[] = [];
  for (const line of stderr.split('\n')) {
    const m = DIAG_RE.exec(line.trimEnd());
    if (!m) continue;
    out.push({
      file: m[1],
      line: parseInt(m[2], 10),
      col: parseInt(m[3], 10),
      message: m[4],
    });
  }
  return out;
}

/** Zero-width range guard: LSP forbids end < start. */
function safeRange(startLine: number, startCol: number, endLine: number, endCol: number): Range {
  if (endLine < startLine || (endLine === startLine && endCol < startCol)) {
    return { start: { line: startLine, character: startCol }, end: { line: startLine, character: startCol } };
  }
  return {
    start: { line: startLine, character: startCol },
    end: { line: endLine, character: endCol },
  };
}

/** The extent of the token at a 0-based (line, character) point: an
 *  identifier spans its characters, anything else is one character (or a
 *  zero-width range at end of line). */
export function tokenRangeAt(lines: string[], line0: number, char0: number): Range {
  const text = line0 >= 0 && line0 < lines.length ? lines[line0] : '';
  if (!text) {
    return safeRange(line0, char0, line0, char0);
  }
  const char = Math.min(Math.max(char0, 0), Math.max(text.length - 1, 0));
  const isIdentChar = (c: string) => /[A-Za-z0-9_]/.test(c);
  if (char < text.length && isIdentChar(text[char])) {
    let start = char;
    let end = char;
    while (start > 0 && isIdentChar(text[start - 1])) start--;
    while (end < text.length - 1 && isIdentChar(text[end + 1])) end++;
    return safeRange(line0, start, line0, end + 1);
  }
  return safeRange(line0, char, line0, char + 1);
}

/** Map compiler diags onto one document (1-based point -> LSP Diagnostic).
 *  `belongsTo` decides which compiler-reported files map onto the document
 *  (entries for the embedded prelude etc. are filtered out). */
export function toLspDiags(
  raw: RawDiag[],
  docLines: string[],
  belongsTo: (file: string) => boolean,
): Diagnostic[] {
  const out: Diagnostic[] = [];
  for (const d of raw) {
    if (!belongsTo(d.file)) continue;
    const line0 = d.line - 1;
    const char0 = d.col - 1;
    out.push({
      severity: DiagnosticSeverity.Error,
      range: tokenRangeAt(docLines, line0, char0),
      message: d.message,
      source: 'rho',
    });
  }
  return out;
}

/** Split source text into lines (without terminators) — shared helper so
 *  diagnostics and the symbol scanner agree on coordinates. */
export function splitLines(text: string): string[] {
  return text.split(/\r\n|\r|\n/);
}

/** Bidirectional map between LSP (line, character) positions and 0-based
 *  string offsets, with the document text kept alongside. */
export class LineIndex {
  readonly text: string;
  /** Offset of the first character of each line. */
  private readonly starts: number[] = [0];

  constructor(text: string) {
    this.text = text;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === '\n') this.starts.push(i + 1);
      else if (text[i] === '\r' && text[i + 1] !== '\n') this.starts.push(i + 1);
    }
  }

  get lineCount(): number {
    return this.starts.length;
  }

  /** The text of one line, terminators stripped. */
  lineText(line: number): string {
    if (line < 0 || line >= this.starts.length) return '';
    const start = this.starts[line];
    let end = line + 1 < this.starts.length ? this.starts[line + 1] : this.text.length;
    if (end > start && this.text[end - 1] === '\n') end--;
    if (end > start && this.text[end - 1] === '\r') end--;
    return this.text.slice(start, end);
  }

  /** Offset for a 0-based (line, character) position, clamped to the doc. */
  offsetAt(line: number, character: number): number {
    if (line < 0) return 0;
    if (line >= this.starts.length) return this.text.length;
    const start = this.starts[line];
    const lineEnd = line + 1 < this.starts.length ? this.starts[line + 1] : this.text.length;
    // character offsets exclude the terminator; clamp inside the line
    const contentEnd = lineEnd - start > 0 && this.text[lineEnd - 1] === '\n'
      ? lineEnd - 1 - (lineEnd - 2 >= start && this.text[lineEnd - 2] === '\r' ? 1 : 0)
      : lineEnd;
    return Math.min(start + Math.max(character, 0), Math.max(contentEnd, start));
  }

  /** (line, character) for a 0-based offset, clamped to the doc. */
  positionAt(offset: number): { line: number; character: number } {
    const off = Math.min(Math.max(offset, 0), this.text.length);
    // binary search for the line
    let lo = 0;
    let hi = this.starts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this.starts[mid] <= off) lo = mid;
      else hi = mid - 1;
    }
    return { line: lo, character: off - this.starts[lo] };
  }
}
