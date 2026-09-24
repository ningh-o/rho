// Bundle CodeMirror 6 + the shared rho editor core (site/assets/
// rho-editor.js + its completion tables) into site/assets/codemirror.bundle.js
// — one committed ES module the playground imports, so the no-build site
// still gets a real editor. Run: node tools/bundle-codemirror.mjs
import esbuild from "esbuild";
import { writeFileSync } from "node:fs";

const result = await esbuild.build({
  entryPoints: ["site/assets/editor-entry.js"],
  bundle: true,
  format: "esm",
  minify: process.env.MINIFY === '1',
  write: false,
  legalComments: "none",
});
writeFileSync("site/assets/codemirror.bundle.js", result.outputFiles[0].text);
console.log(`codemirror.bundle.js: ${result.outputFiles[0].text.length} bytes`);
