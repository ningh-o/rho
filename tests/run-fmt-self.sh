#!/bin/zsh
# fmt parity: the self-hosted formatter must be byte-identical to
# boot's on the subset grammar (D1: fmt output is stdout — behavior,
# so the bytes match across compilers), and its own output must be a
# fixpoint (canonical roundtrip).
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RHO=${RHO:-./build/rho}
pass=0; fail=0
for f in tests/fmt-self/*.rho; do
  name=$(basename "$f")
  src=$(cat "$f")
  # boot's canonical form
  "$RHO" fmt "$f" >$T/fmtb-$name.txt 2>/dev/null
  # the self-hosted formatter (SRC rides the build parameter)
  if ! "$RHO" build libs/compiler/main.rho -o $T/fmtc-$name.wasm \
      --set "SRC=$src" --set FMT=1 >$T/fmtc-$name.log 2>&1; then
    echo "FAIL fmt-self/$name: boot could not build the compiler"
    head -3 $T/fmtc-$name.log
    fail=$((fail+1))
    continue
  fi
  wasmtime $T/fmtc-$name.wasm >$T/fmts-$name.txt 2>/dev/null
  if ! cmp -s $T/fmtb-$name.txt $T/fmts-$name.txt; then
    echo "FAIL fmt-self/$name: boot and self-hosted fmt differ"
    diff $T/fmtb-$name.txt $T/fmts-$name.txt | head -6
    fail=$((fail+1))
    continue
  fi
  # the self form is canonical: re-formatting it reproduces it
  cp $T/fmts-$name.txt $T/fmts-$name.rho
  src2=$(cat $T/fmts-$name.rho)
  "$RHO" build libs/compiler/main.rho -o $T/fmtc2-$name.wasm \
    --set "SRC=$src2" --set FMT=1 >/dev/null 2>&1
  wasmtime $T/fmtc2-$name.wasm >$T/fmts2-$name.txt 2>/dev/null
  if ! cmp -s $T/fmts-$name.txt $T/fmts2-$name.txt; then
    echo "FAIL fmt-self/$name: the self form is not a fixpoint"
    diff $T/fmts-$name.txt $T/fmts2-$name.txt | head -6
    fail=$((fail+1))
    continue
  fi
  pass=$((pass+1))
done
echo "fmt-self tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
