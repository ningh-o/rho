#!/bin/zsh
# The byte-golden runner (T6.4): the programs tier's textual goldens are
# // out: headers judged by the suite verb; the goldens that carry
# non-UTF8 bytes cannot ride a text header, so they keep their .out
# file and this runner — stdout compared BYTE-for-BYTE (the file read
# raw against the captured stream, no trailing-newline stripping) and
# the exit code against the // exit: header. Regenerated from the
# seed; never hand-written — spec D1 keeps them stable.
set -u
RHO=${RHO:-./build/rho}
DIR=tests/suites/programs
pass=0; fail=0
for f in $DIR/*.rho(N); do
  name=$(basename "$f" .rho)
  [ -f "$DIR/$name.out" ] || continue # textual golden: the suite verb owns it
  want_exit=$(sed -n 's/^\/\/ exit: //p' "$f" | head -1)
  [ -z "$want_exit" ] && want_exit=0
  # per-program build parameters: '// set: name=value' header lines
  sets=()
  for s in $(sed -n 's/^\/\/ set: //p' "$f"); do
    sets+=(--set "$s")
  done
  if ! "$RHO" check "$f" ${sets:+${sets[@]}} >/dev/null 2>&1; then
    fail=$((fail+1)); echo "FAIL $name: compile error"
    continue
  fi
  "$RHO" run "$f" ${sets:+${sets[@]}} >/tmp/bg-out.$$.bin 2>/dev/null
  rc=$?
  if [ "$rc" -eq "$want_exit" ] && cmp -s /tmp/bg-out.$$.bin "$DIR/$name.out"; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL $name: exit=$rc want=$want_exit"
  fi
done
rm -f /tmp/bg-out.$$.bin
echo "byte goldens: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
