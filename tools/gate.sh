#!/bin/sh
# The bootstrap gate: one command, one verdict per leg.
#
#   tools/gate.sh --wasm     this round's acceptance: the wasm legs, no
#                            native crossings (~2 min measured)
#   tools/gate.sh            full run (the wasm legs + the crossings)
#   tools/gate.sh --fast     full run minus the self chains (~4 min)
#
# Self-sufficient: if the oracle (build/rho-boot) is missing or older than
# its sources (boot/src, boot/prelude, tools/embed.py), the gate rebuilds
# it first. Every rho invocation runs under a wall-clock cap (run_t /
# wasmtime_run), so no leg can hang the gate.
#
# Legs (functional equivalence is the law: same program in, same stdout +
# exit code out — never a byte compare of two compilers' wasm):
#   1 boot-selftest    the oracle is healthy (goldens + diag)
#   2 build-mirror     the pinned seed builds the mirror (the frontier leg:
#                      the mirror's own sources speak the seed's language)
#   3 corpus-diff      every corpus program: boot-era entries boot-built vs
#                      mirror-built with identical stdout + exit; seed-era
#                      entries (boot cannot parse them) graded against their
#                      committed .out goldens; boot runs stay the boot-era
#                      corpus goldens
#   4 self-chain       the full ring: mirror builds itself -> child; child
#                      builds itself -> grandchild; the grandchild rebuilds
#                      every corpus program and matches the golden in
#                      behavior
#   5 seed-chain       the pinned self-built seed (boot/rho-seed.wasm)
#                      builds the mirror: the frontier every run; the leg
#                      first re-checks the pin against the SHA-256 slot
#                      docs/bootstrap.md records (tools/reseed.sh --check)
#   6 web-root         the mirror builds its own root with --set
#                      native=false (web.wasm): version, hello build, the
#                      native refusal, and the fold's fingerprints — the
#                      size win, no native-module qualified name anywhere
#                      in the artifact, a smaller function section (the
#                      six native backend modules are gone)
#   7 web-chain        the web ring: web.wasm builds itself -> child ->
#                      grandchild, all --set native=false; the grandchild
#                      matches the web behavior law, stays pruned, and
#                      rebuilds every corpus program to the goldens
#   8 crossings        per image target: mirror builds itself + a hello
#                      smoke, each image statically structure-checked with
#                      tools/image-check.py (exec natively is never done
#                      here — a corrupt image wedges the kernel,
#                      SIGKILL-proof; the structure probe is the evidence)
#   9 diag-parity      tests/diag/*.rho: mirror's diagnostics are byte-
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
  # stdin is /dev/null for every guest: corpus programs that read stdin
  # (read_line) must see EOF, never an inherited tty that would block
  perl -e 'alarm shift; exec @ARGV or die "wasmtime_run: cannot exec $ARGV[0]: $!\n"' "$t" \
    sh -c 'wasmtime run --dir . "$@" </dev/null' run "$@"
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

