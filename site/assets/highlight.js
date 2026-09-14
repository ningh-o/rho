// A tiny rho syntax highlighter: escapes HTML then wraps tokens in spans.

const KEYWORDS = new Set([
  "fn", "let", "mut", "if", "else", "while", "loop", "break", "continue",
  "return", "defer", "struct", "enum", "use", "pub", "static", "const",
  "match", "as", "new", "null", "true", "false", "weak", "self", "extern",
]);
const TYPES = new Set([
  "bool", "i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64",
  "usize", "f32", "f64", "string",
]);

function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

export function highlightRho(src) {
  let out = "";
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    // comments
    if (c === "/" && src[i + 1] === "/") {
      let j = src.indexOf("\n", i);
      if (j < 0) j = n;
      out += `<span class="com">${esc(src.slice(i, j))}</span>`;
      i = j;
      continue;
    }
    // strings
    if (c === '"') {
      let j = i + 1;
      while (j < n && src[j] !== '"') {
        if (src[j] === "\\") j++;
        j++;
      }
      j = Math.min(j + 1, n);
      out += `<span class="str">${esc(src.slice(i, j))}</span>`;
      i = j;
      continue;
    }
    // identifiers / keywords / types
    if (/[A-Za-z_]/.test(c)) {
      let j = i;
      while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
      const word = src.slice(i, j);
      // a type-ish word: primitive, or Capitalised
      if (TYPES.has(word)) out += `<span class="ty">${word}</span>`;
      else if (KEYWORDS.has(word)) out += `<span class="kw">${word}</span>`;
      else if (/^[A-Z]/.test(word)) out += `<span class="ty">${word}</span>`;
      else out += esc(word);
      i = j;
      continue;
    }
    // numbers
    if (/[0-9]/.test(c)) {
      let j = i;
      while (j < n && /[0-9a-fA-FxXbBoO_.]/.test(src[j])) j++;
      out += `<span class="num">${esc(src.slice(i, j))}</span>`;
      i = j;
      continue;
    }
    out += esc(c);
    i++;
  }
  return out;
}
