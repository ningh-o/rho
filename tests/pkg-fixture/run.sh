#!/bin/sh
# Evergreen package-manager fixture (docs/package-manager.md §11).
#
# Proves, end to end and without a network:
#   1. the committed rho-pkg source builds with the reference boot compiler;
#   2. an app package resolves a path dependency that itself carries a
#      transitive path dependency, plus a git dependency;
#   3. install prints the clone command while a git dep is missing, and
#      verifies the pinned rev once it is cloned;
#   4. rho.lock is byte-stable across regenerations and the --frozen gate
#      rejects both a tampered lock and a wrong checkout rev;
#   5. the app builds and runs through the ordinary toolchain (`rho run`).
#
# The git remote is rebuilt deterministically in a temp directory (fixed
# dates, fixed identities, fixed content), so the pinned rev is stable.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd) # the rho repo root
APP="$ROOT/tests/pkg-fixture/app"
BOOT="$ROOT/build/rho-boot"
WORK=$(mktemp -d)
RESTORE_LOCK="$WORK/lock.pristine"

cp "$APP/rho.lock" "$RESTORE_LOCK"
cleanup() {
  cp "$RESTORE_LOCK" "$APP/rho.lock"
  rm -rf "$WORK" "$APP/vendor/strlib"
}
trap cleanup EXIT

fail() {
  echo "pkg-fixture FAIL: $*"
  exit 1
}

# start from the committed state: any leftover clone must not mask step 3
rm -rf "$APP/vendor/strlib"

command -v wasmtime >/dev/null 2>&1 || fail "wasmtime not on PATH"
[ -x "$BOOT" ] || fail "missing $BOOT (build it once: make -C $ROOT build/rho-boot)"

# ---- 1. build the tool from the committed source -------------------------
"$BOOT" build "$ROOT/tools/pkg/rho-pkg.rho" -o "$WORK/rho-pkg.wasm" \
  --target wasm32-wasi >/dev/null || fail "rho-pkg failed to build"
PKG="wasmtime run --dir . $WORK/rho-pkg.wasm"

# ---- 2. deterministic git remote (two commits) ---------------------------
export GIT_AUTHOR_DATE="2026-01-01T00:00:00+00:00"
export GIT_COMMITTER_DATE="2026-01-01T00:00:00+00:00"
export GIT_AUTHOR_NAME=fixture GIT_AUTHOR_EMAIL=fixture@local
export GIT_COMMITTER_NAME=fixture GIT_COMMITTER_EMAIL=fixture@local
REMOTE="$WORK/remote"
mkdir "$REMOTE"
cd "$REMOTE"
git init -q -b main
git config commit.gpgsign false
printf '[package]\nname = "strlib"\nversion = "0.1.0"\n' > rho.toml
printf 'pub fn placeholder() -> i32 {\n  return 0;\n}\n' > strlib.rho
git add -A
git commit -qm "strlib 0.1.0 base"
REV1=$(git rev-parse HEAD)
printf '// strlib — a library package delivered as a git dependency.\npub fn rev(s: string) -> string {\n  let mut out: string = "";\n  let mut i: usize = len(s);\n  while i > 0 {\n    i -= 1;\n    let one: []u8 = make([]u8, 1);\n    one[0] = s[i];\n    out = out + intrinsics.slice_string(one);\n  }\n  return out;\n}\n' > strlib.rho
git add -A
git commit -qm "strlib 0.2.0"
REV2=$(git rev-parse HEAD)
grep -q "$REV2" "$APP/rho.toml" ||
  fail "app/rho.toml pins a rev the deterministic remote does not produce ($REV2)"

# ---- 3. install before the clone: must print the clone command ------------
cd "$APP"
if $PKG install >"$WORK/step3" 2>&1; then
  fail "install succeeded while strlib was not cloned"
fi
grep -q "git clone https://rho-fixture.local/strlib vendor/strlib" "$WORK/step3" ||
  fail "install did not print the clone command"

# ---- 4. clone at the pinned rev; install and the frozen gate pass ---------
git -c url."$REMOTE".insteadOf="https://rho-fixture.local/strlib" \
  clone -q https://rho-fixture.local/strlib vendor/strlib
git -C vendor/strlib checkout -q "$REV2"
$PKG install >/dev/null || fail "install failed with the dep cloned"
$PKG install --frozen >/dev/null || fail "install --frozen failed"

# ---- 5. the regenerated lock is byte-identical to the committed one -------
diff -q "$RESTORE_LOCK" "$APP/rho.lock" >/dev/null ||
  fail "install rewrote rho.lock to different bytes"

# ---- 6. the app runs through the ordinary toolchain -----------------------
"$BOOT" run main.rho >"$WORK/app.out" || fail "the fixture app failed to run"
diff -u "$APP/golden.out" "$WORK/app.out" || fail "app output differs from golden"

# ---- 7. wrong checkout rev: both gates reject, repair message printed -----
git -C vendor/strlib checkout -q "$REV1"
if $PKG install --frozen >/dev/null 2>&1; then
  fail "frozen gate passed at the wrong rev"
fi
if $PKG install >"$WORK/step7" 2>&1; then
  fail "install passed at the wrong rev"
fi
grep -q "checkout $REV2" "$WORK/step7" || fail "no repair command for the wrong rev"
git -C vendor/strlib checkout -q "$REV2"
$PKG install >/dev/null || fail "install failed after repair"

# ---- 8. tampered lock: frozen rejects; install repairs byte-identically ---
sed 's/^version = "0.1.0"$/version = "9.9.9"/' "$APP/rho.lock" >"$WORK/tampered"
mv "$WORK/tampered" "$APP/rho.lock"
if $PKG install --frozen >/dev/null 2>&1; then
  fail "frozen gate passed with a tampered lock"
fi
$PKG install >/dev/null || fail "install failed to repair the lock"
diff -q "$RESTORE_LOCK" "$APP/rho.lock" >/dev/null ||
  fail "repaired lock is not byte-identical to the committed one"

# ---- 9. usage errors exit 2 ------------------------------------------------
if $PKG >/dev/null 2>&1; then
  fail "no arguments should be a usage error (exit 2)"
fi

echo "pkg-fixture ok"
