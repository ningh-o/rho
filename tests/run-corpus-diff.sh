#!/bin/zsh
# the corpus differential (T3.1's second leg): every corpus program
# through boot AND through the self-hosted chain; behavior (stdout +
# exit) must match. The pass count is pinned — it may only grow; the
# closing of this leg IS the corpus differential going green.
set -u
RHO=${RHO:-./build/rho}
PINNED=63
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
  cr=$?
  if [ $cr -eq 1 ]; then
    # a clean refusal (diagnostics on stderr, exit 1) — a missing
    # feature, not a wrong behavior; falling through would compare
    # boot's output against an EMPTY wasm and mislabel it :diff
    fail=$((fail+1)); failed="$failed ${name}:refuse"; continue
  fi
  if [ $cr -gt 1 ]; then
    # the compiler itself died mid-run — worse than any behavioral
    # diff; the robustness bar is clean refusal or clean compile
    fail=$((fail+1)); failed="$failed $name:PANIC"; continue
  fi
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
