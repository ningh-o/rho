// Single-file symbol index for hover and go-to-definition.
//
// Honest scope: this is a fast syntactic scan of ONE document — the
// compiler wasm exposes check/fmt/build but no queryable semantic model,
// so the server builds its own lightweight table. It understands:
//
//   - top-level declarations: fn (incl. `fn Recv.method` and generics),
//     struct, enum, trait, use, let/const/static
//   - function parameters (scoped to the function's span)
//   - let/const/static bindings inside a function body (scoped the same)
//
// It does NOT do type inference, cross-file name resolution, or full
// shadowing analysis beyond "innermost binding wins". Everything it
// reports is verifiable from the text itself. Closures inside function
// bodies are not indexed (their spans are skipped wholesale).

export interface Span {
  start: number;
  end: number;
}

export interface RhoSymbol {
  /** Declaration keyword: fn / struct / enum / trait / let / const / static / use / param */
  kind: string;
  /** Plain name (for methods: `Recv.method`). */
  name: string;
  /** Human-readable signature shown on hover, e.g. `fn main() -> i32`. */
  detail: string;
  /** Span of the name token inside the declaration (definition target). */
  nameRange: Span;
  /** Span of the whole declaration (scope anchor for fns). */
  span: Span;
}

export interface SymbolIndex {
  /** File-level symbols in source order. */
  topLevel: RhoSymbol[];
  /** Function symbols (body or extern signature). */
  functions: RhoSymbol[];
  /** Parameters and body-local bindings, grouped by owning function. */
  localsByFn: Map<RhoSymbol, RhoSymbol[]>;
}

const IDENT_START = /[A-Za-z_]/;
const IDENT_CHAR = /[A-Za-z0-9_]/;

function isIdentChar(c: string | undefined): boolean {
  return c !== undefined && IDENT_CHAR.test(c);
}

/** Read the identifier at offset i; null if none starts there. */
function readIdent(src: string, i: number): { start: number; end: number; text: string } | null {
  if (i >= src.length || !IDENT_START.test(src[i])) return null;
  const start = i;
  while (i < src.length && IDENT_CHAR.test(src[i])) i++;
  return { start, end: i, text: src.slice(start, i) };
}

/** Skip whitespace and // comments from i. */
function skipTrivia(src: string, i: number): number {
  for (;;) {
    while (i < src.length && /\s/.test(src[i])) i++;
    if (src[i] === '/' && src[i + 1] === '/') {
      while (i < src.length && src[i] !== '\n') i++;
      continue;
    }
    return i;
  }
}

/** Offset of the last non-whitespace char before i (-1 if none). */
function lastNonSpace(src: string, i: number): number {
  let j = i - 1;
  while (j >= 0 && /\s/.test(src[j])) j--;
  return j;
}

/** Skip a string literal starting at (or after) i; returns the offset after
 *  the closing quote (or src.length on unterminated input). */
function skipString(src: string, i: number): number {
  if (src[i] !== '"') return i;
  i++;
  while (i < src.length && src[i] !== '"') {
    if (src[i] === '\\') i++;
    i++;
  }
  return Math.min(i + 1, src.length);
}

/** Skip to the matching close of the bracket open at i. -1 if unbalanced. */
function matchBracket(src: string, open: number, openCh: string, closeCh: string): number {
  let depth = 1;
  let i = open + 1;
  while (i < src.length) {
    if (src[i] === '"') {
      i = skipString(src, i);
      continue;
    }
    if (src[i] === '/' && src[i + 1] === '/') {
      while (i < src.length && src[i] !== '\n') i++;
      continue;
    }
    if (src[i] === openCh) depth++;
    else if (src[i] === closeCh) {
      depth--;
      if (depth === 0) return i;
    }
    i++;
  }
  return -1;
}

/** From i, find the first bracket-nested `{`, `;` or `}` — the end of a
 *  signature region. Returns -1 when the file ends first. */
function topLevelTerminator(src: string, i: number): number {
  let depth = 0;
  while (i < src.length) {
    if (src[i] === '"') {
      i = skipString(src, i);
      continue;
    }
    if (src[i] === '/' && src[i + 1] === '/') {
      while (i < src.length && src[i] !== '\n') i++;
      continue;
    }
    if (src[i] === '(' || src[i] === '[') depth++;
    else if (src[i] === ')' || src[i] === ']') depth--;
    else if (depth === 0 && (src[i] === '{' || src[i] === ';' || src[i] === '}')) return i;
    i++;
  }
  return -1;
}

