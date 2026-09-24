// Bundle entry for the playground's editor: CodeMirror 6 plus the shared
// rho editor core (one source in this submodule — the app imports the same
// file). Re-exported so playground.js stays framework-free.
export { createRhoEditor } from "./rho-editor.js";
