#!/bin/zsh
# Behavioral emit tests: build + run through wasmtime, compare stdout.
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/emit/*.rho; do
  name=$(basename "$f")
  sed -n 's/^\/\/ out: //p' "$f" > $T/emit-want.txt
  if "$RHO" build "$f" -o $T/emit-test.wasm >$T/emit-build.log 2>&1 && \
     wasmtime run $T/emit-test.wasm >$T/emit-got.txt 2>/dev/null && \
     diff -q $T/emit-want.txt $T/emit-got.txt >/dev/null; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name:"
    diff $T/emit-want.txt $T/emit-got.txt | head -4
    head -3 $T/emit-build.log
  fi
done
echo "emit tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
