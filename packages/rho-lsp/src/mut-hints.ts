// mut-hints.ts — the §18 style hint: "Declaring `mut` without ever
// writing through it is a hint (LSP), never an error." The compiler
// exports no such data today (its check face speaks refusal lines
// only), so the scan lives here, server-side, and is a LEXICAL
// heuristic with documented limits. It can only ever DEMOTE: a hint is
// severity 4 (LSP Hint), carries code "mut-never-written", and is
// behind a capability toggle (initializationOptions.mutHints, default
// on). It is never an error and never affects any exit path.
//
// What counts as a write-through of binding `x` (suppresses the hint):
//   x = / x op=            stores through the view (compound forms too)
//   x[i] = / x.f = / x[i].f op=   element and field stores at any depth
//   x.m(...)               ANY method call on x: §18 makes a
//                          mut-receiver call a write, and receiver
//                          mut-ness is not visible lexically — every
//                          method call counts (conservative)
//   f(..., mut x, ...)     an argument marker: §18 grants the callee a
//                          mut view — the callee may write through it,
//                          so the hint must NOT fire (conservative)
//
// Known false positives (a hint MAY fire although a write exists —
// §18 says hint, never error, so these are acceptable and documented):
//   aliasing: `let y = x; y.f = 1;` writes the object x views; the
//     scan does not track aliases.
//   writes from inside closures over captured handles.
// And the mirror-image false negatives (a write MAY hide — the same
// conservatism, other direction):
//   x.m(...) suppresses even when m's receiver is read-only (the scan
//     cannot see receiver mut-ness), so a read-only method chain on a
//     mut binding can hide its hint.
//   `mut x` / `x = ...` spellings inside "…" string literals or line
//     comments never create declarations or writes — they are stripped
//     before both scans — so they only ever SUPPRESS or hide. (The
//     triple-quoted verbatim form is the one gap — see below; a leak
//     there reaches the scans as code, ghost declarations and phantom
//     writes alike.)
// Declaration initializers are stripped before the store scan, so
// `let mut x = 1` does not count as a write of x — but the §18 rebind
// idiom `let mut p = p;` followed by stores is handled: only the later
// stores count, and a never-written rebind idiom is honestly hinted.
//
// Declarations scanned: `let mut x`, `static mut X`, parameter
// `mut x: T`, and `mut self`. String literals and `//` line comments
// are blanked/cut before the DECLARATION scan: a `let mut x` spelled
// inside either must not register a ghost declaration. The blanking
// models ONLY the "…" literal (escaped `\"` honored, per boot/lex.c);
// the lexer's other form — the triple-quoted verbatim string — is not
// modeled, and one that itself contains a `"` (the very reason that
// form exists) splits into short "…" blanks and leaks its middle: a
// `let mut x` spelled there registers a ghost declaration whose hint
// fires. KNOWN LIMITATION, pinned as such in test/mut-hints.test.ts;
// §18 bounds the damage — a hint, never an error. Single-file scan:
// closure captures, other-module writes, and writes through imported
// names are out of scope.
//
// Pins: test/mut-hints.test.ts.

import type { Diagnostic } from "./protocol.js";
import { jsOffsetToLsp, lineStarts } from "./positions.js";

export const MUT_HINT_CODE = "mut-never-written";
export const MUT_HINT_SOURCE = "rho-lsp";

interface MutDecl {
  name: string;
  /** Document offset of the `mut` keyword itself. */
  mutOffset: number;
}