/** A decl keyword occurrence: `kw` at i, bounded by non-ident chars on both
 *  sides (whitespace/`;`/`{`/`}`/start before, non-ident after). */
function isKeywordAt(src: string, i: number, kw: string): boolean {
  if (!src.startsWith(kw, i)) return false;
  if (isIdentChar(src[i + kw.length])) return false;
  const prev = lastNonSpace(src, i);
  if (prev >= 0 && IDENT_CHAR.test(src[prev])) return false;
  return true;
}

/** Read `<ident>` after trivia at i; null when absent. */
function expectIdent(src: string, i: number): { start: number; end: number; text: string; after: number } | null {
  const j = skipTrivia(src, i);
  const id = readIdent(src, j);
  return id ? { ...id, after: id.end } : null;
}

function detailSlice(src: string, start: number, end: number): string {
  return src.slice(start, Math.max(start, end)).replace(/\s+/g, ' ').trim();
}

/** Type text of a binding: after `name`, the `: T` up to a top-level `=`,
 *  `,`, `;`, `{` or newline. Empty when there is no annotation. */
function bindingType(src: string, afterName: number): string {
  let i = skipTrivia(src, afterName);
  if (src[i] !== ':') return '';
  i++;
  const start = skipTrivia(src, i);
  let depth = 0;
  let j = start;
  while (j < src.length) {
    const c = src[j];
    if (c === '<' || c === '[' || c === '(') depth++;
    else if (c === '>' || c === ']' || c === ')') {
      if (depth === 0) break;
      depth--;
    } else if (depth === 0 && (c === '=' || c === ',' || c === ';' || c === '{' || c === '\n')) break;
    j++;
  }
  return src.slice(start, j).replace(/\s+/g, ' ').trim();
}

/** Parameter names of the list between openParen and closeParen:
 *  the leading identifier of each top-level comma chunk (`name: T`). */
function parseParams(src: string, openParen: number, closeParen: number): { name: string; start: number; end: number }[] {
  const out: { name: string; start: number; end: number }[] = [];
  let i = openParen + 1;
  for (;;) {
    i = skipTrivia(src, i);
    if (i >= closeParen) break;
    const id = readIdent(src, i);
    if (!id) {
      i++;
      continue;
    }
    // first identifier of a chunk is the name unless it is `mut`
    if (id.text !== 'mut') {
      out.push({ name: id.text, start: id.start, end: id.end });
    }
    // skip to the next top-level comma
    let depth = 0;
    let j = id.end;
    while (j < closeParen) {
      const c = src[j];
      if (c === '<' || c === '[' || c === '(') depth++;
      else if (c === '>' || c === ']' || c === ')') depth--;
      else if (depth === 0 && c === ',') break;
      j++;
    }
    i = j + 1;
  }
  return out;
}

/** let/const/static bindings inside [start, end). */
function scanLocals(src: string, start: number, end: number): RhoSymbol[] {
  const out: RhoSymbol[] = [];
  let i = start;
  while (i < end) {
    if (src[i] === '"') {
      i = skipString(src, i);
      continue;
    }
    if (src[i] === '/' && src[i + 1] === '/') {
      while (i < end && src[i] !== '\n') i++;
      continue;
    }
    const kw = ['let', 'const', 'static'].find((k) => isKeywordAt(src, i, k));
    if (kw) {
      let j = skipTrivia(src, i + kw.length);
      let mut = false;
      const mutId = readIdent(src, j);
      if (mutId && mutId.text === 'mut' && !isIdentChar(src[mutId.end])) {
        mut = true;
        j = skipTrivia(src, mutId.end);
      }
      const id = readIdent(src, j);
      if (id) {
        const type = bindingType(src, id.end);
        out.push({
          kind: kw,
          name: id.text,
          detail: `${kw}${mut ? ' mut' : ''} ${id.text}${type ? ': ' + type : ''}`,
          nameRange: { start: id.start, end: id.end },
          span: { start: i, end: id.end },
        });
        i = id.end;
        continue;
      }
    }
    i++;
  }
  return out;
}

const DECL_KEYWORDS = ['fn', 'struct', 'enum', 'trait', 'use', 'let', 'const', 'static'] as const;

/** Enum variants inside [openBrace, closeBrace): top-level identifiers at
 *  slot starts (after `{` or `,`), payloads skipped. Tuple payloads become
 *  part of the detail text, struct-variant fields are not indexed. */
