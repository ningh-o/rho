# The rho site

Hand-written static pages — no framework, no build step. The playground
downloads the real compiler (`rho.wasm`, a wasm32-wasi program) and
compiles + runs your program in the browser: compilation and execution
happen in a worker so a runaway loop can be stopped from the UI.

## Pages

| file             | what it is                                          |
| ---------------- | --------------------------------------------------- |
| `index.html`     | landing page                                        |
| `tutorial.html`  | the tutorial (examples shared with the playground)  |
| `playground.html`| the editor + compiler playground                    |

The playground feeds the stdin box (under the editor) to the program's
fd 0 — one `read_line()` per line, EOF past the end. The caps are
phase-scoped: the load cap (120 s) covers the compiler download, the
compile cap (20 s) and run cap (10 s) measure rho only.| `spec.html`      | spec reader over `spec/` markdown                   |

## Assets (`assets/`)

| file                | role                                                        |
| ------------------- | ----------------------------------------------------------- |
| `compiler.js`       | fetches `rho.wasm`, exposes compile/run                |
| `worker.js`         | off-main-thread compile + run, Stop-able                    |
| `wasi.js`           | minimal wasm32-wasi host (used by `tools/` tests too)       |
| `playground.js`     | playground wiring: editor, run pipeline, persistence        |
| `highlight.js`      | tiny rho syntax highlighter                                 |
| `examples.js`       | every example program (verified by `tools/verify_examples.mjs`) |
| `completion.js`     | completion popup controller (this feature)                  |
| `completion-core.js`| pure logic: word extraction, symbol scan, filter/rank       |
| `completion-data.js`| static tables: keywords, types, builtins, prelude           |
| `completion.css`    | popup styles                                                |
| `style.css`         | the whole site's styles                                     |

## Playground completion

`completion.js` wires a fast, type-unaware completion popup to the editor
textarea. Three sources feed it:

1. **Language keywords and primitive types** — static tables in
   `completion-data.js`, transcribed from `spec/syntax.md` (keywords,
   reserved words) and the spec's type section.
2. **Symbols visible in the buffer** — `scanSymbols` in
   `completion-core.js` extracts `fn` / `struct` / `enum` / `trait` names
   and `let` / `const` / `static` bindings with regexes. Module-level items
   are order-independent; bindings only count before the caret (a lexical
   approximation of visibility). Methods (`fn Type.name`) are skipped —
   they complete after a dot, which this layer does not cover.
3. **The prelude** — the user-facing surface of `boot/prelude/core.rho`
   (builtins per `spec/module-system.md`, plus `assert*` /
   `Option` / `Result` / `Show`), as static data with English docs. String
   concatenation is the `+` operator, not a prelude function.

Interaction: typing an identifier opens the popup (120 ms debounce);
ArrowUp/Down move, Tab or Enter accepts, Escape closes, clicking a row
accepts; Ctrl/Cmd+Space forces the popup. The popup is positioned by
measuring the monospace cell size and converting the caret's line/column
(`lineCol` in the core) to pixels — no editor library is pulled in.

**Fast version — the type-aware completion is specified in
[docs/language-service.md](../docs/language-service.md).** This layer is
its adapter seat: `attachCompletion(...).registerProvider(fn)` accepts a
provider that receives `{ text, caret, prefix, wordStart }` and returns
items (a Promise is fine); provider items outrank the static tables, and
the item/rank vocabulary is defined in `completion-core.js`.

## Checking the compiler pipeline

The playground's compile path is what `tools/test_browser_compiler.mjs`
drives over the corpus (read-only, runs in Node):

```bash
cd rho && node tools/test_browser_compiler.mjs
```
