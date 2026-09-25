#!/bin/zsh
# differential seed (T3.1's first leg): the same programs through boot
# and through the self-hosted compiler must behave identically
# (stdout). The self-hosted surface is the run-selfhost subset; it
# grows until the full corpus differential closes it.
set -u
RHO=${RHO:-./build/rho}
FAILED=0
diff_one() { # name, src
  local name=$1 src=$2
  printf '%s\n' "$src" > /tmp/diff-$name.rho
  local bgot brc sgot src_rc
  bgot=$("$RHO" run /tmp/diff-$name.rho 2>/dev/null)
  brc=$?
  "$RHO" build libs/compiler/main.rho -o /tmp/diffc-$name.wasm \
    --set "SRC=$src" >/dev/null 2>&1
  wasmtime /tmp/diffc-$name.wasm >/tmp/diff-$name.wat 2>/dev/null
  wat2wasm /tmp/diff-$name.wat -o /tmp/diff-$name.self.wasm 2>/dev/null
  sgot=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime \
    /tmp/diff-$name.self.wasm 2>/dev/null)
  src_rc=$?
  if [ "$brc" -eq "$src_rc" ] && [ "$bgot" = "$sgot" ]; then
    echo "  $name: boot==self rc=$brc [$bgot]"
  else
    echo "FAIL diff/$name: boot rc=$brc=[$bgot] self rc=$src_rc=[$sgot]"
    FAILED=1
  fi
}
diff_one hello 'fn main() -> i32 { printf("hello, self\n"); return 0; }'
diff_one ints 'fn main() -> i32 { printf("n={} and {}\n", 42, 7); return 0; }'
diff_one exprs 'fn main() -> i32 { let x = 6 * 7; let y = x - 2; printf("x={} y={} sum={}\n", x, y, x + y); return 0; }'
diff_one flow 'fn main() -> i32 { let mut i = 0; let mut sum = 0; while i < 10 { if i % 2 == 0 { sum = sum + i; } else { sum = sum - 1; } i = i + 1; } printf("sum={} i={}\n", sum, i); return 0; }'
diff_one fib 'fn fib(n: i64) -> i64 { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); } fn main() -> i32 { printf("fib(10)={}\n", fib(10)); return 0; }'
diff_one strs 'fn main() -> i32 { let a = "one"; let b = "two"; let c = a + "-" + b + "!"; printf("a={} b={} c={} all={}\n", a, b, c, "x" + "y"); return 0; }'
diff_one suite 'fn banner() -> i64 { printf("[banner]\n"); return 3; } fn main() -> i32 { let n = banner(); if n == 3 { printf("three\n"); } else if n == 4 { printf("four\n"); } else { printf("other\n"); } printf("neg={} len={}\n", 0 - 5, len("abcd")); banner(); return 7; }'
diff_one strparam 'fn greet(s: string) -> i64 { printf("hello {}! len={}\n", s, len(s)); return len(s) as i64; } fn main() -> i32 { let n = greet("world"); let t = greet("rho"); let big: i64 = 3000000000; printf("n={} t={} big={} wide={}\n", n, t, big, (big as i32) as i64); return 0; }'
if [ "$FAILED" -eq 0 ]; then
  echo "differential: ok (boot == self-hosted on the growing subset)"
else
  exit 1
fi
