import { defineConfig } from 'vite';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { rhoPlugin } from 'vite-plugin-rho';

// The rho compiler as a wasm32-wasi module. Build it once from the rho repo:
//   make build/rho.wasm
// This example points at the sibling checkout's build output.
const compiler = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '../../../build/rho.wasm',
);

export default defineConfig({
  plugins: [rhoPlugin({ compiler })],
  build: {
    target: 'es2022', // top-level await in the generated companions
  },
});
