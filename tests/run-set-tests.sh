#!/bin/zsh
# --set overrides: the widened type face (bool, integer widths, floats,
# string), range refusal = exit 2. Reuses the t06 corpus program.
set -u
RHO=${RHO:-./build/rho}
f=corpus/t06_params_widen.rho
pass=0; fail=0
check() { # desc, expected-stdout, sets...
  local desc=$1 want=$2; shift 2
  local got rc
  got=$("$RHO" run "$f" "$@" 2>/dev/null); rc=$?
  if [ "$rc" -eq 0 ] && [ "$got" = "$want" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL $desc: rc=$rc got=[$got] want=[$want]"
  fi
}
check "i32"      "w=99 m=true r=0.5 n=plain b=100"  --set WIDTH=99
check "bool-f"   "w=4 m=false r=0.5 n=plain b=100"  --set MODE=false
check "f64"      "w=4 m=true r=2.75 n=plain b=100"  --set RATE=2.75
check "string"   "w=4 m=true r=0.5 n=set! b=100"    --set NAME=set!
check "u64-wide" "w=4 m=true r=0.5 n=plain b=18446744073709551615" --set BIG=18446744073709551615
# range refusal: i32 can't hold 3000000000
"$RHO" run "$f" --set WIDTH=3000000000 >/dev/null 2>&1
if [ $? -eq 2 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL refusal"; fi
echo "set tests: $pass pass, $fail fail"
[ "$fail" -eq 0 ]
