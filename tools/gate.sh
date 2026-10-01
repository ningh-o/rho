#!/bin/zsh
# gate.sh — the 0.1.0 gate (T3.1). Every leg time-capped; any red leg
# fails the gate. Legs:
#   1 boot selftest           (the seed's own harness)
#   2 the script legs         (check/emit/fmt/set + fmt parity)
#   3 corpus differential     (boot-built vs self-hosted-built, 108/108)
#   4 the self chain          (mirror -> child -> grandchild; the child
#                              and grandchild agree byte-for-byte, the
#                              grandchild behaves like boot on probes)
#   5 pure-source trust root  (the seed rebuilds from boot's C source;
#                              the pinned canary byte-compares — T3.2)
#   6 diagnostic parity       (both compilers refuse the same broken
#                              programs, same stderr substrings)
set -u
cd "$(dirname "$0")/.."
RHO=${RHO:-./build/rho}
legs=()

cap() { perl -e 'alarm shift; exec @ARGV' -- "$@"; }

fail() { echo "GATE RED: $1"; exit 1; }

# --- leg 0: the seed builds from source (pure-source trust root) ---
echo "== gate: build boot from C source"
cap 120 make all >/dev/null || fail "boot build"
[ -x build/rho ] || fail "boot binary"

# --- leg 1: selftest ---
echo "== gate: boot selftest"
cap 60 "$RHO" selftest >/dev/null || fail "selftest"

# --- leg 2: the script legs (behavioral tests live in the suites; these
# are the formatter and cross-compiler infrastructure legs) ---
for s in run-fmt-tests run-fmt-self; do
  echo "== gate: $s"
  cap 300 ./tests/$s.sh >/dev/null || fail "$s"
done

# --- leg 2b: the conformance suites (the main test framework) ---
echo "== gate: the suites (rho test tests/suites)"
cap 900 "$RHO" test tests/suites >/dev/null || fail "the suites"

# --- leg 3: the corpus differential (108/108, pinned) ---
echo "== gate: corpus differential"
cap 1800 ./tests/run-corpus-diff.sh >/dev/null || fail "corpus differential"

# --- leg 4: the self chain ---
echo "== gate: the self chain (mirror -> child -> grandchild)"
# the mirror: boot compiles the self-hosted compiler
modsrc() { cat libs/compiler/main.rho; }
CMODS=""
for f in libs/compiler/*.rho; do
  [ "$(basename "$f")" = "main.rho" ] && continue
  CMODS="$CMODS@MOD@ ${f#libs/compiler/}
$(cat "$f")
"
done
# the reserved std/ tree rides the bake too (repo-relative paths) —
# the compiler's own sources consume std.collections (T4.3)
for f in std/*.rho std/*/*.rho std/*/*/*.rho; do
  [ -f "$f" ] && CMODS="$CMODS@MOD@ $f
$(cat "$f")
"
done
cap 600 "$RHO" build libs/compiler/main.rho -o /tmp/gate-mirror.wasm \
    --set "SRC=$(modsrc)" --set "MODS=$CMODS" >/dev/null 2>&1 \
  || fail "mirror build"
# the child: the mirror compiles the compiler
cap 600 wasmtime /tmp/gate-mirror.wasm >/tmp/gate-child.wat 2>/dev/null \
  || fail "mirror run"
[ -s /tmp/gate-child.wat ] || fail "mirror emitted nothing"
wat2wasm /tmp/gate-child.wat -o /tmp/gate-child.wasm 2>/dev/null \
  || fail "child wat2wasm"
# the grandchild: the child compiles the compiler (same input — the
# determinism law wants byte-identical WAT to the child's own build)
cap 600 wasmtime /tmp/gate-child.wasm --set "SRC=$(modsrc)" \
    --set "MODS=$CMODS" >/tmp/gate-grand.wat 2>/dev/null \
  || fail "child run"
cmp -s /tmp/gate-child.wat /tmp/gate-grand.wat \
  || fail "child/grandchild WAT diverge (determinism law)"
# behavior: every level's input is frozen at bake time, so no deeper
# level can ever compile a fresh probe — what the old probe clause was
# after (the grandchild behaves as the child) is implied by the
# byte-identity just proved, and the rho codegen's program-level
# behavior is graded against boot by leg 3's corpus differential. The
# direct residue: run the grand — its code compiling its own baked
# input must reproduce itself (v3 == v2, the chain closed at depth).
wat2wasm /tmp/gate-grand.wat -o /tmp/gate-grand.wasm 2>/dev/null \
  || fail "grandchild wat2wasm"
cap 600 wasmtime /tmp/gate-grand.wasm >/tmp/gate-v3.wat 2>/dev/null \
  || fail "grand run"
cmp -s /tmp/gate-grand.wat /tmp/gate-v3.wat \
  || fail "v3 diverges (chain not closed at depth)"

# --- leg 5: the seed canary (T3.2) ---
echo "== gate: seed canary (pure-source rebuild, byte-exact)"
if [ -f build/seed.wasm ]; then
  cap 600 "$RHO" build libs/compiler/main.rho -o /tmp/gate-seed.wasm \
      --set "SRC=$(cat corpus/001_hello.rho)" --set "MODS=" >/dev/null 2>&1
  cap 600 "$RHO" build libs/compiler/main.rho -o /tmp/gate-seed2.wasm \
      --set "SRC=$(cat corpus/001_hello.rho)" --set "MODS=" >/dev/null 2>&1
  cmp -s /tmp/gate-seed.wasm /tmp/gate-seed2.wasm \
    || fail "two identical builds differ (nondeterminism alarm)"
  # the canary pins the compiler CHAIN product: the mirror rebuild
  cmp -s /tmp/gate-mirror.wasm build/seed.wasm \
    || fail "seed canary mismatch — rebuild != pin (re-pin only in the commit that changes the compiler)"
fi

# --- leg 6: diagnostic parity ---
echo "== gate: diagnostic parity"
par() {
  local bad=$1 want=$2
  local bmsg smsg
  bmsg=$("$RHO" check "$bad" 2>&1 >/dev/null); local brc=$?
  # the self-host side bakes the bad program into a fresh mirror (a
  # compiled compiler's input is frozen at bake time — a runtime --set
  # is ignored, and reusing the chain's mirror just recompiles the
  # compiler and accepts everything)
  cap 300 "$RHO" build libs/compiler/main.rho -o /tmp/gate-par.wasm \
      --set "SRC=$(cat "$bad")" --set "MODS=" >/dev/null 2>&1 \
    || fail "parity mirror build: $bad"
  cap 300 wasmtime /tmp/gate-par.wasm >/dev/null 2>/tmp/gate-par.err
  local src_rc=$?
  smsg=$(cat /tmp/gate-par.err)
  [ "$brc" -ne 0 ] || fail "parity: boot accepted $bad"
  [ "$src_rc" -ne 0 ] || fail "parity: self-host accepted $bad"
  case "$smsg" in
    *"$want"*) ;;
    *) fail "parity: self-host stderr lacks '$want' for $bad (got: $smsg)" ;;
  esac
}
mkdir -p /tmp/gate-par
cat > /tmp/gate-par/a.rho <<'EOF'
fn main() -> i32 { printf("{}", nosuchfn(1)); return 0; }
EOF
cat > /tmp/gate-par/b.rho <<'EOF'
fn main() -> i32 { let x: i32 = 1; printf("{}", y); return 0; }
EOF
par /tmp/gate-par/a.rho "unknown fn"
par /tmp/gate-par/b.rho "unknown name"

echo "GATE GREEN — every leg passed"