# wasm_funcs <file> — the count of entries in the artifact's function
# section (a static walk: custom sections aside, section id 3 carries one
# ULEB128 vector length and nothing else worth parsing). The fold's
# structural evidence: the six native backend modules a web build drops
# are a few hundred functions gone (measured 2026-09-24: 1158 full vs
# 944 web).
wasm_funcs() {
  python3 - "$1" <<'PYEOF'
import sys
b = open(sys.argv[1], "rb").read()
if b[:4] != b"\x00asm":
    sys.exit(f"wasm_funcs: {sys.argv[1]}: not a wasm module")
def leb(p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7F) << s
        if not x & 0x80:
            return r, p
        s += 7
p = 8
while p < len(b):
    sid = b[p]; p += 1
    size, p = leb(p)
    if sid == 3:
        n, _ = leb(p)
        print(n)
        sys.exit(0)
    p += size
sys.exit(f"wasm_funcs: {sys.argv[1]}: no function section")
PYEOF
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
# and must parse as that target's image (tools/image-check.py: static
# structure only — a corrupt image is never exec'd here)
cross_smoke() {
  wasmtime_run 90 $G/m.wasm build corpus/001_hello.rho --target "$1" -o "$2" &&
    [ -f "$2" ] && run_t 10 python3 tools/image-check.py "$2" "$1" >/dev/null
}

# cross_self <target> <image> — the full self-build for one native target,
# structure-checked like the smoke
cross_self() {
  wasmtime_run 900 $G/m.wasm build libs/compiler/cli.rho --target "$1" -o "$2" &&
    [ -f "$2" ] && run_t 10 python3 tools/image-check.py "$2" "$1" >/dev/null
}

echo "== rho bootstrap gate $(date '+%H:%M:%S') =="

# 1 — the oracle
leg boot-selftest run_t 10 ./build/rho-boot selftest

# 2 — the mirror. Since the dot round the mirror's own sources speak the
# pinned seed's language (dots, `as`, `pub use`, and since the params
# round the merged root's own `if (native)` fold), which boot — frozen —
# cannot compile: the seed builds the mirror, and this leg enforces the
# frontier (docs/bootstrap.md, the re-pinning ritual's step 3). The old
# image is removed first: a failed build must not leave the mirror-side
# legs grading last run's compiler.
rm -f $G/m.wasm
leg build-mirror wasmtime_run 900 boot/rho-seed.wasm build libs/compiler/cli.rho --target wasm32-wasi -o $G/m.wasm

# every leg below grades the mirror THIS run produced; without one there is
# nothing to compare and the leg goes RED saying so — it never falls back
# to a stale image
no_mirror() {
  printf '%-24s' "$1"
  echo "RED (no fresh mirror: build-mirror failed)"
  FAIL=$((FAIL+1))
}

# 3 — corpus differential (wasm): two eras, one law. Boot-era programs:
# boot builds and runs them (the frozen oracle) and the mirror must match
# that behavior. Seed-era programs (the dot-round corpus, e.g. 032): boot
# cannot even parse them, so the committed .out golden — generated by the
# self-built chain — is the oracle the mirror must meet.
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
  run_t 10 ./build/rho-boot build "$src" --target wasm32-wasi -o $G/corpus/${name}.boot.wasm >/dev/null 2>&1
  if [ -f $G/corpus/${name}.boot.wasm ]; then
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
  else
    wasmtime_run 10 $G/m.wasm build "$src" --target wasm32-wasi -o $G/corpus/${name}.self.wasm >/dev/null 2>&1 || { echo "corpus $name: mirror build failed"; diffs=$((diffs+1)); continue; }
    [ -f $G/corpus/${name}.self.wasm ] || { echo "corpus $name: mirror produced no artifact"; diffs=$((diffs+1)); continue; }
    sout=$(wasmtime_run 10 $G/corpus/${name}.self.wasm 2>/dev/null); src_rc=$?
    sout=$(printf '%s' "$sout" | strip_dbg)
    want_out=$(cat "corpus/$name.out" 2>/dev/null)
    want_rc=$(sed -n 's|^// exit: ||p' "$src" | head -1)
    [ -n "$want_rc" ] || want_rc=0
    printf '%s' "$want_out" > $G/corpus/${name}.golden.out
    printf '%s\n' "$want_rc" > $G/corpus/${name}.golden.rc
    if [ "$sout" != "$want_out" ] || [ "$src_rc" != "$want_rc" ]; then
      echo "corpus $name: seed-era golden differs (want rc=$want_rc mirror rc=$src_rc)"
      diffs=$((diffs+1))
    fi
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
  elif ! wasmtime_run 900 $G/m.wasm build libs/compiler/cli.rho --target wasm32-wasi -o $G/child.wasm >/dev/null 2>&1 || [ ! -f $G/child.wasm ]; then
    echo "RED (child build failed)"; FAIL=$((FAIL+1))
  elif ! wasmtime_run 900 $G/child.wasm build libs/compiler/cli.rho --target wasm32-wasi -o $G/grandchild.wasm >/dev/null 2>&1 || [ ! -f $G/grandchild.wasm ]; then
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

# 5a — the pinned seed: the feature frontier. The seed is a frozen
# self-built compiler; the mirror's sources may use anything the seed
# accepts. While the mirror still fits boot's subset, the seed's build
# of it is byte-identical to boot's (same compiler, same source); once
# the mirror grows past the pin, the leg grades behavior instead and
# says so — the ritual for re-pinning lives in docs/bootstrap.md.
printf '%-24s' seed-chain
# the pin is checked against the SHA-256 slot docs/bootstrap.md records
# (tools/reseed.sh --check) BEFORE anything else: a rotted record would
# make every verdict below mean nothing
pinmsg=$(sh tools/reseed.sh --check 2>&1)
pinrc=$?
if [ "$pinrc" != 0 ]; then
  echo RED
  printf '%s\n' "$pinmsg" | tail -2 | sed 's/^/    /'
  FAIL=$((FAIL+1))
elif [ ! -f boot/rho-seed.wasm ]; then
  echo "RED (no pinned seed at boot/rho-seed.wasm)"
  FAIL=$((FAIL+1))
else
  seedok=0
  seednote=""
  sv=$(wasmtime_run 10 boot/rho-seed.wasm --version 2>/dev/null)
  if [ "$sv" = "rho 0.4.0" ]; then
    rm -f $G/seed-m.wasm
    if wasmtime_run 900 boot/rho-seed.wasm build libs/compiler/cli.rho --target wasm32-wasi -o $G/seed-m.wasm >/dev/null 2>&1 && [ -f $G/seed-m.wasm ]; then
      # the seed building the mirror IS the child build; when the
      # self-chain leg ran, grade against its child.wasm
      if [ -f $G/child.wasm ] && cmp -s $G/seed-m.wasm $G/child.wasm; then
        seedok=1
        seednote="current (byte-identical to the chain's child)"
      else
        rm -rf $G/seedhello.wasm
        if wasmtime_run 90 $G/seed-m.wasm build corpus/001_hello.rho --target wasm32-wasi -o $G/seedhello.wasm >/dev/null 2>&1; then
          sh=$(wasmtime_run 10 $G/seedhello.wasm 2>/dev/null)
          if [ "$sh" = "hello, world" ]; then
            seedok=1
            seednote="older pin, still builds the mirror (behavior-graded)"
          fi
        fi
      fi
    fi
  fi
  if [ "$seedok" = 1 ]; then
    echo "GREEN ($seednote)"
    PASS=$((PASS+1))
  else
    echo "RED"
    FAIL=$((FAIL+1))
  fi
fi

# 5b — the web configuration: the browser artifact, the one package root
# compiled with `--set native=false`. The fold drops the native pipe and
# its six backends from the artifact; a native target must refuse with
# exit 2 and the one-line reason, exactly like boot refuses what it does
# not ship. This run's own mirror (m.wasm) is the builder: the leg thus
# exercises the mirror's own --set and folding on itself.
printf '%-24s' web-root
if [ ! -f $G/m.wasm ]; then
  no_mirror web-root
else
  rm -f $G/web.wasm
  webok=0
  websize=0
  if wasmtime_run 900 $G/m.wasm build libs/compiler/cli.rho --target wasm32-wasi --set native=false -o $G/web.wasm >/dev/null 2>&1 && [ -f $G/web.wasm ]; then
    websize=$(wc -c < $G/web.wasm | tr -d ' ')
    fullsize=$(wc -c < $G/m.wasm | tr -d ' ')
    vout=$(wasmtime_run 10 $G/web.wasm --version 2>/dev/null)
    rm -rf $G/whello.wasm
    wasmtime_run 90 $G/web.wasm build corpus/001_hello.rho --target wasm32-wasi -o $G/whello.wasm >/dev/null 2>&1
    hrun=$(wasmtime_run 10 $G/whello.wasm 2>/dev/null)
    nat=$(wasmtime_run 30 $G/web.wasm build corpus/001_hello.rho --target arm64-mac -o $G/never.out 2>&1)
    # the refusal: exit code 2, one clear line (rc read via a re-run)
    wasmtime_run 30 $G/web.wasm build corpus/001_hello.rho --target arm64-mac -o $G/never.out >/dev/null 2>&1
    natrc=$?
    # the fold's fingerprints: the six native backend modules are gone
    # from the artifact — their diag strings ("rho: macho64: …") absent,
    # the function section visibly smaller, the file strictly lighter.
    # The positive control keeps the greps honest: if the FULL artifact
    # ever loses the string too, the leg re-points instead of passing
    # vacuously.
    fgm=$(grep -ac macho64 $G/m.wasm); fgw=$(grep -ac macho64 $G/web.wasm)
    few=$(grep -ac elf64 $G/web.wasm)
    ffnfull=$(wasm_funcs $G/m.wasm); ffnweb=$(wasm_funcs $G/web.wasm)
    if [ "$vout" = "rho 0.4.0" ] && [ "$hrun" = "hello, world" ] && [ "$natrc" = 2 ] &&
       printf '%s' "$nat" | grep -q "not linked in this build" &&
       [ "$fgm" -ge 1 ] && [ "$fgw" -eq 0 ] && [ "$few" -eq 0 ] &&
       [ -n "$ffnfull" ] && [ -n "$ffnweb" ] && [ "$ffnweb" -lt "$ffnfull" ] &&
       [ "$websize" -lt "$fullsize" ]; then
      webok=1
    fi
  fi
  if [ "$webok" = 1 ]; then
    echo "GREEN ($(printf '%s' $websize | awk '{printf "%.1f", $1/1048576}') MiB vs $(printf '%s' $fullsize | awk '{printf "%.1f", $1/1048576}') MiB full; $ffnweb fns vs $ffnfull; native diag strings absent)"
    PASS=$((PASS+1))
  else
    echo "RED (v=${vout:-?} hello=${hrun:-?} rc=${natrc:-?} macho64 full=${fgm:-?} web=${fgw:-?} elf64 web=${few:-?} fns ${ffnweb:-?}/${ffnfull:-?} bytes ${websize:-?}/${fullsize:-?})"
    FAIL=$((FAIL+1))
  fi
fi

# 7 — the web ring: the web configuration's own bootstrap loop. web.wasm
# builds itself -> child -> grandchild, all --set native=false. The
# grandchild must satisfy the web behavior law (version, hello, the
# native refusal), stay pruned (the fold's fingerprints — same evidence
# web-root uses), and rebuild every corpus program to the goldens
# corpus-diff laid down: the web compiler is a full citizen of the same
# functional-equivalence law, not a shrugged-off subset.
printf '%-24s' web-chain
if [ ! -f $G/web.wasm ] || [ ! -f $G/m.wasm ]; then
  echo "RED (no fresh web root: web-root failed)"
  FAIL=$((FAIL+1))
else
  rm -f $G/web-child.wasm $G/web-grandchild.wasm
  if ! wasmtime_run 900 $G/web.wasm build libs/compiler/cli.rho --target wasm32-wasi --set native=false -o $G/web-child.wasm >/dev/null 2>&1 || [ ! -f $G/web-child.wasm ]; then
    echo "RED (web child build failed)"; FAIL=$((FAIL+1))
  elif ! wasmtime_run 900 $G/web-child.wasm build libs/compiler/cli.rho --target wasm32-wasi --set native=false -o $G/web-grandchild.wasm >/dev/null 2>&1 || [ ! -f $G/web-grandchild.wasm ]; then
    echo "RED (web grandchild build failed)"; FAIL=$((FAIL+1))
  else
    ringok=1; wnote=""
    # the web behavior law, on the grandchild
    wv=$(wasmtime_run 10 $G/web-grandchild.wasm --version 2>/dev/null)
    [ "$wv" = "rho 0.4.0" ] || { ringok=0; wnote="$wnote version"; }
    rm -f $G/wghello.wasm
    wasmtime_run 90 $G/web-grandchild.wasm build corpus/001_hello.rho --target wasm32-wasi -o $G/wghello.wasm >/dev/null 2>&1
    wh=$(wasmtime_run 10 $G/wghello.wasm 2>/dev/null)
    [ "$wh" = "hello, world" ] || { ringok=0; wnote="$wnote hello"; }
    rm -f $G/wgnever.out
    wasmtime_run 30 $G/web-grandchild.wasm build corpus/001_hello.rho --target arm64-mac -o $G/wgnever.out >/dev/null 2>&1
    wnrc=$?
    wnat=$(wasmtime_run 30 $G/web-grandchild.wasm build corpus/001_hello.rho --target arm64-mac -o $G/wgnever.out 2>&1)
    { [ "$wnrc" = 2 ] && printf '%s' "$wnat" | grep -q "not linked in this build"; } ||
      { ringok=0; wnote="$wnote refusal"; }
    # stays pruned
    [ "$(grep -ac macho64 $G/web-grandchild.wasm)" = 0 ] || { ringok=0; wnote="$wnote macho64"; }
    [ "$(grep -ac elf64 $G/web-grandchild.wasm)" = 0 ] || { ringok=0; wnote="$wnote elf64"; }
    wgfn=$(wasm_funcs $G/web-grandchild.wasm)
    mfn=$(wasm_funcs $G/m.wasm)
    { [ -n "$wgfn" ] && [ -n "$mfn" ] && [ "$wgfn" -lt "$mfn" ]; } ||
      { ringok=0; wnote="$wnote fns"; }
    # the corpus, rebuilt by the web grandchild to the shared goldens
    mkdir -p $G/webchain
    wdiffs=0; wtotal=0
    for src in corpus/*.rho; do
      name=$(basename "$src" .rho)
      wtotal=$((wtotal+1))
      goutf=$G/corpus/${name}.golden.out; grcf=$G/corpus/${name}.golden.rc
      if [ ! -f "$goutf" ] || [ ! -f "$grcf" ]; then
        echo "webchain $name: no golden (corpus-diff failed?)"
        wdiffs=$((wdiffs+1)); continue
      fi
      rm -f $G/webchain/${name}.wasm
      if ! wasmtime_run 10 $G/web-grandchild.wasm build "$src" --target wasm32-wasi -o $G/webchain/${name}.wasm >/dev/null 2>&1 || [ ! -f $G/webchain/${name}.wasm ]; then
        echo "webchain $name: grandchild-web build failed"
        wdiffs=$((wdiffs+1)); continue
      fi
      cout=$(wasmtime_run 10 $G/webchain/${name}.wasm 2>/dev/null); crc=$?
      cout=$(printf '%s' "$cout" | strip_dbg)
      if [ "$cout" != "$(cat "$goutf")" ] || [ "$crc" != "$(cat "$grcf")" ]; then
        echo "webchain $name: behavior differs from golden (rc=$crc golden rc=$(cat "$grcf"))"
        wdiffs=$((wdiffs+1))
      fi
    done
    [ "$wdiffs" = 0 ] || { ringok=0; wnote="$wnote corpus($wdiffs/$wtotal)"; }
    if [ "$ringok" = 1 ]; then
      echo "GREEN ($wtotal programs via the web grandchild; $wgfn fns vs $mfn full)"
      PASS=$((PASS+1))
    else
      echo "RED ($wnote)"
      FAIL=$((FAIL+1))
    fi
  fi
fi

# 9 — diagnostics parity
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
