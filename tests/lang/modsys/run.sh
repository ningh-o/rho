#!/bin/sh
# tests/lang/modsys — the module-system suite: `pub static mut`,
# module-qualified enum variants, and the use-path root fallback.
#
# These are BOOT features (the mirror may only use what boot implements),
# so everything here runs on the boot compiler itself, not the mirror:
#
#   first line `// exit: N` — build with boot, run under wasmtime, stdout
#                            must equal main.out and the exit code must be N
#   first line `// diag`    — `boot check` diagnostics must equal main.out
#
# One directory per test: main.rho is the entry (the root module), the
# siblings are its modules.
#
# Usage: sh tests/lang/modsys/run.sh
set -u
cd "$(dirname "$0")/../../.."

B=${BOOT:-build/rho-boot}
if [ ! -f "$B" ]; then
  echo "modsys: no boot compiler at $B (make build/rho-boot first)"
  exit 2
fi

G=build/gate/modsys
mkdir -p "$G"

# every invocation under a wall-clock cap (macOS: perl alarm survives exec)
wr() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$@"
}

# strip the temporary HEAP@/WE debug prints — the gate's strip_dbg law
strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

fails=0
total=0
for dir in tests/lang/modsys/*/; do
  [ -f "$dir/main.rho" ] || continue
  dir=${dir%/}
  name=$(basename "$dir")
  total=$((total + 1))
  verdict=""
  case $(head -1 "$dir/main.rho") in
  "// exit:"*)
    want_exit=$(head -1 "$dir/main.rho" | sed 's|// exit: ||')
    rm -f "$G/$name.wasm" "$G/$name.got"
    if ! wr 60 "$B" build "$dir/main.rho" --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (build)"
    else
      wr 10 wasmtime run --dir . "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
      got_exit=$?
      if ! cmp -s "$G/$name.got" "$dir/main.out"; then
        verdict="FAIL (output)"
      fi
    fi
    ;;
  "// diag")
    rm -f "$G/$name.got"
    wr 60 "$B" check "$dir/main.rho" 2>&1 >/dev/null | strip_dbg > "$G/$name.got"
    if ! cmp -s "$G/$name.got" "$dir/main.out"; then
      verdict="FAIL (diag)"
    fi
    ;;
  *)
    verdict="FAIL (no // exit or // diag marker)"
    ;;
  esac
  if [ -n "$verdict" ]; then
    echo "$name: $verdict"
    fails=$((fails + 1))
  fi
done

echo "modsys: $((total - fails))/$total ok"
[ "$fails" = 0 ]