const LET_MUT = /\blet\s+(mut)\s+([A-Za-z_][A-Za-z0-9_]*)/;
const STATIC_MUT = /\bstatic\s+(mut)\s+([A-Za-z_][A-Za-z0-9_]*)/;
const PARAM_MUT = /(?:\(|,)\s*(mut)\s+(?!self\b)([a-z_][A-Za-z0-9_]*)\s*:/;
const MUT_SELF = /\b(mut)\s+(self)\b/;

// a store statement: NAME at a statement/argument boundary, optional
// [i]/.f steps, then = or a compound op (== is excluded by [^=])
const STORE = /(?:^|[\s;{}(,])([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]|\.[A-Za-z_][A-Za-z0-9_]*)*\s*(?:=[^=]|[+\-*/%&|^]=|<<=|>>=)/g;
// any method call on NAME (at a statement/argument boundary, at least
// one [i]/.m step, then an open paren): a mut-receiver call is a write
// per §18, and receiver mut-ness is not visible lexically, so every
// method call counts as a possible write-through (conservative)
const METHOD_CALL = /(?:^|[\s;{}(,])([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]|\.[A-Za-z_][A-Za-z0-9_]*)+\s*\(/g;
// declaration prefixes are stripped before the store scan so a
// declaration's initializer never counts as a write-through
const DECL_PREFIX = /\b(?:let|static)\s+(?:mut\s+)?[A-Za-z_][A-Za-z0-9_]*\s*/g;
const ANY_MUT = /\bmut\s+([A-Za-z_][A-Za-z0-9_]*)/g;
// the "…" string literal form ONLY (escaped quotes honored, per
// boot/lex.c); the triple-quoted verbatim form is NOT modeled — see
// the header's known limitation. Blanked length-preserving so
// declaration offsets stay valid
const STRING_LIT = /"(?:[^"\\]|\\.)*"/g;

/** Blank "…" string literals (length-preserving, escaped `\"`
 * honored) and cut at the first `//` line comment, so neither can
 * ghost a declaration or a write. The triple-quoted verbatim form is
 * NOT modeled: one containing a `"` leaks its middle to the scans
 * (the header's known limitation, pinned in test/mut-hints.test.ts).
 * Call before any declaration/store matching. */
function stripLiteralsAndComments(line: string): string {
  const noStrings = line.replace(STRING_LIT, (m) => " ".repeat(m.length));
  const comment = noStrings.indexOf("//");
  return comment >= 0 ? noStrings.slice(0, comment) : noStrings;
}

function declOf(trimmedLine: string): { name: string; mutIndex: number } | undefined {
  const line = stripLiteralsAndComments(trimmedLine);
  const m = LET_MUT.exec(line) ?? STATIC_MUT.exec(line);
  if (m) {
    const kw = m[1] as string;
    const name = m[2] as string;
    return { name, mutIndex: (m.index ?? 0) + (m[0].indexOf(kw)) };
  }
  const p = PARAM_MUT.exec(line);
  if (p) {
    const kw = p[1] as string;
    const name = p[2] as string;
    return { name, mutIndex: (p.index ?? 0) + p[0].indexOf(kw) };
  }
  const self = MUT_SELF.exec(line);
  if (self) return { name: "self", mutIndex: self.index ?? 0 };
  return undefined;
}

/**
 * Scan one document for declared-but-never-written-through `mut`
 * bindings. Output order: declaration order (deterministic).
 */
export function mutHints(text: string): Diagnostic[] {
  const starts = lineStarts(text);
  const lines: { content: string; trimmed: string; trimLead: number; start: number }[] = [];
  for (let li = 0; li < starts.length; li++) {
    const s = starts[li] as number;
    const e = li + 1 < starts.length ? (starts[li + 1] as number) : text.length;
    const raw = text.slice(s, e);
    const content = raw.replace(/\n$/, "").replace(/\r$/, "");
    const trimmed = content.trim();
    lines.push({ content, trimmed, trimLead: content.length - content.trimStart().length, start: s });
  }

  // declarations, in order
  const decls: MutDecl[] = [];
  for (const l of lines) {
    if (l.trimmed === "" || l.trimmed.startsWith("//")) continue;
    const d = declOf(l.trimmed);
    if (!d) continue;
    decls.push({ name: d.name, mutOffset: l.start + l.trimLead + d.mutIndex });
  }
  if (decls.length === 0) return [];

  // write-throughs over the document (declaration initializers excluded;
  // string literals and line comments stripped first — they never
  // create a write)
  const written = new Set<string>();
  for (const l of lines) {
    if (l.trimmed === "" || l.trimmed.startsWith("//")) continue;
    const stripped = stripLiteralsAndComments(l.content).replace(DECL_PREFIX, " ");
    for (const m of stripped.matchAll(STORE)) {
      const name = m[1];
      if (name) written.add(name);
    }
    for (const m of stripped.matchAll(METHOD_CALL)) {
      const name = m[1];
      if (name) written.add(name);
    }
  }

  // `mut NAME` occurrences that are not the declarations themselves:
  // argument markers granting a callee a mut view (conservative)
  const declOffsets = new Set(decls.map((d) => d.mutOffset));
  const markedElsewhere = new Map<string, boolean>(); // name -> seen a non-decl marker
  for (const m of text.matchAll(ANY_MUT)) {
    const name = m[1];
    if (!name) continue;
    const off = m.index ?? -1;
    if (declOffsets.has(off)) continue;
    markedElsewhere.set(name, true);
  }

  const hints: Diagnostic[] = [];
  for (const d of decls) {
    if (written.has(d.name)) continue;
    if (markedElsewhere.has(d.name)) continue;
    const start = jsOffsetToLsp(text, starts, d.mutOffset);
    hints.push({
      range: {
        start,
        end: { line: start.line, character: start.character + 3 }, // the keyword "mut"
      },
      severity: 4,
      code: MUT_HINT_CODE,
      source: MUT_HINT_SOURCE,
      message: `'${d.name}' is declared mut but never written through (hint)`,
    });
  }
  return hints;
}
