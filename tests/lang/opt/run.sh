#!/bin/sh
# tests/lang/opt — the optimizer suite. MIRROR-ONLY by design: the
# optimizer lives in libs/compiler/opt.rho and boot has none, so every
# leg runs the self-built compiler (build/gate/m.wasm — the gate's name
# for the same artifact the site ships).
#
#   first line `// exit: N` — the mirror builds the program (wasm32-wasi),
#                             wasmtime runs it, stdout must equal <name>.out
#                             and the exit code must be N (behavior is
#                             invariant under folding — the same program
#                             compiled by boot produces this golden)
#   <name>.fold               — optional: markers that MUST appear in
#                             main's --dump-ir section after optimization
#                             (the fold happened and survived DCE; markers
#                             are final values — intermediate constants
#                             are consumed and dropped)
#   <name>.no                 — optional: grep -E patterns that must NOT
#                             appear in main's dump section (e.g. '^  cmp '
#                             when every comparison folded away)
#
# Usage: sh tests/lang/opt/run.sh   (from anywhere; builds the mirror if
# build/gate/m.wasm is missing)
set -u
cd "$(dirname "$0")/../../.."

B=build/rho-boot
M=${MIRROR:-build/gate/m.wasm}
if [ ! -f "$B" ]; then
  echo "opt: no boot compiler at $B (make build/rho-boot first)"
  exit 2
fi
if [ ! -f "$M" ]; then
  echo "opt: no mirror at $M — building it"
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' 60 \
    "$B" build libs/compiler/main.rho --target wasm32-wasi -o "$M" || {
    echo "opt: mirror build failed"
    exit 2
  }
fi

G=build/gate/opt
mkdir -p "$G"

# every invocation under a wall-clock cap (macOS: perl alarm survives exec)
wr() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$@"
}

strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

fails=0
total=0
for src in tests/lang/opt/s*.rho; do
  name=$(basename "$src" .rho)
  total=$((total + 1))
  verdict=""
  case $(head -1 "$src") in
  "// exit:"*) ;;
  *)
    verdict="FAIL (no // exit marker)"
    ;;
  esac
  if [ -z "$verdict" ]; then
    want_exit=$(head -1 "$src" | sed 's|// exit: ||')
    rm -f "$G/$name.wasm" "$G/$name.got"
    if ! wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src" \
        --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (mirror build)"
    else
      wr 10 wasmtime run "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
      if ! cmp -s "$G/$name.got" "tests/lang/opt/$name.out"; then
        verdict="FAIL (output)"
      fi
    fi
  fi
  # the run's exit code: rerun cheaply (wasmtime cached parse; a second
  # run is the honest way to read rc without contorting the pipeline)
  if [ -z "$verdict" ] && [ -f "$G/$name.wasm" ]; then
    wr 10 wasmtime run "$G/$name.wasm" >/dev/null 2>&1
    got_rc=$?
    if [ "$got_rc" != "$want_exit" ]; then
      verdict="FAIL (exit $got_rc, want $want_exit)"
    fi
  fi
  # fold markers / absence pins both read main's --dump-ir section;
  # regenerate it fresh every run (a cached mainir would grade the
  # previous mirror)
  if [ -z "$verdict" ] && { [ -f "tests/lang/opt/$name.fold" ] || [ -f "tests/lang/opt/$name.no" ]; }; then
    sym="rho_$(printf '%s' "$src" | tr '/.' '__')"
    wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src" \
      --target wasm32-wasi -o "$G/$name.wasm" --dump-ir 2>"$G/$name.ir" >/dev/null
    awk -v fn="$sym" 'BEGIN{p=0} /^fn /{p=0} index($0, fn){p=1} p' "$G/$name.ir" | strip_dbg > "$G/$name.mainir"
  fi
  # fold markers: main's dump section must contain each marker line
  if [ -z "$verdict" ] && [ -f "tests/lang/opt/$name.fold" ]; then
    while IFS= read -r marker; do
      [ -z "$marker" ] && continue
      if ! grep -qF "$marker" "$G/$name.mainir"; then
        verdict="FAIL (no fold marker: $marker)"
        break
      fi
    done < "tests/lang/opt/$name.fold"
  fi
  # absence pins: patterns that must NOT appear in main's dump section
  if [ -z "$verdict" ] && [ -f "tests/lang/opt/$name.no" ]; then
    while IFS= read -r pat; do
      [ -z "$pat" ] && continue
      if grep -qE "$pat" "$G/$name.mainir"; then
        verdict="FAIL (pattern survived: $pat)"
        break
      fi
    done < "tests/lang/opt/$name.no"
  fi
  if [ -n "$verdict" ]; then
    echo "$name: $verdict"
    fails=$((fails + 1))
  fi
done

echo "opt: $((total - fails))/$total ok"
[ "$fails" = 0 ]
