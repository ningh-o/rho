#!/bin/sh
# tools/native-ring.sh — the FULL/native configuration's bootstrap ring, the
# deferred item docs/todo.md's NEXT section describes. The gate's crossings
# prove native self-builds STRUCTURALLY (image-check, never exec'd); this
# script goes further: it EXECUTES a self-built native compiler under the
# same safety laws as tools/native-exec.sh (fresh paths, wall-clock alarms,
# exec only for targets this machine can run) and closes the ring:
#
#   1. the mirror (build/gate/m.wasm) builds the package root to arm64-mac
#      and the image is structure-checked (the gate's own bar);
#   2. the native compiler runs: --version, then it compiles a corpus
#      program to wasm and the artifact must run to the golden behavior —
#      a native-hosted compiler producing correct wasm;
#   3. THE RING: the native compiler compiles the package root AGAIN
#      (native -> native, the grandchild of the crossings), structure-
#      checked the same way;
#   4. the ring child compiles a corpus slice to wasm; every program must
#      match the shared goldens — the full/native configuration's own
#      bootstrap loop, closed.
#
# KNOWN-FAILURES law (inherited from native-exec.sh): open bugs carry a
# docs/todo.md entry and are reported as KNOWN without failing the run;
# if one starts passing the run fails — the list must not rot.
#
# Usage: tools/native-ring.sh          (arm64-mac only; needs build/gate/m.wasm)
set -u
cd "$(dirname "$0")/.."

M=${MIRROR:-build/gate/m.wasm}
G=build/gate/native-ring
mkdir -p "$G"

case "$(uname -s)-$(uname -m)" in
Darwin-arm64) ;;
*)
  echo "native-ring: needs Darwin-arm64 to exec arm64-mac images"
  exit 3
  ;;
esac

if [ ! -f "$M" ]; then
  echo "native-ring: no mirror at $M — run the gate first (or set MIRROR=)"
  exit 2
fi

wr() {
  perl -e 'alarm shift; exec @ARGV or die "native-ring: cannot exec $ARGV[0]: $!\n"' "$@"
}

# the wedge laws: every native image is built to a FRESH path, exec'd only
# after a successful build, and never rebuilt in place
SELF=$(mktemp "$G/rho.XXXXXX")
RING=$(mktemp "$G/ring-child.XXXXXX")
cleanup() { rm -f "$SELF" "$SELF.s" "$RING" "$RING.s" "$G"/slice.*.wasm; }
trap cleanup EXIT INT TERM

pass=0
fail=0
known=0

# 1 — the native self image, structure-checked
if ! wr 900 wasmtime run --dir . "$M" build libs/compiler/cli.rho \
    --target arm64-mac -o "$SELF" >/dev/null 2>&1 || [ ! -f "$SELF" ]; then
  echo "native-ring: self build FAILED"
  exit 1
fi
if ! run_t=1 python3 tools/image-check.py "$SELF" arm64-mac >/dev/null 2>&1; then
  echo "native-ring: self image failed structure check"
  exit 1
fi
chmod +x "$SELF"

# 2 — the native compiler is alive and produces correct wasm
v=$(wr 15 "$SELF" --version 2>/dev/null)
if [ "$v" = "rho 0.4.0" ]; then
  pass=$((pass+1))
else
  echo "version: got '${v:-none}'"
  fail=$((fail+1))
fi
rm -f "$G"/slice.hello.wasm
if wr 60 "$SELF" build corpus/001_hello.rho --target wasm32-wasi \
    -o "$G"/slice.hello.wasm >/dev/null 2>&1 && [ -f "$G"/slice.hello.wasm ]; then
  h=$(wr 15 wasmtime run "$G"/slice.hello.wasm 2>/dev/null </dev/null)
  if [ "$h" = "hello, world" ]; then
    pass=$((pass+1))
  else
    echo "hello: native-compiled wasm printed '${h:-none}'"
    fail=$((fail+1))
  fi
else
  echo "hello: native compile to wasm FAILED"
  fail=$((fail+1))
fi

# 3 — the ring: the native compiler compiles the package root again
if wr 900 "$SELF" build libs/compiler/cli.rho --target arm64-mac \
    -o "$RING" >/dev/null 2>&1 && [ -f "$RING" ] &&
   python3 tools/image-check.py "$RING" arm64-mac >/dev/null 2>&1; then
  pass=$((pass+1))
  chmod +x "$RING"
else
  echo "ring-child: native->native self build FAILED (or failed structure check)"
  fail=$((fail+1))
fi

# 4 — the ring child rebuilds a corpus slice to the shared goldens
SLICE="001_hello 002_arith 004_branches 010_statics 013_floats 020_closures"
if [ -f "$RING" ]; then
  for name in $SLICE; do
    rm -f "$G/slice.$name.wasm"
    if ! wr 60 "$RING" build "corpus/$name.rho" --target wasm32-wasi \
        -o "$G/slice.$name.wasm" >/dev/null 2>&1 || [ ! -f "$G/slice.$name.wasm" ]; then
      echo "slice $name: ring-child build FAILED"
      fail=$((fail+1))
      continue
    fi
    out=$(wr 15 wasmtime run "$G/slice.$name.wasm" 2>/dev/null </dev/null)
    want=$(cat "corpus/$name.out" 2>/dev/null)
    wantrc=$(sed -n 's|^// exit: ||p' "corpus/$name.rho" | head -1)
    [ -n "$wantrc" ] || wantrc=0
    rc=$?
    wr 15 wasmtime run "$G/slice.$name.wasm" >/dev/null 2>&1 </dev/null
    rc=$?
    if [ "$out" = "$want" ] && [ "$rc" = "$wantrc" ]; then
      pass=$((pass+1))
    else
      echo "slice $name: behavior differs (rc=$rc want=$wantrc)"
      fail=$((fail+1))
    fi
  done
fi

echo "native-ring: $pass pass, $fail fail, $known known"
[ "$fail" = 0 ]
