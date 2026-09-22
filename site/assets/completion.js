// Playground completion — the fast, type-unaware layer: language keywords,
// symbols scanned from the buffer, and the static prelude tables. The popup
// is hand-drawn and positioned relative to the caret (the editor is a plain
// textarea; no CodeMirror/Monaco is introduced for this).
//
// FAST COMPLETION: filtering is prefix/substring, symbol extraction is
// regex-grade, and there are no types here. Type-aware completion arrives
// with the compiler language service (docs/language-service.md) — this
// module is its adapter slot: registerProvider() accepts a provider that
// receives { text, caret, prefix, wordStart } and returns items (sync or a
// Promise); its items outrank the static ones.

import { wordBefore, scanSymbols, filterItems, lineCol } from "./completion-core.js";
import { STATIC_ITEMS } from "./completion-data.js";

const DEBOUNCE_MS = 120;

/**
 * Wire completion to a textarea. The editor is the .editor wrapper that
 * contains the textarea (position: relative) — the popup is appended there.
 *
 * Returns a handle for the page's own key handling and lifecycle:
 *   onKeydown(e)      — let the popup consume a key; call first in the
 *                       textarea's keydown listener, before the Tab handler
 *   dismiss()         — close the popup (call when the buffer is swapped)
 *   registerProvider(fn) — adapter slot for the language service (see above)
 *
 * @param {HTMLTextAreaElement} ta the editor textarea
 * @returns {{ onKeydown: (e: KeyboardEvent) => boolean,
 *             dismiss: () => void,
 *             registerProvider: (fn: Function) => void }}
 */
