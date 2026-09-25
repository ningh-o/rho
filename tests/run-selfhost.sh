#!/bin/zsh
# T2.1: the self-hosting skeleton — boot compiles libs/compiler (rho),
# the compiler compiles the hello source (via the SRC build param),
# wat2wasm assembles, wasmtime runs: hello, self.
set -u
RHO=${RHO:-./build/rho}
# sources ride the SRC build parameter (§7): compile-time consts
run_one() { # name, src, expected-stdout
  local name=$1 src=$2 want=$3
  "$RHO" build libs/compiler/main.rho -o /tmp/rhoc-$name.wasm --set "SRC=$src" \
    >/tmp/rhoc-$name.log 2>&1
  if [ $? -ne 0 ]; then
    echo "FAIL selfhost/$name: boot could not build the compiler"
    head -3 /tmp/rhoc-$name.log
    FAILED=1
    return
  fi
  wasmtime /tmp/rhoc-$name.wasm >/tmp/hello-$name.wat 2>/dev/null
  wat2wasm /tmp/hello-$name.wat -o /tmp/hi-$name.wasm 2>/dev/null
  local got rc
  got=$(wasmtime /tmp/hi-$name.wasm 2>/dev/null)
  rc=$?
  if [ "$rc" -eq 0 ] && [ "$got" = "$want" ]; then
    echo "  $name: ok"
  else
    echo "FAIL selfhost/$name: rc=$rc got=[$got] want=[$want]"
    FAILED=1
  fi
}
FAILED=0
run_one hello 'fn main() -> i32 { printf("hello, self\n"); return 0; }' 'hello, self'
run_one pieces 'fn main() -> i32 { printf("a{}b{}c\n", "XY", "Z"); return 0; }' 'aXYbZc'
run_one ints 'fn main() -> i32 { printf("n={} and {}\n", 42, 7); return 0; }' 'n=42 and 7'
if [ "$FAILED" -eq 0 ]; then
  echo "selfhost: ok (boot → rho compiler → program → run)"
else
  exit 1
fi
