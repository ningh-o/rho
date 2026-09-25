#!/bin/zsh
# the corpus differential (T3.1's second leg): every corpus program
# through boot AND through the self-hosted chain; behavior (stdout +
# exit) must match. The pass count is pinned — it may only grow; the
# closing of this leg IS the corpus differential going green.
set -u
RHO=${RHO:-./build/rho}
PINNED=37
pass=0; fail=0; failed=""
for f in corpus/*.rho; do
  name=$(basename "$f" .rho)
  bgot=$("$RHO" run "$f" 2>/dev/null); brc=$?
  src=$(cat "$f")
  if ! "$RHO" build libs/compiler/main.rho -o /tmp/cdiff-c.wasm \
      --set "SRC=$src" >/dev/null 2>&1; then
    fail=$((fail+1)); failed="$failed $name:build"; continue
  fi
  wasmtime /tmp/cdiff-c.wasm >/tmp/cdiff.wat 2>/dev/null
  if ! wat2wasm /tmp/cdiff.wat -o /tmp/cdiff.self.wasm 2>/dev/null; then
    fail=$((fail+1)); failed="$failed $name:w2w"; continue
  fi
  sgot=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime \
    /tmp/cdiff.self.wasm 2>/dev/null); src_rc=$?
  if [ "$brc" -eq "$src_rc" ] && [ "$bgot" = "$sgot" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed="$failed $name:diff"
  fi
done
echo "corpus differential: $pass pass, $fail fail (pinned floor: $PINNED)"
if [ "$pass" -lt "$PINNED" ]; then
  echo "REGRESSION below the pinned floor"
  echo "failed:$failed"
  exit 1
fi
if [ "$fail" -gt 0 ]; then
  # the remaining surface, listed but not fatal — the leg closes when
  # this list empties
  echo "remaining:$failed"
fi
