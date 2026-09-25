#!/bin/zsh
# T2.1: the self-hosting skeleton — boot compiles libs/compiler (rho),
# the compiler compiles the hello source (via the SRC build param),
# wat2wasm assembles, wasmtime runs: hello, self.
set -u
RHO=${RHO:-./build/rho}
SRC='fn main() -> i32 { printf("hello, self\n"); return 0; }'
pass=0; fail=0
if ! "$RHO" build libs/compiler/main.rho -o /tmp/rhoc.wasm >/tmp/rhoc.log 2>&1; then
  echo "FAIL selfhost: boot could not build the compiler"; head -3 /tmp/rhoc.log
  exit 1
fi
# the source rides the SRC build parameter (§7): a compile-time const
"$RHO" build libs/compiler/main.rho -o /tmp/rhoc.wasm --set "SRC=$SRC" >/dev/null 2>&1
wasmtime /tmp/rhoc.wasm >/tmp/hello.wat 2>/dev/null
wat2wasm /tmp/hello.wat -o /tmp/hello-self.wasm 2>/dev/null
got=$(wasmtime /tmp/hello-self.wasm 2>/dev/null)
rc=$?
if [ "$rc" -eq 0 ] && [ "$got" = "hello, self" ]; then
  echo "selfhost: ok (boot → rho compiler → hello → run)"
else
  echo "FAIL selfhost: rc=$rc got=[$got]"
  exit 1
fi
