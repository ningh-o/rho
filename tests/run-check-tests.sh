#!/bin/zsh
# Diagnostic and positive check tests. Each tests/check/*.rho carries
# one or more `// expect: <substring>` lines: a positive test has none
# and must check clean; a diagnostic test must fail with every listed
# substring present in the output. Time-capped by the caller (gate).
set -u
RHO=${RHO:-./build/rho}
pass=0
fail=0
for f in tests/check/*.rho; do
  name=$(basename "$f")
  out=$("$RHO" check "$f" 2>&1)
  code=$?
  nexp=$(grep -c '^// expect: ' "$f")
  if [ "$nexp" -eq 0 ]; then
    if [ "$code" -eq 0 ]; then
      pass=$((pass+1))
    else
      fail=$((fail+1))
      echo "FAIL $name (should check clean):"
      echo "$out" | head -3
    fi
    continue
  fi
  if [ "$code" -eq 0 ]; then
    fail=$((fail+1))
    echo "FAIL $name (should have failed)"
    continue
  fi
  ok=1
  while IFS= read -r e; do
    if ! printf '%s\n' "$out" | grep -qF "$e"; then
      ok=0
      fail=$((fail+1))
      echo "FAIL $name: missing '$e'"
    fi
  done < <(sed -n 's/^\/\/ expect: //p' "$f")
  [ "$ok" -eq 1 ] && pass=$((pass+1))
done
echo "check tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
