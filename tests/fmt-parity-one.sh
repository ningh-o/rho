#!/bin/zsh
# one program's fmt parity: boot vs the mirror, diff shown
set -u
RHO=${RHO:-./build/rho}
name=$1
src=$(cat "tests/suites/programs/$name.rho")
mods=""
for mf in tests/suites/programs/geom/*.rho(N) tests/suites/programs/geom/*/*.rho(N) tests/suites/programs/web/*.rho(N) tests/suites/programs/pk/*.rho(N) tests/suites/programs/pk/*/*.rho(N) tests/suites/programs/pk/*/*/*.rho(N); do
  [ -f "$mf" ] && mods="$mods@MOD@ ${mf#tests/suites/programs/}
$(cat "$mf")
"
done
for mf in std/*.rho(N) std/*/*.rho(N) std/*/*/*.rho(N); do
  [ -f "$mf" ] && mods="$mods@MOD@ $mf
$(cat "$mf")
"
done
"$RHO" fmt "tests/suites/programs/$name.rho" >/tmp/p1-boot.txt 2>&1
"$RHO" build libs/compiler/main.rho -o /tmp/p1.wasm \
    --set "SRC=$src" --set "MODS=$mods" --set FMT=1 >/tmp/p1.log 2>&1
if [ $? -ne 0 ]; then
  echo "MIRROR BUILD FAIL:"; head -5 /tmp/p1.log
  exit 1
fi
wasmtime /tmp/p1.wasm >/tmp/p1-mir.txt 2>/dev/null
if cmp -s /tmp/p1-boot.txt /tmp/p1-mir.txt; then
  echo "ok $name"
else
  echo "DIFF $name:"
  diff /tmp/p1-boot.txt /tmp/p1-mir.txt | head -${2:-20}
fi
