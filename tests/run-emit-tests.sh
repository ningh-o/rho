#!/bin/zsh
# Behavioral emit tests: build + run through wasmtime, compare stdout.
set -u
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/emit/*.rho; do
  name=$(basename "$f")
  sed -n 's/^\/\/ out: //p' "$f" > /tmp/emit-want.txt
  if "$RHO" build "$f" -o /tmp/emit-test.wasm >/tmp/emit-build.log 2>&1 && \
     wasmtime run /tmp/emit-test.wasm >/tmp/emit-got.txt 2>/dev/null && \
     diff -q /tmp/emit-want.txt /tmp/emit-got.txt >/dev/null; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name:"
    diff /tmp/emit-want.txt /tmp/emit-got.txt | head -4
    head -3 /tmp/emit-build.log
  fi
done
echo "emit tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
