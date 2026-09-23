#!/bin/sh
# tests/lang/modsys — the module-system suite: `pub static mut`,
# module-qualified enum variants, the use-path root fallback, and (since the
# dot round) the point-sep module system: dots, aliases, `pub use`, and the
# resolution/encapsulation laws.
#
#   first line `// exit: N` — build, run under wasmtime, stdout must equal
#                            main.out and the exit code must be N
#   first line `// diag`    — `check` diagnostics must equal main.out
#
# One directory per test: main.rho is the entry (the root module), the
# siblings are its modules.
#
# Two builder lanes, by the two-layer feature law:
#   - plain fixtures are BOOT features (the mirror may only use what boot
#     implements) and run on the boot compiler;
#   - fixtures carrying an empty `mirror` marker file postdate boot's
#     freeze (dots / `as` / `pub use`) and run on the self-hosted
#     compiler. That mirror wasm is rebuilt here whenever any mirror source
#     is newer: boot builds it while it still can (leg 2 of the gate), and
#     once the mirror's own sources adopt post-boot syntax the pinned seed
#     (boot/rho-seed.wasm) takes over — exactly the gate's builder switch.
#
# Mirror exit-fixtures also get a fmt check: the formatter must print
# dots/`as`/`pub use` and roundtrip byte-for-byte (fmt∘fmt idempotent).
# Since the params round the mirror is seed-built unconditionally: the
# merged root (cli.rho) uses the root-const fold boot lacks.
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

# the wasm runner with the gate's stack headroom
wt() {
  wr "$1" wasmtime run --dir . "${@:2}"
}

# strip the temporary HEAP@/WE debug prints — the gate's strip_dbg law
strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

# the mirror lane's compiler: fresh whenever a mirror source is newer.
# The pinned seed is the builder (docs/bootstrap.md, the re-pinning
# ritual): the merged root (cli.rho) uses the root-const fold boot lacks.
M=$G/mirror.wasm
if [ ! -f "$M" ] || [ -n "$(find libs/compiler -name '*.rho' -newer "$M" -print -quit 2>/dev/null)" ]; then
  rm -f "$M"
  if ! wt 900 boot/rho-seed.wasm build libs/compiler/cli.rho --target wasm32-wasi -o "$M" >/dev/null 2>&1 || [ ! -f "$M" ]; then
    echo "modsys: the pinned seed cannot build the mirror"
    exit 2
  fi
fi

fails=0
total=0
for dir in tests/lang/modsys/*/; do
  [ -f "$dir/main.rho" ] || continue
  dir=${dir%/}
  name=$(basename "$dir")
  total=$((total + 1))
  mirror_fix=0
  [ -f "$dir/mirror" ] && mirror_fix=1
  verdict=""
  case $(head -1 "$dir/main.rho") in
  "// exit:"*)
    want_exit=$(head -1 "$dir/main.rho" | sed 's|// exit: ||')
    rm -f "$G/$name.wasm" "$G/$name.got"
    if [ "$mirror_fix" = 1 ]; then
      wt 120 "$M" build "$dir/main.rho" --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1
    else
      wr 60 "$B" build "$dir/main.rho" --target wasm32-wasi -o "$G/$name.wasm" >/dev/null 2>&1
    fi
    if [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (build)"
    else
      wr 10 wasmtime run --dir . "$G/$name.wasm" 2>/dev/null | strip_dbg > "$G/$name.got"
      got_exit=$?
      if ! cmp -s "$G/$name.got" "$dir/main.out"; then
        verdict="FAIL (output)"
      fi
    fi
    if [ "$mirror_fix" = 1 ] && [ -z "$verdict" ]; then
      # fmt: dots / `as` / `pub use` must canonicalize and roundtrip bytes
      rm -f "$dir/zz_fmt_tmp.rho" "$G/$name.fmt2"
      wt 60 "$M" fmt "$dir/main.rho" 2>/dev/null > "$dir/zz_fmt_tmp.rho"
      wt 60 "$M" fmt "$dir/zz_fmt_tmp.rho" 2>/dev/null > "$G/$name.fmt2"
      if ! cmp -s "$dir/zz_fmt_tmp.rho" "$G/$name.fmt2"; then
        verdict="FAIL (fmt roundtrip)"
      fi
      rm -f "$dir/zz_fmt_tmp.rho"
    fi
    ;;
  "// diag")
    rm -f "$G/$name.got"
    if [ "$mirror_fix" = 1 ]; then
      wt 120 "$M" check "$dir/main.rho" 2>&1 >/dev/null | strip_dbg > "$G/$name.got"
    else
      wr 60 "$B" check "$dir/main.rho" 2>&1 >/dev/null | strip_dbg > "$G/$name.got"
    fi
    if ! cmp -s "$G/$name.got" "$dir/main.out"; then
      verdict="FAIL (diag)"
    fi
    ;;
  "// build")
    # compile-only pin: the build must exit 0 and its diagnostics (stderr)
    # must equal main.out — for programs whose behavior cannot run (e.g. a
    # deliberate infinite tail loop) or whose law is the diagnostic itself
    rm -f "$G/$name.wasm" "$G/$name.got"
    if [ "$mirror_fix" = 1 ]; then
      wt 120 "$M" build "$dir/main.rho" --target wasm32-wasi -o "$G/$name.wasm" 2> "$G/$name.got" >/dev/null
    else
      wr 60 "$B" build "$dir/main.rho" --target wasm32-wasi -o "$G/$name.wasm" 2> "$G/$name.got" >/dev/null
    fi
    build_rc=$?
    if [ "$build_rc" != "0" ] || [ ! -f "$G/$name.wasm" ]; then
      verdict="FAIL (build)"
    elif ! cmp -s "$G/$name.got" "$dir/main.out"; then
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
