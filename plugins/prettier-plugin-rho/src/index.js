// index.js — the prettier plugin for rho.
//
// Thin client by law (docs/ecosystem.md §3): never a second formatter.
// The parser runs the embedded fmt wasm and the printer hands its
// canonical text to prettier verbatim — one formatting truth, the
// compiler's own (assets/fmt.wasm, generation pinned in
// assets/generation.json and in package.json's description).

import { formatRho } from "./fmt.js";

const plugin = {
  languages: [
    {
      name: "rho",
      parsers: ["rho"],
      extensions: [".rho"],
      tmScope: "source.rho",
    },
  ],
  parsers: {
    rho: {
      // the "AST" is the canonical text itself: the plugin does not
      // interpret the program, the compiler's fmt already did
      parse: async (text) => formatRho(text),
      astFormat: "rho",
      hasComments: false,
      locStart: () => 0,
      locEnd: () => 0,
    },
  },
  printers: {
    rho: {
      print: (path) => path.node,
    },
  },
};

export default plugin;
