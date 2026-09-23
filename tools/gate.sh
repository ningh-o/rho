#!/bin/sh
# The bootstrap gate: one command, one verdict per leg.
#
#   tools/gate.sh --wasm     this round's acceptance: the five wasm legs,
#                            no native crossings (~1 min measured)
#   tools/gate.sh            full run (the wasm legs + the crossings)
#   tools/gate.sh --fast     full run minus the self chain (~4 min)
#
# Self-sufficient: if the oracle (build/rho-boot) is missing or older than
# its sources (boot/src, boot/prelude, tools/embed.py), the gate rebuilds
# it first. Every rho invocation runs under a wall-clock cap (run_t /
# wasmtime_run), so no leg can hang the gate.
#
# Legs (functional equivalence is the law: same program in, same stdout +
# exit code out — never a byte compare of two compilers' wasm):
#   1 boot-selftest    the oracle is healthy (goldens + diag)
#   2 build-mirror     boot compiles the mirror to wasm
#   3 corpus-diff      every corpus program: boot-built vs mirror-built
#                      wasm artifacts run with identical stdout + exit;
#                      the boot runs are the corpus goldens
#   4 self-chain       mirror builds itself -> child; child builds itself
#                      -> grandchild; the grandchild rebuilds every corpus
#                      program and matches the boot goldens in behavior
#   5 crossings        per image target: mirror builds itself + a hello
#                      smoke (build-only; exec natively is never done here
#                      — a corrupt image wedges the kernel, SIGKILL-proof)
#   6 diag-parity      tests/diag/*.rho: mirror's diagnostics are byte-
#                      identical to boot's
set -u
cd "$(dirname "$0")/.."

FAST=0; WASM=0
[ "${1:-}" = "--fast" ] && FAST=1
[ "${1:-}" = "--wasm" ] && WASM=1

G=build/gate
mkdir -p "$G"
PASS=0; FAIL=0

# run_t <seconds> <command...> — run a real executable under a wall-clock
# cap (macOS has no GNU timeout; the alarm survives exec). The command must
# be a real binary: perl's exec cannot resolve shell functions, and an
# unresolvable name must die nonzero — a silent fall-through would turn
# every wasmtime leg into a fake green.
run_t() {
  t=$1; shift
  perl -e 'alarm shift; exec @ARGV or die "run_t: cannot exec $ARGV[0]: $!\n"' "$t" "$@"
}

# wasmtime_run <seconds> <module-and-args...> — the wasm runner under a
# wall-clock cap. A function, not a variable: a multi-word command string
# passed through leg()'s "$@" quoted would be looked up as one (nonexistent)
# command name. Perl execs the real wasmtime binary, so the alarm sticks.
wasmtime_run() {
  t=$1; shift
  perl -e 'alarm shift; exec @ARGV or die "wasmtime_run: cannot exec $ARGV[0]: $!\n"' "$t" \
    wasmtime run -W max-wasm-stack=1073741824 --dir . "$@"
}

# strip the temporary WE debug prints (scratch lines in boot/src:
# "WE <n> <symbol>" from boot/src/emit_wasm.c; the mirror's HEAP@ scratch
# lines were removed 2026-09-23 — the filter keeps boot's)
# before text compares. ^WE is anchored
# with a space — the debug format always has one — so a real output line that
# merely begins with the letters WE ("WEird…") survives. Nothing else is
# filtered.
strip_dbg() {
  grep -v -e '^HEAP@' -e '^WE '
}

# the gate is invoked directly (no Makefile target): if the oracle is
# missing or older than anything it is built from — boot/src (C + the
# generated prelude_data.c) or its inputs (boot/prelude, tools/embed.py) —
# rebuild it first
if [ ! -x build/rho-boot ] || [ -n "$(find boot/src boot/prelude tools/embed.py -newer build/rho-boot -print -quit 2>/dev/null)" ]; then
  echo "build/rho-boot missing or older than boot/src — rebuilding (60s cap)"
  run_t 60 make build/rho-boot || { echo "gate aborted: cannot make build/rho-boot"; exit 1; }
fi

# leg <name> <command...> — GREEN on exit 0, RED otherwise (tail of output)
leg() {
  name=$1; shift
  printf '%-24s' "$name"
  if out=$("$@" 2>&1); then
    echo GREEN; PASS=$((PASS+1))
  else
    echo RED
    printf '%s\n' "$out" | tail -3 | sed 's/^/    /'
    FAIL=$((FAIL+1))
  fi
}

