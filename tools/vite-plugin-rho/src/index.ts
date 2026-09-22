// vite-plugin-rho — public API.

export { rhoPlugin, type RhoPluginOptions } from './plugin.js';
export { compileRhoModule, writeDtsSidecar, type CompiledRhoModule, type CompileOptions } from './compile.js';
export { createDriver, RhoCompileError, type CompilerSource, type Driver } from './driver.js';
export { generateDts } from './dts.js';
export { parseSignatures, type RhoSignature } from './bridge.js';
export { parseModule, withExport } from './wasm.js';
