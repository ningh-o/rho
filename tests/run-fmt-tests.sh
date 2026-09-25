#!/bin/zsh
# fmt canonical roundtrip: fmt output re-parses and re-fmts byte-identical
set -u
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/emit/*.rho; do
  name=$(basename "$f")
  if "$RHO" fmt "$f" >/tmp/fmt-a.txt 2>/dev/null && \
     "$RHO" fmt /tmp/fmt-a.txt >/tmp/fmt-b.txt 2>/dev/null && \
     diff -q /tmp/fmt-a.txt /tmp/fmt-b.txt >/dev/null; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $name"
  fi
done
echo "fmt tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