function enumVariants(
  src: string,
  openBrace: number,
  closeBrace: number,
  enumName: string,
): RhoSymbol[] {
  const out: RhoSymbol[] = [];
  let i = openBrace + 1;
  while (i < closeBrace) {
    i = skipTrivia(src, i);
    if (i >= closeBrace) break;
    const id = readIdent(src, i);
    if (!id) {
      i++;
      continue;
    }
    const prev = lastNonSpace(src, id.start);
    const prevCh = prev >= 0 ? src[prev] : '{';
    let end = id.end;
    const next = skipTrivia(src, id.end);
    if (src[next] === '{') {
      const cb = matchBracket(src, next, '{', '}');
      end = cb >= 0 ? cb + 1 : closeBrace;
    } else if (src[next] === '(') {
      const cb = matchBracket(src, next, '(', ')');
      end = cb >= 0 ? cb + 1 : id.end;
    }
    if (prevCh === '{' || prevCh === ',') {
      // detail: the variant's own text up to the next top-level comma
      let stop = end;
      if (stop <= id.end) {
        let depth = 0;
        let j = id.end;
        while (j < closeBrace) {
          const ch = src[j];
          if (ch === '(' || ch === '[') depth++;
          else if (ch === ')' || ch === ']') depth--;
          else if (depth === 0 && ch === ',') break;
          j++;
        }
        stop = j;
      }
      out.push({
        kind: 'variant',
        name: id.text,
        detail: detailSlice(src, id.start, stop).replace(/^/, `${enumName}.`),
        nameRange: { start: id.start, end: id.end },
        span: { start: id.start, end: stop },
      });
    }
    i = Math.max(end, id.end);
  }
  return out;
}

/** Index every symbol in one source document. Never throws — a malformed
 *  document yields whatever symbols were recognisable. */
export function indexSymbols(src: string): SymbolIndex {
  const topLevel: RhoSymbol[] = [];
  const functions: RhoSymbol[] = [];
  const localsByFn = new Map<RhoSymbol, RhoSymbol[]>();

  let i = 0;
  while (i < src.length) {
    if (src[i] === '"') {
      i = skipString(src, i);
      continue;
    }
    if (src[i] === '/' && src[i + 1] === '/') {
      while (i < src.length && src[i] !== '\n') i++;
      continue;
    }

    const kw = DECL_KEYWORDS.find((k) => isKeywordAt(src, i, k));
    if (!kw) {
      i++;
      continue;
    }

    if (kw === 'use') {
      let j = skipTrivia(src, i + kw.length);
      const pathStart = j;
      while (j < src.length && /[A-Za-z0-9_/]/.test(src[j])) j++;
      const path = src.slice(pathStart, j);
      const bind = path.split('/').pop() ?? path;
      topLevel.push({
        kind: 'use',
        name: bind,
        detail: `use ${path}`,
        nameRange: { start: j - bind.length, end: j },
        span: { start: i, end: j },
      });
      i = j;
      continue;
    }

    if (kw === 'let' || kw === 'const' || kw === 'static') {
      // top-level binding — same shape as a body local
      const stop = topLevelTerminator(src, i + kw.length);
      const [local] = scanLocals(src, i, stop >= 0 ? stop : src.length);
      if (local) {
        topLevel.push({ ...local, span: { start: i, end: local.nameRange.end } });
        i = local.nameRange.end;
        continue;
      }
      i += kw.length;
      continue;
    }

    // fn / struct / enum / trait: read the (possibly qualified) name
    let j = skipTrivia(src, i + kw.length);
    const first = expectIdent(src, j);
    if (!first) {
      i += kw.length;
      continue;
    }
    let name = first.text;
    let nameStart = first.start;
    let nameEnd = first.end;
    let afterName = skipTrivia(src, first.after);
    if (kw === 'fn' && src[afterName] === '.') {
      const method = expectIdent(src, afterName + 1);
      if (method) {
        name = `${first.text}.${method.text}`;
        nameStart = method.start;
        nameEnd = method.end;
        afterName = skipTrivia(src, method.after);
      }
    }
    // optional generic parameter list [A, B] before the value parameters
    if (src[afterName] === '[') {
      const close = matchBracket(src, afterName, '[', ']');
      if (close >= 0) afterName = skipTrivia(src, close + 1);
    }

    if (kw === 'fn' && src[afterName] === '(') {
      const openParen = afterName;
      const closeParen = matchBracket(src, openParen, '(', ')');
      if (closeParen >= 0) {
        const sigStop = topLevelTerminator(src, closeParen + 1);
        const detailEnd = sigStop >= 0 ? sigStop : Math.min(src.length, closeParen + 200);
        const sym: RhoSymbol = {
          kind: 'fn',
          name,
          detail: detailSlice(src, i, detailEnd),
          nameRange: { start: nameStart, end: nameEnd },
          span: { start: i, end: closeParen + 1 },
        };
        const params: RhoSymbol[] = parseParams(src, openParen, closeParen).map(
          (p): RhoSymbol => ({
            kind: 'param',
            name: p.name,
            detail: `${p.name}: ${bindingType(src, p.end)}`,
            nameRange: { start: p.start, end: p.end },
            span: { start: openParen, end: closeParen + 1 },
          }),
        );
        if (sigStop >= 0 && src[sigStop] === '{') {
          const closeBrace = matchBracket(src, sigStop, '{', '}');
          const bodyEnd = closeBrace >= 0 ? closeBrace : src.length;
          sym.span = { start: i, end: bodyEnd };
          localsByFn.set(sym, [...params, ...scanLocals(src, sigStop + 1, bodyEnd)]);
        } else {
          localsByFn.set(sym, params);
        }
        functions.push(sym);
        topLevel.push(sym);
        i = sym.span.end;
        continue;
      }
    }

    // struct / enum / trait (or fn without a parameter list)
    const sigStop = topLevelTerminator(src, afterName);
    const detailEnd = sigStop >= 0 ? sigStop : Math.min(src.length, afterName + 200);
    topLevel.push({
      kind: kw,
      name,
      detail: detailSlice(src, i, detailEnd),
      nameRange: { start: nameStart, end: nameEnd },
      span: { start: i, end: nameEnd },
    });
    i = nameEnd;
    if (kw === 'enum' && sigStop >= 0 && src[sigStop] === '{') {
      const closeBrace = matchBracket(src, sigStop, '{', '}');
      const end = closeBrace >= 0 ? closeBrace : src.length;
      for (const v of enumVariants(src, sigStop, end, name)) topLevel.push(v);
      i = end;
    }
  }

  return { topLevel, functions, localsByFn };
}