export function attachCompletion(ta) {
  const host = ta.parentElement; // .editor — position: relative in style.css

  const pop = document.createElement("div");
  pop.className = "cp-pop";
  pop.id = "cp-pop";
  pop.setAttribute("role", "listbox");
  pop.setAttribute("aria-label", "completions");
  pop.hidden = true;
  host.appendChild(pop);
  const hint = document.createElement("div");
  hint.className = "cp-hint";
  pop.appendChild(hint);
  let rows = []; // row elements, parallel to items

  // ---- caret geometry (monospace editor: charWidth x lineCol is exact) --

  let charW = 8.1;
  let lineH = 21.6;
  let padTop = 16;
  let padLeft = 16;

  function measure() {
    const cs = getComputedStyle(ta);
    const probe = document.createElement("span");
    probe.textContent = "M".repeat(20);
    probe.style.cssText =
      "position:absolute;visibility:hidden;white-space:pre;left:-9999px;top:0;";
    probe.style.fontFamily = cs.fontFamily;
    probe.style.fontSize = cs.fontSize;
    probe.style.fontWeight = cs.fontWeight;
    probe.style.fontStyle = cs.fontStyle;
    probe.style.letterSpacing = cs.letterSpacing;
    document.body.appendChild(probe);
    charW = probe.getBoundingClientRect().width / 20 || charW;
    probe.remove();
    lineH = parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.6 || lineH;
    padTop = parseFloat(cs.paddingTop) || 0;
    padLeft = parseFloat(cs.paddingLeft) || 0;
  }

  function caretPx() {
    const { line, col } = lineCol(ta.value, cur.wordStart);
    return {
      x: padLeft + col * charW - ta.scrollLeft,
      y: padTop + (line + 1) * lineH - ta.scrollTop, // below the caret line
      lineTop: padTop + line * lineH - ta.scrollTop, // top of the caret line
    };
  }

  // ---- state ------------------------------------------------------------

  let open = false;
  let items = [];
  let active = 0;
  let cur = { prefix: "", wordStart: 0 };
  let debounceTimer = null;
  let suppressOnce = false; // skip the one input cycle caused by accepting

  const providers = [];

  function position() {
    if (!open || !items.length) return;
    const { x, y, lineTop } = caretPx();
    const pw = pop.offsetWidth;
    const ph = pop.offsetHeight;
    const box = host.getBoundingClientRect();
    let left = Math.min(Math.max(8, x), Math.max(8, box.width - pw - 8));
    let top = y;
    if (y + ph > box.height - 4) top = lineTop - ph - 2; // flip above the caret
    pop.style.left = Math.round(left) + "px";
    pop.style.top = Math.round(Math.max(2, top)) + "px";
  }

  function render() {
    for (const r of rows) r.remove();
    rows = items.map((it, i) => {
      const row = document.createElement("div");
      row.className = "cp-row" + (i === active ? " cp-active" : "");
      row.id = "cp-opt-" + i;
      row.setAttribute("role", "option");
      row.setAttribute("aria-selected", i === active ? "true" : "false");
      const label = document.createElement("span");
      label.className = "cp-label";
      label.textContent = it.label;
      row.appendChild(label);
      if (it.detail) {
        const detail = document.createElement("span");
        detail.className = "cp-detail";
        detail.textContent = it.detail;
        row.appendChild(detail);
      }
      const kind = document.createElement("span");
      kind.className = "cp-kind cp-kind-" + it.kind;
      kind.textContent = it.kind;
      row.appendChild(kind);
      row.addEventListener("pointerdown", (e) => {
        e.preventDefault(); // keep the textarea focus (no blur)
        accept(i);
      });
      pop.insertBefore(row, hint); // rows above the hint footer
      return row;
    });
    const it = items[active];
    hint.textContent = it ? it.doc || it.detail || "" : "";
    ta.setAttribute("aria-controls", "cp-pop");
    ta.setAttribute("aria-activedescendant", "cp-opt-" + active);
  }

  function markActive() {
    rows.forEach((r, i) => {
      r.classList.toggle("cp-active", i === active);
      r.setAttribute("aria-selected", i === active ? "true" : "false");
    });
    const it = items[active];
    if (it) hint.textContent = it.doc || it.detail || "";
    const row = rows[active];
    if (row) row.scrollIntoView({ block: "nearest" });
    ta.setAttribute("aria-activedescendant", "cp-opt-" + active);
  }

  function show(nextItems, word) {
    cur = word;
    items = nextItems;
    active = 0;
    open = true;
    render();
    pop.hidden = false;
    position();
  }

  function dismiss() {
    if (debounceTimer) {
      clearTimeout(debounceTimer);
      debounceTimer = null;
    }
    if (!open) return;
    open = false;
    for (const r of rows) r.remove(); // no stale rows may survive into the next show
    items = [];
    rows = [];
    pop.hidden = true;
    ta.removeAttribute("aria-controls");
    ta.removeAttribute("aria-activedescendant");
  }

  // ---- the input cycle: debounce → gather → filter → show ---------------

  async function refresh() {
    const text = ta.value;
    const caret = ta.selectionStart;
    const w = wordBefore(text, caret);
    if (!w) {
      dismiss();
      return;
    }
    const ctx = { text, caret, prefix: w.prefix, wordStart: w.wordStart };
    let extra = [];
    try {
      extra = (await Promise.all(providers.map((p) => p(ctx)))).flat();
    } catch (err) {
      console.warn("completion provider failed:", err);
    }
    // A provider may have resolved after the buffer changed again: only
    // show what still matches the caret as it stands now.
    if (ta.value !== text || ta.selectionStart !== caret) return;
    const merged = scanSymbols(text, caret).concat(extra, STATIC_ITEMS);
    const filtered = filterItems(merged, w.prefix);
    if (!filtered.length) {
      dismiss();
      return;
    }
    show(filtered, w);
  }

  function schedule() {
    if (suppressOnce) {
      suppressOnce = false;
      return;
    }
    if (debounceTimer) clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      debounceTimer = null;
      refresh();
    }, DEBOUNCE_MS);
  }

  function accept(i) {
    const it = items[i];
    if (!it) return;
    const start = cur.wordStart;
    const end = start + cur.prefix.length;
    suppressOnce = true;
    ta.setRangeText(it.label, start, end, "end");
    // setRangeText does not fire input; the page's input listener re-renders
    // the highlight overlay and persists to localStorage.
    ta.dispatchEvent(new Event("input", { bubbles: true }));
    dismiss();
    ta.focus();
  }

  // ---- events -----------------------------------------------------------

  ta.addEventListener("input", schedule);
  ta.addEventListener("blur", dismiss);
  ta.addEventListener("scroll", dismiss);
  ta.addEventListener("click", dismiss); // caret repositioned by mouse
  window.addEventListener("resize", dismiss);

  /**
   * Key routing for the popup. Returns true when the key was consumed —
   * the page then returns early and must not also handle it (Tab would
   * otherwise insert two spaces).
   */
  function onKeydown(e) {
    if ((e.ctrlKey || e.metaKey) && e.code === "Space") {
      e.preventDefault();
      if (debounceTimer) {
        clearTimeout(debounceTimer);
        debounceTimer = null;
      }
      refresh(); // force: offer items for the word under the caret
      return true;
    }
    if (!open) return false;
    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        active = (active + 1) % items.length;
        markActive();
        return true;
      case "ArrowUp":
        e.preventDefault();
        active = (active - 1 + items.length) % items.length;
        markActive();
        return true;
      case "Enter":
      case "Tab":
        e.preventDefault();
        accept(active);
        return true;
      case "Escape":
        e.preventDefault();
        dismiss();
        return true;
      case "ArrowLeft":
      case "ArrowRight":
      case "Home":
      case "End":
      case "PageUp":
      case "PageDown":
        dismiss(); // caret left the word; let the key through
        return false;
      default:
        return false;
    }
  }

  return {
    onKeydown,
    dismiss,
    registerProvider(fn) {
      providers.push(fn);
    },
  };
}
