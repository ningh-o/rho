// symbols.ts — a minimal, single-file declaration index that backs
// hover, go-to-definition, and completion. HONEST SCOPE, stated up
// front: the compiler exports no symbol or query surface today (the
// wasm module exports exactly memory and _start; the check face speaks
// only refusal lines), so these three services ride this lexical
// scanner — a heuristic, not semantics. It is deliberately far smaller
// than the archive's 523-line front end (required reading, never
// copied): no scope simulation, no shadowing model, no type inference.
// When the compiler grows a query API, this file retires wholesale.
//
// What it indexes (line-shaped, no nesting analysis):
//   fn name(...) -> T        functions
//   fn Type.name(...)        methods
//   struct Name / enum Name / trait Name
//   const NAME[: T] / static [mut] NAME[: T]
//   use a.b.c;               (recorded for completion of nothing —
//                             path completion is out of scope)
//   let [mut] name = ...     body-local bindings, nearest-above wins
//
// Known blind spots (all yield "nothing", never a wrong answer):
// struct fields, match-pattern binders, closure params, or-pattern
// binders. Hovering such a name simply answers null.
//
// Pins: test/symbols.test.ts.

import type { CompletionItem, CompletionKind } from "./protocol.js";
import { lspPosToOffset, lineStarts } from "./positions.js";

export type SymbolKind =
  | "fn"
  | "method"
  | "struct"
  | "enum"
  | "trait"
  | "const"
  | "static"
  | "use"
  | "let";

export interface Symbol {
  kind: SymbolKind;
  name: string;
  /** 0-based line. */
  line: number;
  /** The whole declaration line, trimmed (the hover text). */
  lineText: string;
  /** JS string offset of the name inside the document. */
  nameOffset: number;
}

/**
 * The keyword set, pinned to boot/lex.c's k_keywords table (read
 * 2026-09-27): the grammar's full spelling list, alphabetical — with
 * "Self" where k_keywords itself carries it (between "use" and
 * "while"; the C table grew it there in T3.12's K_SELF work).
 */
export const KEYWORDS: readonly string[] = [
  "as", "break", "const", "continue", "defer", "dyn", "else", "enum",
  "extern", "false", "fn", "for", "if", "impl", "let", "loop", "match",
  "mut", "new", "null", "pub", "return", "static", "struct", "test",
  "trait", "true", "use", "Self", "while",
  // the primitive type spellings share the keyword table in boot/lex.c
  "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "usize", "f32",
  "f64", "bool", "string",
];

/**
 * The prelude's public surface, pinned to boot/prelude.rho as read
 * 2026-09-27: the Show trait, Option/Result and their variants, and
 * the mechanism functions (§14: exactly the mechanism-required set).
 * Primitive to_str methods travel with their types (import-scoped
 * visibility) and are not listed here.
 */
export const PRELUDE: readonly { label: string; kind: CompletionKind }[] = [
  { label: "Show", kind: 8 },
  { label: "Option", kind: 13 },
  { label: "Result", kind: 13 },
  { label: "Option.Some", kind: 20 },
  { label: "Option.None", kind: 20 },
  { label: "Result.Ok", kind: 20 },
  { label: "Result.Err", kind: 20 },
  { label: "printf", kind: 3 },
  { label: "eprintf", kind: 3 },
  { label: "format", kind: 3 },
  { label: "len", kind: 3 },
  { label: "make", kind: 3 },
  { label: "panic", kind: 3 },
  { label: "assert", kind: 3 },
  { label: "assert_eq", kind: 3 },
  { label: "exit", kind: 3 },
];

