// Pure, DOM-free core of the playground completion: word extraction,
// regex-grade document symbol scanning, and filtering/ranking. Kept free of
// the DOM so it can be exercised from Node directly.
//
// FAST COMPLETION, regex-grade: symbol extraction is a lexical
// approximation (no scoping, no types). The type-aware replacement is
// specified in docs/language-service.md; completion.js adapts to it via
// registerProvider, and this module stays the shared item/rank vocabulary.

// An identifier: letters/digits/underscore, not starting with a digit.
const IDENT = "[A-Za-z_][A-Za-z0-9_]*";

/**
 * A completion item. Every source agrees on this shape:
 *   label  — the text inserted on accept
 *   kind   — tag shown in the popup (kw/ty/fn/enum/trait/struct/let/const)
 *   detail — optional one-line signature
 *   doc    — optional one-sentence documentation
 *   source — "doc" (scanned from the buffer) or "static" (completion-data)
 *   rank   — tie-break within one match tier; lower wins first
 *
 * @typedef {{ label: string, kind: string, detail?: string, doc?: string,
 *             source?: string, rank?: number }} Item
 */

/**
 * Result of wordBefore: what the user has typed at the caret.
 *
 * @typedef {{ prefix: string, wordStart: number }} Word
 */

/**
 * The identifier immediately before the caret, or null when completion
 * should not fire: caret not after an identifier, mid-number, inside a
 * string or line comment, or right after a `.` (member completion needs
 * the language service — see docs/language-service.md).
 *
 * @param {string} text full editor text
 * @param {number} caret caret offset (ta.selectionStart)
 * @returns {Word | null}
 */
export function wordBefore(text, caret) {
  if (caret <= 0 || caret > text.length) return null;
  let start = caret;
  while (start > 0 && /[A-Za-z0-9_]/.test(text[start - 1])) start--;
  if (start === caret) return null; // caret not right after a word char
  if (!/[A-Za-z_]/.test(text[start])) return null; // digits-only run
  if (start > 0 && text[start - 1] === ".") return null; // member position
  // Heuristic context guard: skip inside string literals and // comments.
  const lineStart = text.lastIndexOf("\n", caret - 1) + 1;
  const before = text.slice(lineStart, start);
  let quotes = 0;
  for (const ch of before) if (ch === '"') quotes++;
  if (quotes % 2 === 1) return null; // odd quotes: inside a string
  if (before.includes("//")) return null; // a comment runs to end of line
  return { prefix: text.slice(start, caret), wordStart: start };
}

/**
 * Regex-grade scan of the visible document symbols. Module-level items
 * (fn / struct / enum / trait) are order-independent, so every declaration
 * counts; bindings (let / const / static) are offered only when declared
 * before the caret — a lexical approximation of visibility. Methods
 * (`fn Type.name`) are skipped: they complete after a dot, which the fast
 * layer does not cover.
 *
 * @param {string} text full editor text
 * @param {number} caret caret offset for binding visibility
 * @returns {Item[]}
 */
export function scanSymbols(text, caret) {
  /** @type {Item[]} */
  const items = [];
  const seen = new Set();

  const add = (name, kind, at) => {
    if (seen.has(name)) return;
    if ((kind === "let" || kind === "const") && at > caret) return; // not yet visible
    seen.add(name);
    items.push({ label: name, kind, source: "doc", rank: 0 });
  };

  // Top-level functions and methods: `fn name` / `pub fn name`; a dotted
  // name is a method and is dropped (see above).
  const fnRe = new RegExp("(?:^|\\n)\\s*(?:pub\\s+)?fn\\s+(" + IDENT + ")(\\." + IDENT + ")?", "g");
  for (const m of text.matchAll(fnRe)) {
    if (!m[2]) add(m[1], "fn", m.index);
  }

  // Types: struct / enum / trait declarations.
  const tyRe = new RegExp("\\b(struct|enum|trait)\\s+(" + IDENT + ")", "g");
  for (const m of text.matchAll(tyRe)) add(m[2], m[1], m.index);

  // Bindings: let / const / static, with optional `mut`.
  const bindRe = new RegExp("\\b(let|const|static)\\s+(?:mut\\s+)?(" + IDENT + ")", "g");
  for (const m of text.matchAll(bindRe)) {
    add(m[2], m[1] === "let" ? "let" : "const", m.index);
  }

  return items;
}

const MAX_ITEMS = 40;

/**
 * Filter + rank a merged item list against the typed prefix.
 * Tier 0: prefix match (case-insensitive). Tier 1: substring match.
 * The label typed exactly is dropped — accepting it would be a no-op.
 * Within a tier: rank (document locals first), then label order.
 *
 * @param {Item[]} items merged items from every source
 * @param {string} prefix the partial identifier being completed
 * @returns {Item[]} at most MAX_ITEMS, ranked
 */
export function filterItems(items, prefix) {
  const p = prefix.toLowerCase();
  const tier0 = [];
  const tier1 = [];
  for (const it of items) {
    const l = it.label.toLowerCase();
    if (l === p) continue;
    if (l.startsWith(p)) tier0.push(it);
    else if (l.includes(p)) tier1.push(it);
  }
  const byRank = (a, b) =>
    (a.rank || 0) - (b.rank || 0) || (a.label < b.label ? -1 : a.label > b.label ? 1 : 0);
  tier0.sort(byRank);
  tier1.sort(byRank);
  return tier0.concat(tier1).slice(0, MAX_ITEMS);
}

/**
 * Line/column of an offset — the geometry the DOM layer converts to pixels
 * (monospace editor, tab stops counted at the editor's tab size).
 *
 * @param {string} text full editor text
 * @param {number} caret caret offset
 * @returns {{ line: number, col: number }}
 */
export function lineCol(text, caret) {
  let line = 0;
  let col = 0;
  for (let i = 0; i < caret; i++) {
    const c = text[i];
    if (c === "\n") {
      line++;
      col = 0;
    } else if (c === "\t") {
      col += 2 - (col % 2); // tab-size 2, as styled in style.css
    } else {
      col++;
    }
  }
  return { line, col };
}
