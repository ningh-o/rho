// The rho editor core: one source for BOTH surfaces. The rho.ningh.org app
// imports this file straight from the submodule (rho-site/* alias, typed
// via these JSDoc annotations); the site bundles it with CodeMirror via
// tools/bundle-codemirror.mjs. A stream parser carrying the same token
// classes as highlight.js, enter-key auto-indent (brace depth, two spaces),
// bracket closing, history, and completion over the shared fast tables.

// The rho editor: CodeMirror 6 wired for the language — a stream parser
// carrying the same token classes as highlightRho, enter-key auto-indent
// (brace depth, two spaces), bracket closing, history, and completion fed
// by the language's shared fast tables (rho-site/completion-*.js — the
// single source lives in the rho submodule; no vendored copy).


import { EditorState, EditorSelection } from '@codemirror/state';
import { EditorView, keymap, highlightActiveLine } from '@codemirror/view';
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands';
import {
  StreamLanguage,
  indentUnit,
  syntaxHighlighting,
  HighlightStyle,
} from '@codemirror/language';
import {
  closeBrackets,
  closeBracketsKeymap,
  autocompletion,
} from '@codemirror/autocomplete';
import { tags as t, Tag } from '@lezer/highlight';
import { wordBefore, scanSymbols, filterItems } from './completion-core.js';
import { STATIC_ITEMS } from './completion-data.js';

const KEYWORDS = new Set([
  'fn', 'let', 'mut', 'if', 'else', 'while', 'loop', 'break', 'continue',
  'return', 'defer', 'struct', 'enum', 'use', 'pub', 'static', 'const',
  'match', 'as', 'new', 'null', 'true', 'false', 'weak', 'self', 'extern',
  'trait', 'impl', 'for', 'dyn',
]);

const TYPES = new Set([
  'bool', 'i8', 'i16', 'i32', 'i64', 'isize', 'u8', 'u16', 'u32', 'u64',
  'usize', 'f32', 'f64', 'string', 'void',
]);

const BUILTINS = new Set([
  'printf', 'eprintf', 'format', 'panic', 'make', 'len', 'assert',
  'assert_eq', 'read_line', 'size_of',
]);

// the builtin tag — same warm copper the old overlay gave printf & co.
const BiTag = Tag.define();

// the same five token classes highlightRho paints, as a stream parser
const rhoLanguage = StreamLanguage.define({
  name: 'rho',
  startState() {
    return { inString: false };
  },
  token(stream, state) {
    if (stream.match('//')) {
      stream.skipToEnd();
      return 'comment';
    }
    if (state.inString || stream.match('"')) {
      state.inString = true;
      while (!stream.eol()) {
        const ch = stream.next();
        if (ch === '\\') {
          stream.next(); // the escaped char, whatever it is
        } else if (ch === '"') {
          state.inString = false;
          break;
        }
      }
      return 'string';
    }
    if (stream.match(/[0-9][0-9a-fA-FxXbBoO_.]*/)) return 'number';
    if (stream.match(/[A-Za-z_][A-Za-z0-9_]*/)) {
      const w = stream.current();
      if (TYPES.has(w)) return 'typeName';
      if (KEYWORDS.has(w)) return 'keyword';
      if (BUILTINS.has(w)) return 'builtin';
      if (/^[A-Z]/.test(w)) return 'typeName';
      return null;
    }
    stream.next();
    return null;
  },
  languageData: {
    commentTokens: { line: '//' },
    indentUnit: '  ',
    closeBrackets: { brackets: ['(', '[', '{', '"'] },
  },
  tokenTable: {
    keyword: t.keyword,
    typeName: t.typeName,
    number: t.number,
    string: t.string,
    comment: t.lineComment,
    builtin: BiTag,
  },
});

const rhoHighlight = HighlightStyle.define([
  { tag: t.keyword, color: '#d886c8' },
  { tag: t.typeName, color: '#7fb3d5' },
  { tag: t.string, color: '#a3be8c' },
  { tag: t.number, color: '#e8a04c' },
  { tag: t.lineComment, color: '#8a8170', fontStyle: 'italic' },
  { tag: BiTag, color: '#d08770' },
]);

