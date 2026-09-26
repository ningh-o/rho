#!/bin/zsh
# robustness: hostile inputs must never crash or hang the compiler.
# Every file under tests/robust/ runs through `rho check` and
# `rho build` under a wall-clock cap; the only acceptable outcomes
# are exit 0 (accepted) or exit 1 (clean diagnostic). A signal exit
# (segfault, 128+n) or a timeout is a FAIL naming the file.
set -u
RHO=${RHO:-./build/rho}
D=$(dirname "$0")/robust
CAP=10
FAILED=0
N=0

probe_one() { # file
  local f=$1 verb=$2 out rc
  N=$((N+1))
  out=$(perl -e 'alarm $ARGV[0]; exec @ARGV[1..$#ARGV]' $CAP "$RHO" $verb "$f" 2>&1)
  rc=$?
  if [ $rc -gt 1 ]; then
    echo "FAIL robust/$verb/$(basename $f): exit $rc (crash or timeout)"
    printf '%s\n' "$out" | head -3
    FAILED=1
  fi
}

for f in $D/*.rho(N); do
  probe_one "$f" check
  probe_one "$f" build
done
echo "robust: $N probes, $([ $FAILED -eq 0 ] && echo all clean || echo FAILURES)"
exit $FAILED