# cross_smoke <target> <image> — hello build for one native target; the
# image must actually exist (a silent exit-0 with no write is not a green)
cross_smoke() {
  wasmtime_run 90 $G/m.wasm build corpus/001_hello.rho --target "$1" -o "$2" && [ -f "$2" ]
}

# cross_self <target> <image> — the full self-build for one native target
cross_self() {
  wasmtime_run 900 $G/m.wasm build libs/compiler/main.rho --target "$1" -o "$2" && [ -f "$2" ]
}

echo "== rho bootstrap gate $(date '+%H:%M:%S') =="

# 1 — the oracle
leg boot-selftest run_t 10 ./build/rho-boot selftest

# 2 — boot compiles the mirror. The old image is removed first: a failed
# build must not leave the three mirror-side legs below grading last run's
# compiler. Boot builds the mirror in under a second (measured 0.6s); the
# 60s cap is headroom for a cold machine, not slack.
rm -f $G/m.wasm
leg build-mirror run_t 60 ./build/rho-boot build libs/compiler/main.rho --target wasm32-wasi -o $G/m.wasm

# every leg below grades the mirror THIS run produced; without one there is
# nothing to compare and the leg goes RED saying so — it never falls back
# to a stale image
no_mirror() {
  printf '%-24s' "$1"
  echo "RED (no fresh mirror: build-mirror failed)"
  FAIL=$((FAIL+1))
}

# 3 — corpus differential (wasm): boot artifact vs mirror artifact
printf '%-24s' corpus-diff
if [ ! -f $G/m.wasm ]; then
  no_mirror corpus-diff
