#!/bin/zsh
# fmt parity, full layer: boot's fmt is the judge — the self-hosted
# formatter must reproduce its bytes on EVERY corpus program (the whole
# grammar, not the subset the fmt-self suite pins), and its own output
# must be a fixpoint. Two laws, one runner:
#
#   parity     fmt_m(x)   == fmt_b(x)      (byte-identical, per program)
#   fixpoint   fmt_m(fmt_m(x)) == fmt_m(x) — wherever boot's own
#              roundtrip is stable. Where boot itself drifts (a
#              same-line tail comment cannot survive its own move),
#              the same-sound law rides above the fixpoint law: the
#              mirror follows boot byte for byte at the second hop too
#              (fmt_m(fmt_m(x)) == fmt_b(fmt_b(x))).
set -u
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
RHO=${RHO:-./build/rho}
pass=0; fail=0; failed=""
# the module tree rides MODS for the mirror side (a baked compiler
# resolves use paths against it — boot reads the same tree from disk):
# the corpus's package dirs, plus the reserved std/ tree, repo-relative
mods=""
for mf in tests/suites/programs/geom/*.rho(N) tests/suites/programs/geom/*/*.rho(N) tests/suites/programs/web/*.rho(N) tests/suites/programs/pk/*.rho(N) tests/suites/programs/pk/*/*.rho(N) tests/suites/programs/pk/*/*/*.rho(N); do
  if [ -f "$mf" ]; then
    mods="$mods@MOD@ ${mf#tests/suites/programs/}
$(cat "$mf")
"
  fi
done
for mf in std/*.rho(N) std/*/*.rho(N) std/*/*/*.rho(N); do
  if [ -f "$mf" ]; then
    mods="$mods@MOD@ $mf
$(cat "$mf")
"
  fi
done

for f in tests/suites/programs/*.rho(N); do
  name=$(basename "$f" .rho)
  src=$(cat "$f")
  # boot: the judge, twice — its own roundtrip stability decides which
  # fixpoint law applies to this program
  perl -e 'alarm 60; exec @ARGV' -- "$RHO" fmt "$f" >$T/boot1-$name.txt 2>/dev/null
  # boot's second hop must reparse where the module tree resolves: a
  # fmt output carried to $T loses the modules and boot refuses its
  # own output (an artifact, not a drift). The scratch file rides a
  # dot name — zsh's *.rho glob skips dotfiles, so no later run sees it
  # — and is removed the moment its hop is done
  cp $T/boot1-$name.txt tests/suites/programs/.fmtck-$name.rho
  perl -e 'alarm 60; exec @ARGV' -- "$RHO" fmt \
      tests/suites/programs/.fmtck-$name.rho >$T/boot2-$name.txt 2>/dev/null
  rm -f tests/suites/programs/.fmtck-$name.rho
  if ! cmp -s $T/boot1-$name.txt $T/boot2-$name.txt; then
    bootfix=0
  else
    bootfix=1
  fi
  # the mirror: the compiler baked with this program as SRC, fmt mode
  if ! perl -e 'alarm 120; exec @ARGV' -- "$RHO" build libs/compiler/main.rho -o $T/mc1-$name.wasm \
      --set "SRC=$src" --set "MODS=$mods" --set FMT=1 >$T/mc1-$name.log 2>&1; then
    echo "DIFF $name: the mirror refuses the program"
    fail=$((fail+1)); failed="$failed $name:build"; continue
  fi
  perl -e 'alarm 60; exec @ARGV' -- wasmtime $T/mc1-$name.wasm >$T/mir1-$name.txt 2>/dev/null
  # hop two: the mirror re-formats its own output (the fixpoint probe)
  src2=$(cat $T/mir1-$name.txt)
  if ! perl -e 'alarm 120; exec @ARGV' -- "$RHO" build libs/compiler/main.rho -o $T/mc2-$name.wasm \
      --set "SRC=$src2" --set "MODS=$mods" --set FMT=1 >$T/mc2-$name.log 2>&1; then
    echo "DIFF $name: the mirror refuses its own fmt output"
    fail=$((fail+1)); failed="$failed $name:rebuild"; continue
  fi
  perl -e 'alarm 60; exec @ARGV' -- wasmtime $T/mc2-$name.wasm >$T/mir2-$name.txt 2>/dev/null
  # the verdict ladder: parity first (hop one), then the fixpoint law
  # at hop two — a parity failure reports as parity, never masked by
  # the hop-two outcome
  verdict=""
  if ! cmp -s $T/mir1-$name.txt $T/boot1-$name.txt; then
    verdict="DIFF parity"
  elif [ "$bootfix" -eq 1 ]; then
    if ! cmp -s $T/mir1-$name.txt $T/mir2-$name.txt; then
      verdict="DIFF fixpoint"
    fi
  else
    # boot drifts at hop two: the mirror must drift to boot's exact
    # bytes (the same-sound law, above the fixpoint law)
    if ! cmp -s $T/mir2-$name.txt $T/boot2-$name.txt; then
      verdict="DIFF same-sound"
    fi
  fi
  if [ -z "$verdict" ]; then
    pass=$((pass+1))
    echo "ok $name"
  else
    fail=$((fail+1)); failed="$failed $name:${verdict#DIFF }"
    echo "$verdict $name"
  fi
done
echo "fmt parity: $pass pass, $fail fail (pinned floor: 96)"
if [ "$fail" -gt 0 ]; then
  echo "failed:$failed"
  exit 1
fi
