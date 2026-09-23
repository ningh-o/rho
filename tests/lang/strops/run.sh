#!/bin/sh
# tests/lang/strops — the string `+` suite.
#
# `cat` is retired: `+` concatenates strings (string + string -> string,
# the same precedence/associativity as arithmetic +, no implicit to_str),
# and lowering reuses the prelude's internal __cat2 primitive. Both ends
# implement it (the mirror adopted it 2026-09-23; the gate's corpus-diff
# grades the mirror on these programs). The suite still runs on the boot
# compiler — boot features are pinned at boot:
#
#   first line `// exit: N` — build with boot, run under wasmtime, stdout
#                             must equal <name>.out and the exit code must be N
#   first line `// diag`    — `boot check` diagnostics must equal <name>.out
#
# Usage: sh tests/lang/strops/run.sh
set -u
cd "$(dirname "$0")/../../.."

B=${BOOT:-build/rho-boot}
if [ ! -f "$B" ]; then
  echo "strops: no boot compiler at $B (make build/rho-boot first)"
  exit 2
fi

G=build/gate/strops
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
for src in tests/lang/strops/s*.rho tests/lang/strops/d*.rho; do
  name=$(basename "$src" .rho)
  total=$((total + 1))
  verdict=""
  case $(head -1 "$src") in
  "// exit:"*)
    want_exit=$(head -1 "$src" | sed 's|// exit: ||')
    rm -f "$G/$name.wasm" "$G/$name.got"
    if ! wr 60 "$B" build "$src" --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (build)"
    else
      wr 10 wasmtime run -W max-wasm-stack=1073741824 --dir . "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
      got_exit=$?
      if ! cmp -s "$G/$name.got" "tests/lang/strops/$name.out"; then
        verdict="FAIL (output)"
      fi
    fi
    ;;
  "// diag")
    rm -f "$G/$name.got"
    wr 60 "$B" check "$src" 2>&1 >/dev/null | strip_dbg > "$G/$name.got"
    if ! cmp -s "$G/$name.got" "tests/lang/strops/$name.out"; then
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

echo "strops: $((total - fails))/$total ok"
[ "$fails" = 0 ]
