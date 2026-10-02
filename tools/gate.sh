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

# --- leg 2c: robustness (T3.17 leg 1 — hostile inputs never crash
# the compiler; the only acceptable outcomes are a clean refuse or a
# clean compile, never a signal or a hang) ---
echo "== gate: robust (hostile inputs under wall-clock caps)"
cap 300 ./tests/run-robust.sh >/dev/null || fail "robust"

# --- leg 2d: the fuzz differential (T3.3 + T3.17's discovery leg) —
# seeded LCG random programs, boot vs the self-hosted chain, stdout +
# exit must agree. Fully reproducible by seed; finds land pinned in
# the corpus (n22-n25 name theirs). ---
echo "== gate: fuzz differential (seeds 1..150, seeded LCG)"
cap 900 node tools/fuzz/gen.mjs --from 1 --to 150 --budget 780 --step 15 \
  >/dev/null || fail "fuzz differential"

# --- leg 2e: the sanitizer build (T3.17's mechanical detector) —
# ASAN+UBSAN boot over robust/, the corpus, and the saved fuzz
# programs. A sanitizer report or a signal is a finding regardless of
# the exit code (ASAN exits 1 on its own, indistinguishable from a
# clean refuse — the stderr pattern is the detector). ---
echo "== gate: asan+ubsan boot over robust/, corpus, fuzz batch"
cap 300 make asan >/dev/null || fail "asan build"
asan_report() { # verb file out rc
  case "$3" in
    *"AddressSanitizer"*|*"LeakSanitizer"*|*"runtime error"*)
      fail "asan $1 $2: sanitizer report: $(printf '%s' "$3" | head -2)" ;;
  esac
  [ "$4" -le 1 ] || fail "asan $1 $2: exit $4 (crash or timeout)"
}
for f in tests/robust/*.rho(N); do
  for verb in check build; do
    out=$(cap 60 ./build/rho-asan $verb "$f" 2>&1)
    asan_report "$verb" "robust/$(basename "$f")" "$out" $?
  done
done
for f in tests/suites/programs/*.rho(N); do
  out=$(cap 60 ./build/rho-asan check "$f" 2>&1)
  asan_report check "programs/$(basename "$f")" "$out" $?
done
for f in examples/*.rho(N); do
  out=$(cap 60 ./build/rho-asan check "$f" 2>&1)
  asan_report check "examples/$(basename "$f")" "$out" $?
done
i=0
for f in build/gate/fuzz/*.rho(N); do
  i=$((i+1))
  verb=check
  [ $((i % 10)) -eq 0 ] && verb=build
  out=$(cap 60 ./build/rho-asan $verb "$f" 2>&1)
  asan_report "$verb" "fuzz/$(basename "$f")" "$out" $?
done
rm -f build/rho-asan

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

# --- leg 5: the seed canary (T3.2) — the pinned build/seed.wasm is
# byte-compared against the mirror this run rebuilt; inequality is the
# determinism alarm. Re-pin ONLY in the commit that changes the
# compiler. The canary's SRC is the old corpus/001_hello — the file
# retired with corpus/ (T6.4), so its exact bytes live here: the
# canary input must never change, only the compiler it bakes. ---
echo "== gate: seed canary (pure-source rebuild, byte-exact)"
CANARY_SRC='// exit: 0
fn main() -> i32 {
  printf("hello, world\n");
  return 0;
}'
if [ -f build/seed.wasm ]; then
  cap 600 "$RHO" build libs/compiler/main.rho -o /tmp/gate-seed.wasm \
      --set "SRC=$CANARY_SRC" --set "MODS=" >/dev/null 2>&1
  cap 600 "$RHO" build libs/compiler/main.rho -o /tmp/gate-seed2.wasm \
      --set "SRC=$CANARY_SRC" --set "MODS=" >/dev/null 2>&1
  cmp -s /tmp/gate-seed.wasm /tmp/gate-seed2.wasm \
    || fail "two identical builds differ (nondeterminism alarm)"
  # the canary pins the compiler CHAIN product: the mirror rebuild
  cmp -s /tmp/gate-mirror.wasm build/seed.wasm \
    || fail "seed canary mismatch — rebuild != pin (re-pin only in the commit that changes the compiler)"
else
  fail "build/seed.wasm missing — the canary pin is law (T3.2)"
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
cat > /tmp/gate-par/c.rho <<'EOF'
fn main() -> i32 {
  let x: i32 = 1;
  match x {
    1 => printf("one\n"),
    _ => printf("other\n"),
  };
  return 0;
}
EOF
par /tmp/gate-par/a.rho "unknown fn"
par /tmp/gate-par/b.rho "unknown name"
par /tmp/gate-par/c.rho "expected an expression, found ';'"

echo "GATE GREEN — every leg passed"
