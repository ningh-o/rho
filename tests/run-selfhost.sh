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
  got=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime /tmp/hi-$name.wasm 2>/dev/null)
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
run_one exprs 'fn main() -> i32 { let x = 6 * 7; let y = x - 2; printf("x={} y={} sum={}\n", x, y, x + y); return 0; }' 'x=42 y=40 sum=82'
run_one prec 'fn main() -> i32 { printf("{} {}\n", 2 + 3 * 4, (2 + 3) * 4); return 0; }' '14 20'
run_one flow 'fn main() -> i32 { let mut i = 0; let mut sum = 0; while i < 10 { if i % 2 == 0 { sum = sum + i; } else { sum = sum - 1; } i = i + 1; } printf("sum={} i={}\n", sum, i); return 0; }' 'sum=15 i=10'
run_one fns 'fn double(x: i64) -> i64 { return x * 2; } fn add3(a: i64, b: i64, c: i64) -> i64 { return a + b + c; } fn main() -> i32 { let d = double(21); let t = add3(d, 10, 1); printf("d={} t={}\n", d, t); return 0; }' 'd=42 t=53'
run_one fib 'fn fib(n: i64) -> i64 { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); } fn main() -> i32 { printf("fib(10)={}\n", fib(10)); return 0; }' 'fib(10)=55'
if [ "$FAILED" -eq 0 ]; then
  echo "selfhost: ok (boot → rho compiler → program → run)"
else
  exit 1
fi
