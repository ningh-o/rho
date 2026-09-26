#!/bin/zsh
# fmt canonical roundtrip: fmt output re-parses and re-fmts byte-identical,
# and fmt never drops a comment — the input's comment texts all appear in
# the output, count-matched (fmt may move a comment, never edit or lose it)
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/suites/emit/*_test.rho; do
  name=$(basename "$f")
  if "$RHO" fmt "$f" >$T/fmt-a.txt 2>/dev/null && \
     "$RHO" fmt $T/fmt-a.txt >$T/fmt-b.txt 2>/dev/null && \
     diff -q $T/fmt-a.txt $T/fmt-b.txt >/dev/null && \
     [ "$(grep -o '//.*' "$f" | sort | md5)" = "$(grep -o '//.*' $T/fmt-a.txt | sort | md5)" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $name"
  fi
done
echo "fmt tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
