#!/bin/sh
# tests/lang/eq — element-wise aggregate equality (spec 4.2), MIRROR-ONLY:
# boot is frozen before this feature, so every leg runs the self-built
# compiler (build/gate/m.wasm — the gate's name for the same artifact the
# site ships). Language evolution lives in the self-hosted compiler only.
#
#   first line `// exit: N`     — the mirror builds the program
#                               (wasm32-wasi), wasmtime runs it, stdout
#                               must equal <name>.out and the exit code
#                               must be N
#   first line `// reject: S`   — the build must FAIL with S in its
#                               diagnostics (comparability gate)
#
# Usage: sh tests/lang/eq/run.sh   (from anywhere; builds the mirror if
# build/gate/m.wasm is missing)
set -u
cd "$(dirname "$0")/../../.."

B=build/rho-boot
M=${MIRROR:-build/gate/m.wasm}
if [ ! -f "$B" ]; then
  echo "eq: no boot compiler at $B (make build/rho-boot first)"
  exit 2
fi
if [ ! -f "$M" ]; then
  echo "eq: no mirror at $M — building it"
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' 60 \
    "$B" build libs/compiler/full.rho --target wasm32-wasi -o "$M" || {
    echo "eq: mirror build failed"
    exit 2
  }
fi

G=build/gate/eq
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
for src in tests/lang/eq/e*.rho; do
  name=$(basename "$src" .rho)
  total=$((total + 1))
  verdict=""
  case $(head -1 "$src") in
  "// exit:"*) ;;
  "// reject:"*) ;;
  *)
    verdict="FAIL (no // exit or // reject marker)"
    ;;
  esac
  if [ -z "$verdict" ]; then
    case $(head -1 "$src") in
    "// reject:"*)
      want=$(head -1 "$src" | sed 's|// reject: ||')
      rm -f "$G/$name.err"
      if wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src" \
          --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>"$G/$name.err"; then
        verdict="FAIL (accepted a program it must reject)"
      elif ! grep -qF "$want" "$G/$name.err"; then
        verdict="FAIL (diagnostic missing: $want)"
        sed -n '1,4p' "$G/$name.err"
      fi
      ;;
    *)
      want_exit=$(head -1 "$src" | sed 's|// exit: ||')
      rm -f "$G/$name.wasm" "$G/$name.got"
      if ! wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src" \
          --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.wasm" ]; then
        verdict="FAIL (mirror build)"
      else
        wr 10 wasmtime run "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
        if ! cmp -s "$G/$name.got" "tests/lang/eq/$name.out"; then
          verdict="FAIL (output)"
          diff "tests/lang/eq/$name.out" "$G/$name.got" | head -4
        fi
      fi
      if [ -z "$verdict" ] && [ -f "$G/$name.wasm" ]; then
        wr 10 wasmtime run "$G/$name.wasm" >/dev/null 2>&1
        got_rc=$?
        if [ "$got_rc" != "$want_exit" ]; then
          verdict="FAIL (exit $got_rc, want $want_exit)"
        fi
      fi
      ;;
    esac
  fi
  if [ -n "$verdict" ]; then
    echo "$name: $verdict"
    fails=$((fails + 1))
  fi
done

echo "eq: $((total - fails))/$total ok"
[ "$fails" = 0 ]
