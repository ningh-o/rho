#!/bin/zsh
# fmt parity: the self-hosted formatter must be byte-identical to
# boot's on the subset grammar (D1: fmt output is stdout — behavior,
# so the bytes match across compilers), and its own output must be a
# fixpoint (canonical roundtrip).
set -u
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/fmt-self/*.rho; do
  name=$(basename "$f")
  src=$(cat "$f")
  # boot's canonical form
  "$RHO" fmt "$f" >/tmp/fmtb-$name.txt 2>/dev/null
  # the self-hosted formatter (SRC rides the build parameter)
  if ! "$RHO" build libs/compiler/main.rho -o /tmp/fmtc-$name.wasm \
      --set "SRC=$src" --set FMT=1 >/tmp/fmtc-$name.log 2>&1; then
    echo "FAIL fmt-self/$name: boot could not build the compiler"
    head -3 /tmp/fmtc-$name.log
    fail=$((fail+1))
    continue
  fi
  wasmtime /tmp/fmtc-$name.wasm >/tmp/fmts-$name.txt 2>/dev/null
  if ! cmp -s /tmp/fmtb-$name.txt /tmp/fmts-$name.txt; then
    echo "FAIL fmt-self/$name: boot and self-hosted fmt differ"
    diff /tmp/fmtb-$name.txt /tmp/fmts-$name.txt | head -6
    fail=$((fail+1))
    continue
  fi
  # the self form is canonical: re-formatting it reproduces it
  cp /tmp/fmts-$name.txt /tmp/fmts-$name.rho
  src2=$(cat /tmp/fmts-$name.rho)
  "$RHO" build libs/compiler/main.rho -o /tmp/fmtc2-$name.wasm \
    --set "SRC=$src2" --set FMT=1 >/dev/null 2>&1
  wasmtime /tmp/fmtc2-$name.wasm >/tmp/fmts2-$name.txt 2>/dev/null
  if ! cmp -s /tmp/fmts-$name.txt /tmp/fmts2-$name.txt; then
    echo "FAIL fmt-self/$name: the self form is not a fixpoint"
    diff /tmp/fmts-$name.txt /tmp/fmts2-$name.txt | head -6
    fail=$((fail+1))
    continue
  fi
  pass=$((pass+1))
done
echo "fmt-self tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
