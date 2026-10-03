// index.js — prettier-plugin-rho: prettier support for the rho language.
//
// A thin client, never a second formatter: the plugin embeds the rho
// compiler's fmt wasm (one pinned generation — assets/, pinned by
// assets/generation.json) and hands prettier the canonical text
// verbatim. The parser runs the compiler's `rho fmt` face over the
// source; the printer returns that text unchanged — prettier's doc
// machinery never re-lays-out a byte of it.
//
// The same wasm fmt serves the site playground and (planned) the LSP —
// one formatting truth, many clients (docs/ecosystem.md §3).
//
// Formatter scope: the embedded fmt is the repo's subset-grammar
// formatter — the scope tests/run-fmt-self.sh pins it on. See README
// "Formatter scope" before formatting programs outside that scope.

import { fmtRho } from "./runtime.js";

const RHO_PARSER = {
  parse(text) {
    const result = fmtRho(text);
    if (!result.ok) {
      const detail = result.stderr || `rho fmt exited with code ${result.exitCode}`;
      const error = new Error(detail);
      // prettier reads these for parse-error locations
      error.locStart = 0;
      error.locEnd = text.length;
      throw error;
    }
    // the AST is a wrapper around the canonical text; the printer
    // emits it verbatim (a printer must never re-format the
    // compiler's canonical form — not even prettier)
    return { type: "RhoCanonical", value: result.text };
  },
  astFormat: "rho",
  locStart: () => 0,
  locEnd: (node) => (node && typeof node.value === "string" ? node.value.length : 0),
};

const RHO_PRINTER = {
  print(path) {
    return path.node.value;
  },
  // the printer will prepend/append nothing: the canonical text
  // already carries its own comments and trailing newline
};

export const languages = [
  {
    name: "rho",
    parsers: ["rho"],
    extensions: [".rho"],
    linguistLanguageId: null,
  },
];

export const parsers = {
  rho: RHO_PARSER,
};

export const printers = {
  rho: RHO_PRINTER,
};

export const options = {};
export const defaultOptions = {};

export default { languages, parsers, printers, options, defaultOptions };