else
mkdir -p $G/corpus
diffs=0; total=0
for src in corpus/*.rho; do
  name=$(basename "$src" .rho)
  total=$((total+1))
  rm -f $G/corpus/${name}.boot.wasm $G/corpus/${name}.self.wasm
  run_t 10 ./build/rho-boot build "$src" --target wasm32-wasi -o $G/corpus/${name}.boot.wasm >/dev/null 2>&1 || { echo "corpus $name: boot build failed"; diffs=$((diffs+1)); continue; }
  [ -f $G/corpus/${name}.boot.wasm ] || { echo "corpus $name: boot produced no artifact"; diffs=$((diffs+1)); continue; }
  wasmtime_run 10 $G/m.wasm build "$src" --target wasm32-wasi -o $G/corpus/${name}.self.wasm >/dev/null 2>&1 || { echo "corpus $name: mirror build failed"; diffs=$((diffs+1)); continue; }
  [ -f $G/corpus/${name}.self.wasm ] || { echo "corpus $name: mirror produced no artifact"; diffs=$((diffs+1)); continue; }
  bout=$(wasmtime_run 10 $G/corpus/${name}.boot.wasm 2>/dev/null); brc=$?
  sout=$(wasmtime_run 10 $G/corpus/${name}.self.wasm 2>/dev/null); src_rc=$?
  bout=$(printf '%s' "$bout" | strip_dbg)
  sout=$(printf '%s' "$sout" | strip_dbg)
  # boot is the frozen oracle: its behavior is the corpus golden the
  # self-chain leg tests the grandchild against
  printf '%s' "$bout" > $G/corpus/${name}.golden.out
  printf '%s\n' "$brc" > $G/corpus/${name}.golden.rc
  if [ "$bout" != "$sout" ] || [ "$brc" != "$src_rc" ]; then
    echo "corpus $name: behavior differs (boot rc=$brc mirror rc=$src_rc)"
    diffs=$((diffs+1))
  fi
done
if [ "$diffs" = 0 ]; then echo "GREEN ($total programs)"; PASS=$((PASS+1))
else echo "RED ($diffs/$total differ)"; FAIL=$((FAIL+1)); fi
fi

# 4 — the self chain, one leg: mirror builds itself -> child; child builds
# itself -> grandchild; then the grandchild rebuilds every corpus program
# and must match the boot golden in behavior (stdout + exit). Child and
# grandchild are never compared byte for byte — two correct compilers may
# lay out functions differently; what must agree is what the programs do.
if [ "$FAST" = 0 ]; then
  printf '%-24s' self-chain
  rm -f $G/child.wasm $G/grandchild.wasm
  # builds silenced like corpus-diff's: the mirror's stderr still carries
  # HEAP@ scratch lines, and the verdict line owns the leg's output
  if [ ! -f $G/m.wasm ]; then
    no_mirror self-chain
  elif ! wasmtime_run 900 $G/m.wasm build libs/compiler/main.rho --target wasm32-wasi -o $G/child.wasm >/dev/null 2>&1 || [ ! -f $G/child.wasm ]; then
    echo "RED (child build failed)"; FAIL=$((FAIL+1))
  elif ! wasmtime_run 900 $G/child.wasm build libs/compiler/main.rho --target wasm32-wasi -o $G/grandchild.wasm >/dev/null 2>&1 || [ ! -f $G/grandchild.wasm ]; then
    echo "RED (grandchild build failed)"; FAIL=$((FAIL+1))
  else
    mkdir -p $G/selfchain
    sdiffs=0; stotal=0
    for src in corpus/*.rho; do
      name=$(basename "$src" .rho)
      stotal=$((stotal+1))
      goutf=$G/corpus/${name}.golden.out; grcf=$G/corpus/${name}.golden.rc
      if [ ! -f "$goutf" ] || [ ! -f "$grcf" ]; then
        echo "selfchain $name: no boot golden (boot build failed in corpus-diff)"
        sdiffs=$((sdiffs+1)); continue
      fi
      rm -f $G/selfchain/${name}.wasm
      if ! wasmtime_run 10 $G/grandchild.wasm build "$src" --target wasm32-wasi -o $G/selfchain/${name}.wasm >/dev/null 2>&1 || [ ! -f $G/selfchain/${name}.wasm ]; then
        echo "selfchain $name: grandchild build failed"
        sdiffs=$((sdiffs+1)); continue
      fi
      cout=$(wasmtime_run 10 $G/selfchain/${name}.wasm 2>/dev/null); crc=$?
      cout=$(printf '%s' "$cout" | strip_dbg)
      if [ "$cout" != "$(cat "$goutf")" ] || [ "$crc" != "$(cat "$grcf")" ]; then
        echo "selfchain $name: behavior differs from boot golden (rc=$crc golden rc=$(cat "$grcf"))"
        sdiffs=$((sdiffs+1))
      fi
    done
    if [ "$sdiffs" = 0 ]; then echo "GREEN ($stotal programs)"; PASS=$((PASS+1))
    else echo "RED ($sdiffs/$stotal differ)"; FAIL=$((FAIL+1)); fi
  fi
else
  echo "(self chain skipped: --fast)"
fi

# 5 — crossings, build-only: the full self per target + a hello smoke
# (hello trips arm64 emit regressions in seconds; the self-build takes minutes)
# smoke cap is 90s: the broken arm64 emitters grind ~60s before hitting their
# natural memory-fault abort — a 10s cap would report a timeout, not the fault
# (--wasm skips this whole section: native targets are not this round's work,
# and their red is expected, not a verdict)
if [ "$WASM" = 0 ]; then
  for t in arm64-mac amd64-linux arm64-linux; do
    rm -f $G/hello_${t}
    leg "cross-$t-smoke" cross_smoke "$t" $G/hello_${t}
  done
  for t in arm64-mac amd64-linux arm64-linux; do
    rm -f $G/self_${t}
    leg "cross-$t-self" cross_self "$t" $G/self_${t}
  done
fi

# 6 — diagnostics parity
printf '%-24s' diag-parity
if [ ! -f $G/m.wasm ]; then
  no_mirror diag-parity
else
ddiffs=0; dtotal=0
for d in tests/diag/d*.rho; do
  name=$(basename "$d" .rho)
  dtotal=$((dtotal+1))
  boot_msg=$(run_t 10 ./build/rho-boot check "$d" 2>&1 >/dev/null | strip_dbg)
  self_msg=$(wasmtime_run 10 $G/m.wasm check "$d" 2>&1 >/dev/null | strip_dbg)
  if [ "$boot_msg" != "$self_msg" ]; then
    echo "diag $name: messages differ"
    ddiffs=$((ddiffs+1))
  fi
done
if [ "$ddiffs" = 0 ]; then echo "GREEN ($dtotal cases)"; PASS=$((PASS+1))
else echo "RED ($ddiffs/$dtotal differ)"; FAIL=$((FAIL+1)); fi
fi

echo "== gate: $PASS green, $FAIL red =="
[ "$FAIL" = 0 ]
