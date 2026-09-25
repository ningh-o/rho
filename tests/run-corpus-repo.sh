#!/bin/zsh
# The behavioral corpus: build + run each corpus/*.rho through the
# compiler under test; compare stdout byte-for-byte and the exit code
# against the recorded golden (regenerated from the seed; never
# hand-written — spec D1 keeps them stable).
set -u
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in corpus/*.rho; do
  name=$(basename "$f" .rho)
  want_exit=$(sed -n 's/^\/\/ exit: //p' "$f" | head -1)
  [ -z "$want_exit" ] && want_exit=0
  want_out=""
  [ -f "corpus/$name.out" ] && want_out=$(cat "corpus/$name.out")
  # per-program build parameters: '// set: name=value' header lines
  sets=()
  for s in $(sed -n 's/^\/\/ set: //p' "$f"); do
    sets+=(--set "$s")
  done
  if ! "$RHO" check "$f" ${sets:+${sets[@]}} >/dev/null 2>&1; then
    fail=$((fail+1)); echo "FAIL $name: compile error"
    continue
  fi
  got=$("$RHO" run "$f" ${sets:+${sets[@]}} 2>/dev/null)
  rc=$?
  if [ "$rc" -eq "$want_exit" ] && [ "$got" = "$want_out" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name: exit=$rc want=$want_exit"
  fi
done
echo "corpus: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
