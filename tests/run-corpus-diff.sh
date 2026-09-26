#!/bin/zsh
# the corpus differential (T3.1's second leg): every corpus program
# through boot AND through the self-hosted chain; behavior (stdout +
# exit) must match. The pass count is pinned — it may only grow; the
# closing of this leg IS the corpus differential going green.
#
# Every file that fails its first pass re-runs ONCE (bounded): a
# hundred wasmtime spawns per run sit atop macOS's memory-pressure
# cliff, and an environment-blip (a short read under load, a busy
# rename) must never paint a green leg red. A REAL regression fails
# both times; the pin keeps its teeth.
set -u
RHO=${RHO:-./build/rho}
PINNED=108

# one file's differential; echoes "pass" or the failure label
check_one() {
  local f=$1
  local name src sets s kn vv mods mf
  name=$(basename "$f" .rho)
  sets=()
  src=$(cat "$f")
  # the module tree rides MODS for the self-host side (boot reads the
  # same tree from disk): every rho file under the corpus's packages
  mods=""
  for mf in corpus/geom/*.rho(N) corpus/geom/*/*.rho(N) corpus/web/*.rho(N) corpus/pk/*.rho(N) corpus/pk/*/*.rho(N) corpus/pk/*/*/*.rho(N); do
    if [ -f "$mf" ]; then
      mods="$mods@MOD@ ${mf#corpus/}
$(cat "$mf")
"
    fi
  done
  for s in $(sed -n 's/^\/\/ set: //p' "$f"); do
    sets+=(--set "$s")
    kn=${s%%=*}
    vv=${s#*=}
    src=$(printf '%s\n' "$src" | sed -E \
      "s/^(const +${kn} *: *[A-Za-z0-9?*]+ *= *).*/\1${vv};/")
  done
  bgot=$("$RHO" run "$f" 2>/dev/null ${sets:+${sets[@]}}); brc=$?
  if ! "$RHO" build libs/compiler/main.rho -o /tmp/cdiff-c-$$.wasm \
      --set "SRC=$src" --set "MODS=$mods" >/dev/null 2>&1; then
    echo "build"; return
  fi
  wasmtime /tmp/cdiff-c-$$.wasm >/tmp/cdiff-$$.wat 2>/dev/null
  local cr=$?
  rm -f /tmp/cdiff-c-$$.wasm
  if [ $cr -eq 1 ]; then
    # a clean refusal (diagnostics on stderr, exit 1) — a missing
    # feature, not a wrong behavior; falling through would compare
    # boot's output against an EMPTY wasm and mislabel it :diff
    echo "refuse"; return
  fi
  if [ $cr -gt 1 ]; then
    # the compiler itself died mid-run — worse than any behavioral
    # diff; the robustness bar is clean refusal or clean compile
    echo "PANIC"; return
  fi
  if ! wat2wasm /tmp/cdiff-$$.wat -o /tmp/cdiff-$$.self.wasm 2>/dev/null; then
    echo "w2w"; return
  fi
  rm -f /tmp/cdiff-$$.wat
  local sgot src_rc
  sgot=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime \
    /tmp/cdiff-$$.self.wasm 2>/dev/null); src_rc=$?
  rm -f /tmp/cdiff-$$.self.wasm
  if [ "$brc" -eq "$src_rc" ] && [ "$bgot" = "$sgot" ]; then
    echo "pass"; return
  fi
  echo "diff"; return
}

pass=0; fail=0; failed=""
for f in corpus/*.rho; do
  name=$(basename "$f" .rho)
  r=$(check_one "$f")
  if [ "$r" != "pass" ]; then
    # the bounded re-check: environment blips fail once, regressions
    # fail twice
    r=$(check_one "$f")
  fi
  if [ "$r" = "pass" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed="$failed $name:$r"
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