// ------------------------------------------------------------- queries --

export interface TokenAt {
  start: number;
  end: number;
  text: string;
}

/** The identifier token overlapping a 0-based offset (the cursor may sit at
 *  either edge of the token), null if none. */
export function identAt(src: string, offset: number): TokenAt | null {
  if (offset < 0 || offset > src.length) return null;
  if (offset === src.length || !IDENT_CHAR.test(src[offset])) {
    if (offset > 0 && IDENT_CHAR.test(src[offset - 1])) {
      let start = offset - 1;
      while (start > 0 && IDENT_CHAR.test(src[start - 1])) start--;
      return { start, end: offset, text: src.slice(start, offset) };
    }
    return null;
  }
  let start = offset;
  while (start > 0 && IDENT_CHAR.test(src[start - 1])) start--;
  let end = offset;
  while (end < src.length && IDENT_CHAR.test(src[end])) end++;
  return { start, end, text: src.slice(start, end) };
}

export interface Lookup {
  /** The symbol declared with this name that the position refers to. */
  definition: RhoSymbol | null;
  /** The function whose scope contains the position, if any. */
  owner: RhoSymbol | null;
}

/** Resolve the name at `offset` to its definition within this document.
 *  Inside a function, params and body locals shadow file-level names. */
export function resolveName(index: SymbolIndex, src: string, offset: number): Lookup {
  const token = identAt(src, offset);
  if (!token) return { definition: null, owner: null };
  const name = token.text;

  let owner: RhoSymbol | null = null;
  for (const fn of index.functions) {
    if (offset >= fn.span.start && offset < fn.span.end) owner = fn;
  }

  if (owner) {
    let best: RhoSymbol | null = null;
    for (const l of index.localsByFn.get(owner) ?? []) {
      if (l.name !== name) continue;
      if (l.span.start <= token.start && (best === null || l.span.start > best.span.start)) best = l;
    }
    if (best) return { definition: best, owner };
  }

  for (const sym of index.topLevel) {
    if (sym.name === name) return { definition: sym, owner };
  }

  // the cursor may sit on a declaration's own name (file-level, or a
  // param/local of the enclosing function)
  for (const sym of index.topLevel) {
    if (offset >= sym.nameRange.start && offset <= sym.nameRange.end) {
      return { definition: sym, owner };
    }
  }
  if (owner) {
    for (const l of index.localsByFn.get(owner) ?? []) {
      if (offset >= l.nameRange.start && offset <= l.nameRange.end) {
        return { definition: l, owner };
      }
    }
  }

  return { definition: null, owner };
}
