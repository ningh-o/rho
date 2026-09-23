#!/bin/sh
# Evergreen pin for the json package (rho/libs/json).
#
#   1. the package typechecks and is fmt-canonical;
#   2. every case in cases/ builds with the reference boot compiler, runs
#      under wasmtime, and matches its .out golden byte for byte (exit
#      code 0 included);
#   3. a fresh consumer package consumes json through the real rho-pkg
#      workflow — manifest written by hand, `add --path`, `install
#      --frozen` as the CI gate — then builds and runs through the
#      ordinary toolchain (`rho run`).
#
# No `make`, no corpus: the compiler corpus lives elsewhere and this
# package's tests are self-contained. The only artifacts are created in a
# temp directory; the tree is left exactly as found.
#
# Usage: sh rho/libs/json/tests/run.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)          # rho/libs/json/tests
PKGDIR=$(cd "$HERE/.." && pwd)               # rho/libs/json
ROOT=$(cd "$PKGDIR/../.." && pwd)            # the rho repo root
BOOT="$ROOT/build/rho-boot"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK" "$HERE/cases/json"' EXIT

fail() {
  echo "json tests FAIL: $*"
  exit 1
}

command -v wasmtime >/dev/null 2>&1 || fail "wasmtime not on PATH"
[ -x "$BOOT" ] || fail "missing $BOOT (build it once: make -C $ROOT build/rho-boot)"

# the formatter lives in the self-hosted compiler since boot's slimming:
# boot builds/checks, the mirror canonicalizes (bootstrap.md)
MIRROR="$ROOT/build/rho.wasm"
[ -f "$MIRROR" ] || MIRROR="$ROOT/build/gate/m.wasm"
[ -f "$MIRROR" ] || fail "missing the self-hosted compiler (make -C $ROOT site)"
mfmt() {
  wasmtime run -W max-wasm-stack=1073741824 --dir . "$MIRROR" fmt "$1"
}

# ---- 1. the package gates: typecheck + canonical formatting --------------
(cd "$PKGDIR" && "$BOOT" check json.rho) || fail "json.rho does not typecheck"
if ! (cd "$PKGDIR" && mfmt json.rho) 2>/dev/null | diff - "$PKGDIR/json.rho" >/dev/null; then
  fail "json.rho is not fmt-canonical (run: rho fmt -w json.rho through the self-hosted compiler)"
fi

# ---- 2. the case suite ----------------------------------------------------
# each case imports the package through a descending module path; a symlink
# named `json` points at the package root for the duration of the run
# (cases/ -> ../.. is rho/libs/json, so `use json/json;` finds json/json.rho)
ln -sfn ../.. "$HERE/cases/json"

failures=0
for case in "$HERE"/cases/[0-9]*.rho; do
  name=$(basename "$case" .rho)
  if ! "$BOOT" build "$case" -o "$WORK/$name.wasm" --target wasm32-wasi >/dev/null; then
    echo "FAIL $name (build)"
    failures=$((failures + 1))
    continue
  fi
  if ! wasmtime run "$WORK/$name.wasm" >"$WORK/$name.got"; then
    echo "FAIL $name (run exited nonzero)"
    failures=$((failures + 1))
    continue
  fi
  if ! diff -u "$HERE/cases/$name.out" "$WORK/$name.got" >"$WORK/$name.diff"; then
    echo "FAIL $name (output differs)"
    cat "$WORK/$name.diff"
    failures=$((failures + 1))
  else
    echo "ok $name"
  fi
done

# ---- 3. the rho-pkg path-dependency proof ---------------------------------
# a fresh consumer package vendors json (manifest + entry module — the
# whole package per docs/package-manager.md §4), wires it in with the real
# tool, passes the frozen gate, and runs through `rho run`.
"$BOOT" build "$ROOT/tools/pkg/rho-pkg.rho" -o "$WORK/rho-pkg.wasm" \
  --target wasm32-wasi >/dev/null || fail "rho-pkg failed to build"
CONS="$WORK/consumer"
mkdir -p "$CONS/vendor/json"
cp "$PKGDIR/rho.toml" "$PKGDIR/json.rho" "$CONS/vendor/json/"
cat >"$CONS/rho.toml" <<'TOML'
[package]
name = "json-consumer"
version = "0.1.0"
TOML
cat >"$CONS/main.rho" <<'RHO'
// consumer proof — reads a document, mutates it, writes it back.
use vendor/json/json;

fn main() -> i32 {
  let out = json.new_out();
  let src: string = "{\"name\": \"rho\", \"nums\": [1, 2.5, -0.0], \"skip\": false}";
  let r = json.parse(src, out);
  if r.is_err() {
    eprintf("parse: {}\n", json.err_msg(r));
    return 1;
  }
  printf("name={}\n", json.as_str(json.obj_get(out.root, "name")));
  json.obj_set(out.root, "edited", json.mk_bool(true));
  json.obj_set(out.root, "name", json.mk_str("rho-json"));
  let arr: *json.Json = json.obj_get(out.root, "nums");
  json.arr_push(arr, json.mk_num(5e-324));
  let e = json.encode_pretty(out.root);
  if e.is_err() {
    eprintf("encode: {}\n", json.err_msg(e));
    return 1;
  }
  printf("{}\n", json.err_text(e));
  return 0;
}
RHO
cd "$CONS"
printf 'add json --path vendor/json\n' | wasmtime run --dir . "$WORK/rho-pkg.wasm" \
  >"$WORK/pkg-add.log" || { cat "$WORK/pkg-add.log"; fail "rho-pkg add failed"; }
printf 'install --frozen\n' | wasmtime run --dir . "$WORK/rho-pkg.wasm" \
  >"$WORK/pkg-frozen.log" || { cat "$WORK/pkg-frozen.log"; fail "rho-pkg install --frozen failed"; }
grep -q '"json"' "$CONS/rho.lock" || fail "rho.lock does not mention json"
"$BOOT" run main.rho >"$WORK/consumer.got" || fail "the consumer failed to run"
if ! diff -u "$HERE/consumer.out" "$WORK/consumer.got" >"$WORK/consumer.diff"; then
  echo "FAIL consumer (output differs)"
  cat "$WORK/consumer.diff"
  failures=$((failures + 1))
else
  echo "ok consumer (rho-pkg path dependency)"
fi

if [ "$failures" -gt 0 ]; then
  fail "$failures case(s) failing"
fi
echo "json tests ok"
