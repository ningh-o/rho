#!/bin/zsh
# build-site.sh — assemble the site's deploy assets from the repo:
#   1. the compiler: the self-hosted mirror in its app configuration
#      (build-app-artifact.sh — MODS_APP bakes the std tree, the smoke
#      probe runs a std.io greet through wasmtime), then wasm-opt -Oz
#      (the ledgered site-asset shrinker; bulk-memory required) with a
#      post-opt smoke probe of its own
#   2. the spec copies: spec/*.md → site/spec/ (the reader serves them)
# Run before committing site changes; site/ is committed whole (the
# Pages workflow uploads it as-is, no CI build step).
set -eu
cd "$(dirname "$0")/.."
OUT=${1:-site/assets/rho.wasm}

echo "== building the compiler artifact (mirror, app face)"
zsh tools/build-app-artifact.sh build/site-rho.wasm

echo "== shrinking with wasm-opt -Oz"
# multivalue: the emitter's pair lanes produce multi-result blocks —
# the ledger's shrinker flag rides beside its bulk-memory one
wasm-opt -Oz --enable-bulk-memory --enable-multivalue build/site-rho.wasm -o "$OUT"
echo "   $(wc -c < build/site-rho.wasm) -> $(wc -c < "$OUT") bytes"

echo "== smoke probe on the OPTIMIZED artifact"
rm -rf build/site-smoke && mkdir -p build/site-smoke
cat > build/site-smoke/main.rho <<'EOF'
use std.io;
fn main() -> i32 {
  let got = io.read_line();
  let name: string = match got {
    Result.Ok(inner) => match inner {
      Option.Some(s) => s,
      Option.None => "",
    },
    Result.Err(_) => "",
  };
  printf("hi, {}\n", name);
  return 0;
}
EOF
cat > build/site-smoke/mode <<'EOF'
build
EOF
(cd build/site-smoke && wasmtime run --dir . ../../site/assets/rho.wasm > smoke.wat 2>/dev/null)
wat2wasm build/site-smoke/smoke.wat -o build/site-smoke/smoke.wasm
echo "Ada" | wasmtime build/site-smoke/smoke.wasm | grep -q "hi, Ada" \
  || { echo "SMOKE RED: the optimized artifact misbehaves"; exit 1; }
echo "   optimized artifact runs (greet probe green)"
rm -rf build/site-smoke build/site-rho.wasm

echo "== copying the spec"
cp spec/spec.md spec/syntax.md spec/type-system.md spec/module-system.md site/spec/
echo "site assets ready"
