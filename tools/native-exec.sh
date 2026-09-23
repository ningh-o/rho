#!/bin/sh
# tools/native-exec.sh — native image EXEC verification on the real
# machine at hand. The bootstrap gate never runs a native image (a
# corrupt one can wedge the kernel's page-hash check, SIGKILL-proof —
# the lore lives in docs/todo.md's history); this tool is the
# deliberate, separate step the gate's green crossings unblock.
#
# What it does, per program:
#   1. the mirror (build/gate/m.wasm) builds the program twice — once
#      to wasm32-wasi, once to the native target;
#   2. the wasm artifact runs under wasmtime (the behavioral baseline —
#      the same referee the gate uses);
#   3. the native image runs DIRECTLY on this machine.
#   stdout + exit code must agree.
#
# Safety laws (the wedge lore):
#   - every native image is written to a FRESH path (mktemp) and exec'd
#     exactly once — an image is never rebuilt in place, never re-exec'd;
#   - a build that fails is never exec'd;
#   - every exec runs under a wall-clock alarm (macOS has no timeout(1));
#   - exec happens only for targets this machine can actually run
#     (arm64-mac on an Apple-silicon mac; the Linux targets are skipped
#     with a note — they need a container or real hardware).
#
# Usage: tools/native-exec.sh [target]      (default: arm64-mac)
set -u
cd "$(dirname "$0")/.."

TARGET=${1:-arm64-mac}

B=build/rho-boot
M=${MIRROR:-build/gate/m.wasm}
G=build/gate/native-exec
mkdir -p "$G"

case "$(uname -s)-$(uname -m)" in
Darwin-arm64)
  RUNNABLE=arm64-mac
  ;;
*)
  RUNNABLE=none
  ;;
esac
if [ "$TARGET" != "$RUNNABLE" ]; then
  echo "native-exec: target $TARGET is not runnable on $(uname -s)-$(uname -m)"
  echo "  (exec verification for it needs a container or matching hardware;"
  echo "   the gate's build-only crossings remain the evidence there)"
  exit 3
fi

if [ ! -x "$B" ]; then
  echo "native-exec: no boot compiler at $B (make build/rho-boot first)"
  exit 2
fi
if [ ! -f "$M" ]; then
  echo "native-exec: no mirror at $M — building it"
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' 60 \
    "$B" build libs/compiler/main.rho --target wasm32-wasi -o "$M" || {
    echo "native-exec: mirror build failed"
    exit 2
  }
fi

# a representative slice: scalars, control flow, floats, strings (+ the
# retired-cat spellings), slices, statics, closures, generics, variadics,
# printf/format edges — plus the mirror-only element-wise == suite, whose
# eq$ helpers have never executed natively before this tool.
#
# Known failures (pre-existing, NOT introduced by the tree under test)
# carry a docs/todo.md entry with a minimal repro and diagnosis; they
# are reported as KNOWN and don't fail the run — but if one starts
# passing, the run fails: the list must not rot silently.
PROGS="corpus/001_hello corpus/002_arith corpus/004_branches corpus/007_slices \
corpus/010_statics corpus/013_floats corpus/020_closures corpus/027_variadics \
corpus/028_printf corpus/029_format corpus/042_divrem_signed corpus/060_cat_nest \
corpus/064_escapes_basic corpus/081_generics_bounds corpus/100_empty_slice \
tests/lang/eq/e01_slices tests/lang/eq/e02_nested tests/lang/eq/e03_structs \
tests/lang/eq/e04_enum_ladder"

wr() {
  perl -e 'alarm shift; exec @ARGV or die "cannot exec $ARGV[0]\n"' "$@"
}

strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

KNOWN="corpus/081_generics_bounds"

pass=0
fail=0
known=0
for src in $PROGS; do
  name=$(basename "$src" .rho)
  # fresh paths, always — the wedge lore forbids rebuilding in place
  wout="$G/$name.wasm"
  nimg=$(mktemp "$G/$name.XXXXXX")
  rm -f "$wout"
  if ! wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src.rho" \
      --target wasm32-wasi -o "$wout" >/dev/null 2>&1 || [ ! -f "$wout" ]; then
    echo "$name: FAIL (wasm build)"
    fail=$((fail + 1))
    rm -f "$nimg"
    continue
  fi
  if ! wr 90 wasmtime run -W max-wasm-stack=1073741824 --dir . "$M" build "$src.rho" \
      --target "$TARGET" -o "$nimg" >/dev/null 2>&1 || [ ! -f "$nimg" ]; then
    echo "$name: FAIL (native build)"
    fail=$((fail + 1))
    rm -f "$nimg"
    continue
  fi
  chmod +x "$nimg"
  # behavioral baseline first, then the native exec
  wr 30 wasmtime run "$wout" 2>/dev/null | strip_dbg > "$G/$name.want"
  wr 30 "$nimg" > "$G/$name.got" 2>"$G/$name.err"
  rc=$?
  wr 10 wasmtime run "$wout" >/dev/null 2>&1
  wrc=$?
  ok=1
  cmp -s "$G/$name.want" "$G/$name.got" || ok=0
  [ "$rc" = "$wrc" ] || ok=0
  if [ "$ok" = 1 ]; then
    if printf '%s\n' $KNOWN | grep -q "^$src$"; then
      echo "$name: FIXED? (was a known failure) — update KNOWN in this script and docs/todo.md"
      fail=$((fail + 1))
    else
      pass=$((pass + 1))
    fi
  elif printf '%s\n' $KNOWN | grep -q "^$src$"; then
    echo "$name: KNOWN (rc $rc vs $wrc; docs/todo.md carries the repro)"
    known=$((known + 1))
  else
    echo "$name: FAIL (rc $rc vs $wrc)"
    diff "$G/$name.want" "$G/$name.got" | head -4
    [ -s "$G/$name.err" ] && sed -n '1,3p' "$G/$name.err"
    fail=$((fail + 1))
  fi
  # exec'd exactly once; remove while nothing can still hold it mapped
  # (the .s sidecar rides along with every native build now)
  rm -f "$nimg" "$nimg.s" "$wout"
done

echo "native-exec ($TARGET): $pass ok, $known known, $fail fail"
[ "$fail" = 0 ]
