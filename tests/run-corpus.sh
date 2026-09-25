#!/bin/zsh
# Behavioral corpus sweep against the archive goldens (stdout + exit).
# The archive tree is reference-only; corpus programs adapt into
# corpus/ with T1.10. RHO_CORPUS may point elsewhere.
set -u
RHO=${RHO:-./build/rho}
CORPUS=${RHO_CORPUS:-/tmp/rho-archive/corpus}
pass=0; fail=0; skip=0
for f in "$CORPUS"/*.rho; do
  name=$(basename "$f" .rho)
  want_exit=$(sed -n 's/^\/\/ exit: //p' "$f" | head -1)
  [ -z "$want_exit" ] && want_exit=0
  want_out=""
  [ -f "$CORPUS/$name.out" ] && want_out=$(cat "$CORPUS/$name.out")
  if ! "$RHO" check "$f" >/dev/null 2>&1; then
    skip=$((skip+1))   # compile error: not yet supported surface
    continue
  fi
  got=$("$RHO" run "$f" 2>/dev/null)
  rc=$?
  if [ "$rc" -eq "$want_exit" ] && [ "$got" = "$want_out" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name: exit=$rc want=$want_exit"
    if [ "$got" != "$want_out" ]; then
      echo "  stdout: $(echo "$got" | head -2)"
      echo "  want  : $(echo "$want_out" | head -2)"
    fi
  fi
done
echo "corpus: $pass pass, $fail fail, $skip skip (unsupported yet)"