// enter with brace-aware indentation: two spaces deeper after a line that
// opens a block, one level out when the next line closes one
function rhoNewLine(view) {
  const { state } = view;
  const changes = state.changeByRange((range) => {
    const line = state.doc.lineAt(range.from);
    const before = line.text.slice(0, Math.max(0, range.from - line.from));
    const trimmed = before.trimEnd();
    let indent = before.match(/^ */)[0];
    if (trimmed.endsWith('{')) {
      indent += '  ';
    } else if (state.doc.lineAt(Math.min(range.to + 1, state.doc.length)).text.trimStart().startsWith('}')) {
      indent = indent.slice(0, Math.max(0, indent.length - 2));
    }
    return {
      changes: { from: range.from, to: range.to, insert: '\n' + indent },
      range: EditorSelection.cursor(range.from + 1 + indent.length),
    };
  });
  view.dispatch(changes);
  return true;
}

// completion over the shared fast tables: buffer symbols plus the static
// prelude surface, ranked by completion-core exactly like the site
function rhoCompletion(context) {
  const text = context.state.doc.toString();
  const w = wordBefore(text, context.pos);
  if (!w) return null;
  const merged = scanSymbols(text, context.pos).concat(STATIC_ITEMS);
  const filtered = filterItems(merged, w.prefix);
  if (!filtered.length) return null;
  return {
    from: w.wordStart,
    options: filtered.map((it) => ({
      label: it.label,
      detail: it.detail,
      type: it.kind,
      apply: it.label,
    })),
    validFor: /^[A-Za-z_][A-Za-z0-9_]*$/,
  };
}

const MONO = '"SF Mono", ui-monospace, "Cascadia Code", Menlo, Consolas, monospace';

const rhoTheme = EditorView.theme({
  '&': {
    background: 'transparent',
    color: 'var(--color-stage-ink)',
    height: '100%',
    fontSize: '12.5px',
  },
  '&.cm-focused': { outline: 'none' },
  '.cm-scroller': {
    fontFamily: MONO,
    lineHeight: '1.55',
    overflow: 'auto',
  },
  '.cm-content': { caretColor: 'var(--color-stage-gold)' },
  '.cm-cursor, .cm-dropCursor': { borderLeftColor: 'var(--color-stage-gold)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection':
    { backgroundColor: 'color-mix(in srgb, var(--color-stage-gold) 22%, transparent)' },
  '.cm-activeLine': {
    backgroundColor: 'color-mix(in srgb, var(--color-stage-gold) 7%, transparent)',
  },
  '.cm-tooltip.cm-tooltip-autocomplete > ul': {
    fontFamily: MONO,
    background: 'var(--color-stage-2)',
    border: '1px solid var(--color-stage-3)',
    borderRadius: '8px',
    padding: '4px',
    maxHeight: '240px',
  },
  '.cm-tooltip.cm-tooltip-autocomplete > ul > li': {
    color: 'var(--color-stage-ink)',
    padding: '3px 8px',
    borderRadius: '5px',
  },
  '.cm-tooltip.cm-tooltip-autocomplete > ul > li[aria-selected]': {
    backgroundColor: 'color-mix(in srgb, var(--color-stage-gold) 16%, transparent)',
  },
  '.cm-completionDetail': { color: 'var(--color-stage-soft)' },
  '.cm-completionInfo': {
    background: 'var(--color-stage-2)',
    border: '1px solid var(--color-stage-3)',
    color: 'var(--color-stage-soft)',
  },
});


/**
 * Mount a rho CodeMirror editor. Returns { view, setDoc, focus, destroy }.
 * @param {{ parent: HTMLElement, value: string, onChange: (v: string) => void,
 *            onRun: () => void }} opts
 */
export function createRhoEditor(opts) {
  const state = EditorState.create({
    doc: opts.value,
    extensions: [
      indentUnit.of('  '),
      rhoLanguage,
      syntaxHighlighting(rhoHighlight),
      history(),
      closeBrackets(),
      autocompletion({
        override: [rhoCompletion],
        activateOnTyping: true,
        icons: false,
      }),
      highlightActiveLine(),
      EditorView.updateListener.of((u) => {
        if (u.docChanged) opts.onChange(u.state.doc.toString());
      }),
      keymap.of([
        { key: 'Mod-Enter', run: () => { opts.onRun(); return true; } },
        { key: 'Enter', run: rhoNewLine },
        ...closeBracketsKeymap,
        ...historyKeymap,
        indentWithTab,
        ...defaultKeymap,
      ]),
      rhoTheme,
    ],
  });
  const view = new EditorView({ state, parent: opts.parent });
  return {
    view,
    setDoc(value) {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: value },
      });
    },
    focus: () => view.focus(),
    destroy: () => view.destroy(),
  };
}