const DECL_PATTERNS: { re: RegExp; kind: SymbolKind }[] = [
  { re: /^(?:pub\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/, kind: "fn" },
  // fn Type.name( — the method face; matched after bare fn fails on the dot
  { re: /^(?:pub\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\(/, kind: "method" },
  { re: /^(?:pub\s+)?struct\s+([A-Za-z_][A-Za-z0-9_]*)/, kind: "struct" },
  { re: /^(?:pub\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*)/, kind: "enum" },
  { re: /^(?:pub\s+)?trait\s+([A-Za-z_][A-Za-z0-9_]*)/, kind: "trait" },
  { re: /^(?:pub\s+)?const\s+([A-Za-z_][A-Za-z0-9_]*)/, kind: "const" },
  { re: /^(?:pub\s+)?static\s+(?:mut\s+)?([A-Za-z_][A-Za-z0-9_]*)/, kind: "static" },
  { re: /^(?:pub\s+)?use\s+[A-Za-z_{][^;]*;/, kind: "use" },
];

// lets ride a dedicated global scan: a let hides after any statement
// boundary — its own line, or `;`/`{` on a compound line (`{ let v = 1;
// let w = v; }`) — and a fn line's first match must not swallow the
// lets after it. Every match indexes; same-name shadows are resolved
// by the nearest-above law at query time.
const LET_BOUNDARY = /(?:^|[;{]\s*)let\s+(?:mut\s+)?([a-z_][A-Za-z0-9_]*)/g;

export function indexSymbols(text: string): Symbol[] {
  const symbols: Symbol[] = [];
  const starts = lineStarts(text);
  for (let li = 0; li < starts.length; li++) {
    const start = starts[li] as number;
    const end = li + 1 < starts.length ? (starts[li + 1] as number) : text.length;
    const rawLine = text.slice(start, end);
    const lineText = rawLine.replace(/\n$/, "").trim();
    if (lineText === "" || lineText.startsWith("//")) continue;
    const trimLead = rawLine.length - rawLine.trimStart().length;
    for (const { re, kind } of DECL_PATTERNS) {
      const m = re.exec(lineText);
      if (!m) continue;
      let name: string | undefined;
      if (kind === "method") {
        // group 2 is the method name; group 1 the receiver type
        name = m[2];
      } else if (kind === "use") {
        name = lineText;
      } else {
        name = m[1];
      }
      if (name === undefined) break;
      // offset of the name inside the document: match index within the
      // trimmed line + the trimmed line's offset within the raw line
      const nameIdxInLine = kind === "use" ? 0 : (m.index ?? 0) + (m[0].indexOf(name));
      symbols.push({
        kind,
        name,
        line: li,
        lineText,
        nameOffset: start + trimLead + nameIdxInLine,
      });
      break; // one declaration per line for the head forms; multi-line
      //         decls are a blind spot
    }
    LET_BOUNDARY.lastIndex = 0;
    for (let lm = LET_BOUNDARY.exec(lineText); lm !== null; lm = LET_BOUNDARY.exec(lineText)) {
      const name = lm[1];
      if (name === undefined) break;
      symbols.push({
        kind: "let",
        name,
        line: li,
        lineText,
        nameOffset: start + trimLead + (lm.index ?? 0) + (lm[0].indexOf(name)),
      });
    }
  }
  return symbols;
}

export interface WordAt {
  word: string;
  start: number; // JS string offset
  end: number;
}

/** The identifier under a JS string offset (hover/definition target). */
export function wordAt(text: string, offset: number): WordAt | undefined {
  const isIdChar = (c: number) =>
    (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c === 0x5f;
  let start = offset;
  let end = offset;
  while (start > 0 && isIdChar(text.charCodeAt(start - 1))) start--;
  while (end < text.length && isIdChar(text.charCodeAt(end))) end++;
  if (start === end) return undefined;
  return { word: text.slice(start, end), start, end };
}

/** Hover: the declaration line for the word at an LSP position, if indexed. */
export function hoverAt(text: string, line0: number, character0: number): string | undefined {
  const starts = lineStarts(text);
  const offset = lspPosToOffset(text, starts, line0, character0);
  const word = wordAt(text, offset);
  if (!word) return undefined;
  const symbols = indexSymbols(text);
  // nearest declaration of that name at or above the position; a
  // body-local let shadows nothing formally here — nearest-above is
  // the documented heuristic
  let best: Symbol | undefined;
  for (const s of symbols) {
    if (s.name !== word.word) continue;
    if (s.nameOffset <= offset && (best === undefined || s.nameOffset > best.nameOffset)) best = s;
  }
  if (best === undefined) return undefined;
  return "```rho\n" + best.lineText + "\n```";
}

/** Definition: the document range of the declaration for a position. */
export function definitionAt(text: string, line0: number, character0: number): { start: number; end: number; line: number } | undefined {
  const starts = lineStarts(text);
  const offset = lspPosToOffset(text, starts, line0, character0);
  const word = wordAt(text, offset);
  if (!word) return undefined;
  const symbols = indexSymbols(text);
  let best: Symbol | undefined;
  for (const s of symbols) {
    if (s.name !== word.word || s.kind === "use") continue;
    if (s.nameOffset <= offset && (best === undefined || s.nameOffset > best.nameOffset)) best = s;
  }
  if (best === undefined) return undefined;
  const declStart = best.nameOffset;
  return { start: declStart, end: declStart + word.word.length, line: best.line };
}

/** Completion: keywords + declarations visible above the cursor + the prelude. */
export function completionsAt(text: string, line0: number): CompletionItem[] {
  const symbols = indexSymbols(text);
  const starts = lineStarts(text);
  const cursorLineStart = starts[Math.min(line0, starts.length - 1)] as number;
  const kindOf = (s: Symbol): CompletionKind => {
    switch (s.kind) {
      case "fn":
        return 3;
      case "method":
        return 2;
      case "struct":
        return 22;
      case "enum":
        return 13;
      case "trait":
        return 8;
      case "const":
        return 21;
      case "static":
      case "let":
        return 6;
      case "use":
        return 9;
    }
  };
  const items: CompletionItem[] = [];
  const seen = new Set<string>();
  const push = (label: string, kind: CompletionKind, detail?: string) => {
    if (seen.has(label)) return;
    seen.add(label);
    items.push(detail === undefined ? { label, kind } : { label, kind, detail });
  };
  for (const s of symbols) {
    if (s.kind === "use") continue;
    if (s.kind === "let" && s.nameOffset >= cursorLineStart) continue;
    push(s.name, kindOf(s), s.lineText);
  }
  for (const p of PRELUDE) push(p.label, p.kind, "prelude");
  for (const k of KEYWORDS) push(k, 14);
  items.sort((a, b) => (a.label < b.label ? -1 : a.label > b.label ? 1 : 0));
  return items;
}
