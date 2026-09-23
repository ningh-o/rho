#!/bin/sh
# tests/lang/multiline — the multiline string literal suite.
#
# These goldens live outside corpus/ on purpose: corpus is frozen at the
# boot-consistent subset, and the frozen boot oracle cannot lex triple
# quotes. Everything here runs on the self-hosted chain (the mirror,
# build/gate/m.wasm) — the same comparison law as the gate, one directory:
#
#   first line `// exit: N` — build with the mirror, run, stdout must equal
#                            the .out; then fmt the source and require the
#                            formatted form to build, run, and print the
#                            same bytes (the fmt roundtrip law), and fmt to
#                            be idempotent
#   first line `// diag`    — `rho check` diagnostics must equal the .out
#
# Usage: sh tests/lang/multiline/run.sh
set -u
cd "$(dirname "$0")/../../.."

R=${RHO:-build/gate/m.wasm}
if [ ! -f "$R" ]; then
  echo "multiline: no mirror at $R (rebuild it first)"
  exit 2
fi

G=build/gate/multiline
mkdir -p "$G"

# every rho invocation under a wall-clock cap (macOS: perl alarm survives exec)
wr() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$@"
}

# strip the temporary HEAP@/WE debug prints — the gate's strip_dbg law
strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

fails=0
total=0
for src in tests/lang/multiline/m*.rho; do
  name=$(basename "$src" .rho)
  total=$((total+1))
  verdict=""
  case $(head -1 "$src") in
  "// exit:"*)
    rm -f "$G/$name.wasm" "$G/$name.got" "$G/$name.fmt.rho" "$G/$name.fmt.wasm" "$G/$name.fmt.got" "$G/$name.fmt2.rho"
    if ! wr 60 wasmtime run --dir . "$R" build "$src" --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (build)"
    else
      wr 10 wasmtime run --dir . "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
      if ! cmp -s "$G/$name.got" "tests/lang/multiline/$name.out"; then
        verdict="FAIL (output)"
      fi
    fi
    if [ -z "$verdict" ]; then
      # fmt roundtrip: the canonical form must rebuild, rerun, and reprint
      # the same bytes, and formatting must be idempotent
      wr 60 wasmtime run --dir . "$R" fmt "$src" 2>/dev/null > "$G/$name.fmt.rho"
      wr 60 wasmtime run --dir . "$R" fmt "$G/$name.fmt.rho" 2>/dev/null > "$G/$name.fmt2.rho"
      if ! cmp -s "$G/$name.fmt.rho" "$G/$name.fmt2.rho"; then
        verdict="FAIL (fmt not idempotent)"
      elif ! wr 60 wasmtime run --dir . "$R" build "$G/$name.fmt.rho" --target wasm32-wasi -o "$G/$name.fmt.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.fmt.wasm" ]; then
        verdict="FAIL (fmt output does not build)"
      else
        wr 10 wasmtime run --dir . "$G/$name.fmt.wasm" 2>/dev/null | strip_dbg > "$G/$name.fmt.got"
        if ! cmp -s "$G/$name.fmt.got" "tests/lang/multiline/$name.out"; then
          verdict="FAIL (fmt roundtrip changed behavior)"
        fi
      fi
    fi
    ;;
  "// diag")
    wr 60 wasmtime run --dir . "$R" check "$src" 2>&1 >/dev/null | strip_dbg > "$G/$name.got"
    if ! cmp -s "$G/$name.got" "tests/lang/multiline/$name.out"; then
      verdict="FAIL (diag)"
    fi
    ;;
  *)
    verdict="FAIL (no // exit or // diag marker)"
    ;;
  esac
  if [ -n "$verdict" ]; then
    echo "$name: $verdict"
    fails=$((fails+1))
  fi
done

echo "multiline: $((total-fails))/$total ok"
[ "$fails" = 0 ]
