// diag-parser.ts — compiler stderr lines -> raw diagnostics. This is the
// string-level coupling between the shell and the checker's human output
// format (`boot/util.c` diags_print: `file:line:col: error|note: msg`),
// and it knows exactly three more faces:
//
//   positioned      `path:12:9: error: unknown name 'nope'`
//   check lines     `check: unknown fn 'mystery'`   (the self-hosted
//                   checker speaks today — positionless)
//   summary line    `check: 3 error(s)`             (never a diagnostic)
//   parse refusal   `parse: 2 unresolved form(s)`   (count only; the
//                   mirror's parse face exports no per-error detail yet)
//   mods refusal    `mods: unresolved module path(s)`
//
// Laws this module pins (test/diag-parser.test.ts):
// - messages are the compiler's VERBATIM line text, never rewritten;
//   diagnostics parity with `rho check` is auditable byte for byte;
// - order is the compiler's order (the determinism law extends to the
//   wire: same input -> identical published array);
// - unrecognized lines go to `raw`, never silently dropped, never
//   invented into diagnostics;
// - positionless check lines are ATTACHED to the document as file-level
//   diagnostics (range 0:0-0:0) rather than dropped: with the wasm face
//   positionless today, dropping would blank the editor where `rho
//   check` in a terminal still shows the error. The divergence from the
//   archive's temporary call (docs/language-service.md §4.1) is recorded
//   in WORKTREE-NOTES.md and flips back to drop-the-line the day the
//   checker reports positions.

export type RawSeverity = "error" | "note";

/** A diagnostic exactly as the compiler stated it. */
export interface RawDiag {
  kind: "positioned" | "check" | "parse" | "mods";
  /** The compiler's line verbatim except that trailing whitespace is
   * stripped (the newline and any trailing blanks — trimEnd()). */
  message: string;
  /** Positioned face only; undefined otherwise. */
  file?: string;
  line1?: number;
  byteCol1?: number;
  severity?: RawSeverity;
}

export interface ParsedDiags {
  diags: RawDiag[];
  /** Unrecognized lines: logged by the server, never published. */
  raw: string[];
}

const POSITIONED = /^(\S+?):(\d+):(\d+): (error|note): (.+)$/;
const CHECK_LINE = /^check: (.+)$/;
const CHECK_SUMMARY = /^check: \d+ error\(s\)$/;
const PARSE_REFUSAL = /^parse: (\d+) unresolved form\(s\)$/;
const MODS_REFUSAL = /^mods: unresolved module path\(s\)$/;

/** Parse the full stderr text of one compiler run, in order. */
export function parseCompilerDiags(stderr: string): ParsedDiags {
  const diags: RawDiag[] = [];
  const raw: string[] = [];
  const lines = stderr.split("\n");
  for (const line of lines) {
    const text = line.replace(/\r$/, "").trimEnd();
    if (text === "") continue;

    const positioned = POSITIONED.exec(text);
    if (positioned) {
      // the capture groups exist whenever the regex matched; the ??
      // fallbacks are statically unreachable and keep the optional
      // properties exact (never undefined-typed)
      const file = positioned[1] ?? "";
      const line1 = Number(positioned[2] ?? 0);
      const byteCol1 = Number(positioned[3] ?? 0);
      const severity: RawSeverity = positioned[4] === "error" ? "error" : "note";
      diags.push({ kind: "positioned", message: text, file, line1, byteCol1, severity });
      continue;
    }
    if (CHECK_SUMMARY.test(text)) continue; // the summary is bookkeeping, not a diagnostic
    const check = CHECK_LINE.exec(text);
    if (check) {
      diags.push({ kind: "check", message: text });
      continue;
    }
    const parse = PARSE_REFUSAL.exec(text);
    if (parse) {
      diags.push({ kind: "parse", message: text });
      continue;
    }
    if (MODS_REFUSAL.test(text)) {
      diags.push({ kind: "mods", message: text });
      continue;
    }
    raw.push(text);
  }
  return { diags, raw };
}
