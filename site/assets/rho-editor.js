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
import { setDiagnostics, lintGutter } from '@codemirror/lint';
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

// The theme carries literal colors from the language's own palette — no
// host CSS variables. This file is the single source for BOTH surfaces, and
// the static site defines no stage tokens: a var() here silently degrades
// to CodeMirror's default light chrome (white gutters, white tooltips).
// Scrollbars follow the family law (monorepo docs/family.md): invisible
// until hover or keyboard focus, never a track, width always reserved.
const rhoTheme = EditorView.theme({
  '&': {
    background: 'transparent',
    color: '#e8e6e1',
    height: '100%',
    fontSize: '12.5px',
  },
  '&.cm-focused': { outline: 'none' },
  '.cm-scroller': {
    fontFamily: MONO,
    lineHeight: '1.55',
    overflow: 'auto',
    scrollbarWidth: 'thin',
    scrollbarColor: 'transparent transparent',
  },
  '.cm-scroller::-webkit-scrollbar': {
    width: '10px',
    height: '10px',
    background: 'transparent',
  },
  '.cm-scroller::-webkit-scrollbar-thumb': {
    background: 'transparent',
    borderRadius: '5px',
  },
  '.cm-scroller::-webkit-scrollbar-corner': { background: 'transparent' },
  '&:hover .cm-scroller, &:focus-within .cm-scroller': {
    scrollbarColor: '#3a3a42 transparent',
  },
  '&:hover .cm-scroller::-webkit-scrollbar-thumb, &:focus-within .cm-scroller::-webkit-scrollbar-thumb': {
    background: '#3a3a42',
  },
  '.cm-content': { caretColor: '#e8a04c' },
  '.cm-cursor, .cm-dropCursor': { borderLeftColor: '#e8a04c' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {
    backgroundColor: 'rgba(232, 160, 76, 0.22)',
  },
  '.cm-activeLine': { backgroundColor: 'rgba(232, 160, 76, 0.07)' },
  '.cm-gutters': {
    background: 'transparent',
    color: '#6b6862',
    border: 'none',
  },
  '.cm-activeLineGutter': { background: 'transparent', color: '#9b9890' },
  '.cm-tooltip': {
    background: '#17171b',
    border: '1px solid #26262c',
    borderRadius: '8px',
    boxShadow: '0 8px 24px rgba(0, 0, 0, 0.45)',
    padding: '2px',
    color: '#e8e6e1',
    maxWidth: '440px',
    overflow: 'hidden',
  },
  '.cm-tooltip-arrow': { display: 'none' },
  '.cm-tooltip.cm-tooltip-autocomplete > ul': {
    fontFamily: MONO,
    background: 'transparent',
    border: 'none',
    borderRadius: '6px',
    padding: '4px',
    maxHeight: '240px',
  },
  '.cm-tooltip.cm-tooltip-autocomplete > ul > li': {
    color: '#e8e6e1',
    padding: '3px 8px',
    borderRadius: '5px',
  },
  '.cm-tooltip.cm-tooltip-autocomplete > ul > li[aria-selected]': {
    backgroundColor: 'rgba(232, 160, 76, 0.16)',
  },
  '.cm-completionDetail': { color: '#9b9890' },
  '.cm-completionInfo': {
    background: '#17171b',
    border: '1px solid #26262c',
    color: '#9b9890',
  },
  '.cm-tooltip.cm-tooltip-lint': { fontFamily: MONO, fontSize: '12px', padding: '0' },
  '.cm-diagnostic': { color: '#e8e6e1', padding: '6px 10px' },
});


/**
 * Mount a rho CodeMirror editor. Returns { view, setDoc, focus, destroy }.
 * @param {{ parent: HTMLElement, value: string, onChange: (v: string) => void,
 *            onRun: () => void, onFormat?: () => void }} opts
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
      lintGutter(),
      EditorView.updateListener.of((u) => {
        if (u.docChanged) opts.onChange(u.state.doc.toString());
      }),
      keymap.of([
        { key: 'Mod-Enter', run: () => { opts.onRun(); return true; } },
        { key: 'Shift-Alt-f', run: () => { if (opts.onFormat) { opts.onFormat(); return true; } return false; } },
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
    /** apply compiler diagnostics ({ line, col, severity, message }[]) */
    setDiagnostics(diags) {
      view.dispatch(
        setDiagnostics(
          view.state,
          diags.map((d) => {
            const l = view.state.doc.line(Math.min(Math.max(d.line, 1), view.state.doc.lines));
            const from = Math.min(l.from + Math.max(0, d.col - 1), l.to);
            return {
              from,
              to: Math.max(from + 1, Math.min(l.to, from + 64)),
              severity: d.severity || 'error',
              message: d.message,
            };
          }),
        ),
      );
    },
    destroy: () => view.destroy(),
  };
}
