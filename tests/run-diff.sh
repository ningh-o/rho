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
  if ! "$RHO" build libs/compiler/main.rho -o /tmp/diffc-$name.wasm \
      --set "SRC=$src" >/tmp/diff-$name.build 2>&1; then
    echo "FAIL diff/$name: boot could not build the compiler"
    head -3 /tmp/diff-$name.build
    FAILED=1
    return
  fi
  wasmtime /tmp/diffc-$name.wasm >/tmp/diff-$name.wat 2>/dev/null
  if ! wat2wasm /tmp/diff-$name.wat -o /tmp/diff-$name.self.wasm \
      2>/tmp/diff-$name.w2w; then
    echo "FAIL diff/$name: the self-hosted output does not assemble"
    head -3 /tmp/diff-$name.w2w
    FAILED=1
    return
  fi
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
diff_one bools 'fn main() -> i32 { let flag = true; let off = false; if flag { printf("on\n"); } if !off { printf("not off\n"); } if !flag { printf("no\n"); } else { printf("yes\n"); } if !off { printf("still\n"); } return 0; }'
diff_one compound 'fn main() -> i32 { let mut total = 0; let mut i = 0; while i < 5 { total += i; i += 1; } total *= 3; total -= 2; printf("total={} i={}\n", total, i); return 0; }'
diff_one streq 'fn pick(s: string) -> i64 { if s == "yes" { return 1; } if s == "no" { return 2; } return 0; } fn main() -> i32 { let a = pick("yes"); let b = pick("no"); let c = pick("maybe"); printf("a={} b={} c={}\n", a, b, c); if "x" == "x" { printf("eq\n"); } if "x" == "y" { printf("bug\n"); } return 0; }'
diff_one boolprint 'fn main() -> i32 { let r = "x" == "x"; let q = "x" == "y"; let n = 5; printf("r={} q={} n={}\\n", r, q, n); return 0; }'
diff_one bits 'fn main() -> i32 { let a: i64 = 12; let b: i64 = 10; printf("and={} or={} xor={}\n", a & b, a | b, a ^ b); let x: i64 = 1 << 4; printf("shl={} shr={}\n", x, x >> 2); let neg: i64 = 0 - 16; printf("sar={} not={} notnot={}\n", neg >> 2, ~a, ~~a); let mut m: i64 = 255; m &= 15; m |= 32; m ^= 4; printf("m={}\n", m); printf("mix={} eq={} neq={}\n", 1 | 2 & 3, (4 & 2) == 0, 5 ^ 0); let mut big: i64 = 1; big <<= 40; big >>= 2; printf("big={}\n", big); return 0; }'
diff_one labels 'fn main() -> i32 { let mut total = 0; let mut i = 0; outer: while i < 10 { i = i + 1; let mut j = 0; while j < 3 { j = j + 1; if j == 2 { continue outer; } if i == 7 { break outer; } total = total + 1; } } let mut k = 0; let mut hits = 0; scan: loop { k = k + 1; if k % 3 == 0 { continue scan; } if k > 8 { break scan; } hits = hits + 1; } printf("total={} i={} k={} hits={}\n", total, i, k, hits); return 0; }'
diff_one scirc 'fn pick(v: bool) -> i64 { if v { return 1; } return 2; } fn main() -> i32 { let mut i = 5; let mut safe = 0; while i >= 0 { if i == 0 || 100 / i > 10 { safe = safe + 1; } if i > 0 && 100 / i >= 20 { safe = safe + 10; } i = i - 1; } let a: i64 = 3; let b: i64 = 0; let both = a > 0 && b == 0; let either = a == 0 || b == 0; printf("safe={} both={} either={} cmparg={}\n", safe, both, either, pick(a == 3)); return 0; }'
diff_one breakcont 'fn main() -> i32 { let mut i = 0; let mut sum = 0; while true { i = i + 1; if i > 10 { break; } if i % 2 == 0 { continue; } sum = sum + i; } let mut n = 0; loop { n = n + 1; if n >= 5 { break; } } printf("sum={} i={} n={}\n", sum, i, n); return 0; }'
diff_one verbatim 'fn main() -> i32 { let v: string = """ab""cd"""; printf("[{}] {}\\n", v, len(v)); let multi: string = """
line1
line2"""; printf("[{}]\\n", multi); printf("len={}\\n", len(multi)); return 0; }'
if [ "$FAILED" -eq 0 ]; then
  echo "differential: ok (boot == self-hosted on the growing subset)"
else
  exit 1
fi
