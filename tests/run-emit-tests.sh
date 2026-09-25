#!/bin/zsh
# Behavioral emit tests: build + run through wasmtime, compare stdout.
set -u
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/emit/*.rho; do
  name=$(basename "$f")
  want=$(sed -n 's/^\/\/ out: //p' "$f")
  out=$("$RHO" build "$f" -o /tmp/emit-test.wasm 2>&1 && wasmtime run /tmp/emit-test.wasm 2>/dev/null)
  if [ "$out" = "$want" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name: got '$out' want '$want'"
  fi
done
echo "emit tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
