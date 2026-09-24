# The rho site

Hand-written static pages — no framework, no build step (the one exception:
`tools/bundle-codemirror.mjs` pre-bundles CodeMirror 6 + the shared editor
core into `assets/codemirror.bundle.js`, a committed file). The playground
downloads the real compiler (`rho.wasm`, a wasm32-wasi program) and compiles
+ runs your program in the browser: execution happens in a worker so a
runaway loop can be stopped from the UI, while compilation happens on the
main thread (worker contexts miscompile — see docs/todo.md), bounded by the
language's own phase caps.

## Pages

| file              | what it is                                        |
| ----------------- | ------------------------------------------------- |
| `index.html`      | landing page                                      |
| `tutorial.html`   | the tutorial (examples shared with the playground) |
| `playground.html` | the editor + compiler playground                  |
| `spec.html`       | spec reader over `spec/` markdown                 |

The playground feeds the stdin box (under the editor) to the program's
fd 0 — one `read_line()` per line, EOF past the end. When the program
reads PAST what the box holds and the browser speaks JSPI
(`WebAssembly.Suspending`), the run suspends and a terminal row appears:
type a line, Enter hands it to the program, the EOF button (or Ctrl+D)
closes stdin. Without JSPI the same reads simply see EOF.

The tutorial's examples are the same editor running in place: the
compiler loads once per page, every edit is checked (squiggles through
the lint system), Run compiles + executes in the shared worker, and
stdin follows the two modes above.

The editor is CodeMirror 6 carrying the language's own law: enter-key
auto-indent (brace depth, two spaces), bracket closing, completion over
the shared fast tables, and live diagnostics — the compiler's `check`
runs on idle and paints squiggles + gutter markers through the lint
system. The first check fires on page load (once the compiler is warm),
no edit required. Format (Shift-Alt-F) pipes through the compiler's
`fmt`. The caps are phase-scoped: the load cap (120 s) covers the
compiler download, the run cap (10 s) measures rho.

## Assets (`assets/`)

| file                   | role                                                         |
| ---------------------- | ------------------------------------------------------------ |
| `compiler.js`          | fetches `rho.wasm`, exposes compile / check / fmt / run       |
| `worker.js`            | off-main-thread execution, Stop-able                         |
| `wasi.js`              | minimal wasm32-wasi host (used by `tools/` tests too)        |
| `playground.js`        | playground wiring: editor, run pipeline, persistence          |
| `rho-editor.js`        | the shared CodeMirror 6 core (also imported by the app)       |
| `codemirror.bundle.js` | rho-editor.js + CM6, pre-bundled for this no-build site       |
| `editor-entry.js`      | the bundle's entry point                                     |
| `completion-core.js`   | pure completion logic: word extraction, scan, filter/rank     |
| `completion-data.js`   | static tables: keywords, types, builtins, prelude             |
| `highlight.js`         | tiny rho syntax highlighter (non-editor pages)                |
| `examples.js`          | every example program (verified by `tools/verify_examples.mjs`) |
| `style.css`            | the whole site's styles                                      |

## Completion

The editor completes over three sources, merged and ranked by
`completion-core.js`:

1. **Language keywords and primitive types** — static tables in
   `completion-data.js`, transcribed from `spec/syntax.md` and the spec's
   type section.
2. **Symbols visible in the buffer** — `scanSymbols` extracts `fn` /
   `struct` / `enum` / `trait` names and `let` / `const` / `static`
   bindings with regexes. Module-level items are order-independent;
   bindings only count before the caret (a lexical approximation of
   visibility). Methods (`fn Type.name`) are skipped.
3. **The prelude** — the user-facing surface of `boot/prelude/core.rho`
   (builtins per `spec/module-system.md`, plus `assert*` / `Option` /
   `Result` / `Show`), as static data with English docs. String
   concatenation is the `+` operator, not a prelude function.

Typing an identifier opens the popup; ArrowUp/Down move, Tab or Enter
accepts, Escape closes; Ctrl/Cmd+Space forces it. The type-aware layer is
specified in [docs/language-service.md](../docs/language-service.md);
`completion-core.js` defines the item/rank vocabulary both surfaces share.

## Checking the compiler pipeline

The playground's compile path is what `tools/test_browser_compiler.mjs`
drives over the corpus (read-only, runs in Node):

```bash
node tools/test_browser_compiler.mjs
```

The visual probe drives the real page in headless Chrome — the seeded-draft
first check, themed chrome, the scrollbar law, the dark tooltip — and
leaves screenshots in /tmp:

```bash
node tools/probe_playground_visual.mjs
```
