#!/bin/sh
# The corpus, one law: every program's stdout + exit code matches its
# committed .out golden and its first-line `// exit: N`.
#
# Two builder lanes, by the two-layer feature law:
#   - boot-era programs build with boot (the frozen oracle);
#   - seed-era programs (boot cannot parse them — the dot-round corpus,
#     e.g. 032) build with the self-built chain's web root
#     (build/rho.wasm), whose behavior the gate's corpus-diff leg
#     re-proves against boot on every boot-era entry.
#
# The glob is the top level only: a corpus program's module files live in
# subdirectories (corpus/geom/...) and are not programs themselves.
#
# Usage: sh tools/corpus-run.sh
set -u
cd "$(dirname "$0")/.."

B=${BOOT:-build/rho-boot}
SELF=${SELF_WASM:-build/rho.wasm}
[ -x "$B" ] || { echo "corpus-run: no boot compiler at $B (make build/rho-boot)"; exit 2; }
[ -f "$SELF" ] || { echo "corpus-run: no self-built wasm at $SELF (make build/rho.wasm)"; exit 2; }

wt() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$1" \
    wasmtime run --dir . "${@:2}"
}

strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

G=build/gate/corpus-run
mkdir -p "$G"

fails=0
total=0
for src in corpus/*.rho; do
  name=$(basename "$src" .rho)
  total=$((total + 1))
  want_rc=$(sed -n 's|^// exit: ||p' "$src" | head -1)
  [ -n "$want_rc" ] || want_rc=0
  # a missing .out is an empty golden: those programs print nothing
  want="$G/$name.want"
  : > "$want"
  [ -f "corpus/$name.out" ] && cat "corpus/$name.out" > "$want"
  bin="$G/$name.wasm"
  rm -f "$bin" "$G/$name.got" "$G/$name.raw"
  runner=boot
  if perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' 10 \
       "$B" build "$src" --target wasm32-wasi -o "$bin" >/dev/null 2>&1 && [ -f "$bin" ]; then
    :
  else
    rm -f "$bin"
    runner=self
    wt 120 "$SELF" build "$src" --target wasm32-wasi -o "$bin" >/dev/null 2>&1
  fi
  if [ ! -f "$bin" ]; then
    echo "FAIL $name (build, $runner)"
    fails=$((fails + 1))
    continue
  fi
  wt 10 "$bin" 2>/dev/null > "$G/$name.raw"
  rc=$?
  strip_dbg < "$G/$name.raw" > "$G/$name.got"
  if ! cmp -s "$G/$name.got" "$want"; then
    echo "FAIL $name (stdout differs, $runner)"
    fails=$((fails + 1))
    continue
  fi
  if [ "$rc" != "$want_rc" ]; then
    echo "FAIL $name (exit $rc, want $want_rc, $runner)"
    fails=$((fails + 1))
    continue
  fi
done

if [ "$fails" = 0 ]; then
  echo "corpus-run: $total ok"
  exit 0
fi
echo "corpus-run: $fails/$total failed"
exit 1
