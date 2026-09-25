#!/bin/zsh
# T2.1: the self-hosting skeleton — boot compiles libs/compiler (rho),
# the compiler compiles the hello source (via the SRC build param),
# wat2wasm assembles, wasmtime runs: hello, self.
set -u
RHO=${RHO:-./build/rho}
# sources ride the SRC build parameter (§7): compile-time consts
run_one() { # name, src, expected-stdout[, expected-rc]
  local name=$1 src=$2 want=$3 wantrc=${4:-0}
  "$RHO" build libs/compiler/main.rho -o /tmp/rhoc-$name.wasm --set "SRC=$src" \
    >/tmp/rhoc-$name.log 2>&1
  if [ $? -ne 0 ]; then
    echo "FAIL selfhost/$name: boot could not build the compiler"
    head -3 /tmp/rhoc-$name.log
    FAILED=1
    return
  fi
  wasmtime /tmp/rhoc-$name.wasm >/tmp/hello-$name.wat 2>/dev/null
  if ! wat2wasm /tmp/hello-$name.wat -o /tmp/hi-$name.wasm 2>/tmp/w2w-$name; then
    echo "FAIL selfhost/$name: the self-hosted output does not assemble"
    head -3 /tmp/w2w-$name
    FAILED=1
    return
  fi
  local got rc
  got=$(perl -e 'alarm 10; exec @ARGV' -- wasmtime /tmp/hi-$name.wasm 2>/dev/null)
  rc=$?
  if [ "$rc" -eq "$wantrc" ] && [ "$got" = "$want" ]; then
    echo "  $name: ok"
  else
    echo "FAIL selfhost/$name: rc=$rc want=$wantrc got=[$got] want=[$want]"
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
run_one strs 'fn main() -> i32 { let a = "one"; let b = "two"; let c = a + "-" + b + "!"; printf("a={} b={} c={} all={}\n", a, b, c, "x" + "y"); return 0; }' 'a=one b=two c=one-two! all=xy'
run_one suite 'fn banner() -> i64 { printf("[banner]\n"); return 3; } fn main() -> i32 { let n = banner(); if n == 3 { printf("three\n"); } else if n == 4 { printf("four\n"); } else { printf("other\n"); } printf("neg={} len={}\n", 0 - 5, len("abcd")); banner(); return 7; }' $'[banner]\nthree\nneg=-5 len=4\n[banner]' 7
run_one strparam 'fn greet(s: string) -> i64 { printf("hello {}! len={}\n", s, len(s)); return len(s) as i64; } fn main() -> i32 { let n = greet("world"); let t = greet("rho"); let big: i64 = 3000000000; printf("n={} t={} big={} wide={}\n", n, t, big, (big as i32) as i64); return 0; }' $'hello world! len=5\nhello rho! len=3\nn=5 t=3 big=3000000000 wide=-1294967296'
run_one bools 'fn main() -> i32 { let flag = true; let off = false; if flag { printf("on\n"); } if !off { printf("not off\n"); } if !flag { printf("no\n"); } else { printf("yes\n"); } printf("f={} nf={}\n", off, !flag); return 0; }' $'on\nnot off\nyes\nf=0 nf=0'
run_one compound 'fn main() -> i32 { let mut total = 0; let mut i = 0; while i < 5 { total += i; i += 1; } total *= 3; total -= 2; printf("total={} i={}\n", total, i); return 0; }' 'total=28 i=5'
run_one streq 'fn pick(s: string) -> i64 { if s == "yes" { return 1; } if s == "no" { return 2; } return 0; } fn main() -> i32 { let a = pick("yes"); let b = pick("no"); let c = pick("maybe"); printf("a={} b={} c={}\n", a, b, c); if "x" == "x" { printf("eq\n"); } if "x" == "y" { printf("bug\n"); } return 0; }' $'a=1 b=2 c=0\neq'
run_one boolprint 'fn main() -> i32 { let r = "x" == "x"; let q = "x" == "y"; let n = 5; printf("r={} q={} n={}\n", r, q, n); return 0; }' $'r=true q=false n=5'
run_one escapes 'fn main() -> i32 { let s: string = "a\rb\0c\x41\u{e9}\u{1F600}z"; printf("len={} r=[{}] x=[{}] u2=[{}] u4=[{}]\n", len(s), "A\rB", "\x42\x7a", "\u{4e2d}\u{6587}", "\u{1F600}!"); return 0; }' $'len=13 r=[A\rB] x=[Bz] u2=[中文] u4=[😀!]'
run_one widths 'fn main() -> i32 { let a8: i8 = 127; let m8: i8 = (-128); let a32: i32 = 2147483647; let m32: i32 = (-2147483648); let u: u32 = 4294967295; printf("i8: {} {}\n", a8 + 1, m8 - 1); printf("i32: {} {}\n", a32 + 1, m32 - 1); printf("u32+1={} eq={}\n", u + 1, u + 1 == 0); printf("mul={} div={} neg={}\n", m8 * (-1), m8 / (-1), -m8); return 0; }' $'i8: -128 127\ni32: -2147483648 2147483647\nu32+1=0 eq=true\nmul=-128 div=-128 neg=-128'
run_one fmtfn 'fn main() -> i32 { let n = 15; let s = format("[{}]", n); printf("{}\n", s); let t = format("{}-{}-{}", 1, "mid", 2 + 3); printf("{}\n", t); let u = format("plain"); printf("{}\n", u); let v = "n=" + format("{}", n); printf("{}\n", v); return 0; }' $'[15]\n1-mid-5\nplain\nn=15'
run_one strret 'fn join3(a: string, b: string, c: string) -> string { return a + b + c; } fn tag(s: string) -> string { if len(s) > 0 { return "[" + s + "]"; } printf("empty
"); return s; } fn main() -> i32 { let s = join3("x", "-", "y"); printf("[{}]
", s); printf("[{}]
", join3(s, "!", s)); let a = tag("q"); let b = tag(""); printf("{}{}
", a, b); return 0; }' $'[x-y]\n[x-y!x-y]\nempty\n[q]'
run_one cat 'fn main() -> i32 { let a: string = "con"; let b: string = "cat"; let mut c: string = ""; c = a + b + "!"; printf("[{}]\n", c); let d = a + "-" + (b + "?"); printf("[{}]\n", d); printf("[{}|{}]\n", a + b, c + c); let mut e = ""; e = e + "[" + a + "]"; printf("{}\n", e); return 0; }' $'[concat!]\n[con-cat?]\n[concat|concat!concat!]\n[con]'
run_one tco 'fn down(n: i64) -> i64 { if n == 0 { return 7; } return down(n - 1); } fn sumto(n: i64, acc: i64) -> i64 { if n == 0 { return acc; } return sumto(n - 1, acc + n); } fn main() -> i32 { printf("down={} sum={}\n", down(2000000), sumto(1000000, 0)); return 0; }' 'down=7 sum=500000500000'
run_one defer 'static mut LOG: i64 = 0; fn work() -> i64 { defer LOG += 1; defer LOG += 10; LOG += 100; return 0; } fn two() -> i64 { defer LOG += 1000; if LOG >= 0 { defer LOG += 5; } return LOG; } fn main() -> i32 { let r = work(); printf("log={} r={}\n", LOG, r); let t = two(); printf("log={} t={}\n", LOG, t); return 0; }' $'log=111 r=0\nlog=1116 t=116'
run_one statics 'const BASE: i64 = 10; const SCALED: i64 = BASE * 2 + 1; static mut HITS: i64 = 0; static mut MASK: i64 = 255; fn hit() -> i64 { HITS += 1; return HITS; } fn main() -> i32 { let a = hit(); let b = hit(); MASK &= 240; printf("a={} b={} s={} base={} mask={} lit={}\n", a, b, SCALED, BASE, MASK, BASE | 5); return 0; }' 'a=1 b=2 s=21 base=10 mask=240 lit=15'
run_one slices 'fn sum(xs: []i32) -> i32 { let mut total: i32 = 0; let mut i: usize = 0; while i < len(xs) { total += xs[i]; i += 1; } return total; } fn main() -> i32 { let xs: []i32 = make([]i32, 5); let mut i: usize = 0; while i < len(xs) { xs[i] = i as i32 * 2 + 2; i += 1; } let part: []i32 = xs[0..3]; printf("sum={} part={}\n", sum(xs), sum(part)); xs[0] = 100; printf("alias={}\n", part[0]); let tail: []i32 = xs[2..]; printf("tail={} tlen={}\n", sum(tail), len(tail)); let s: string = "hello"; printf("byte={} h={}\n", s[1], "hi"[0]); let head: string = s[0..3]; printf("head={} hl={}\n", head, len(head)); return 0; }' $'sum=30 part=12\nalias=100\ntail=24 tlen=3\nbyte=101 h=104\nhead=hel hl=3'
run_one oob 'fn main() -> i32 { let xs: []i32 = make([]i32, 3); return xs[7]; }' '' 101
run_one bits 'fn main() -> i32 { let a: i64 = 12; let b: i64 = 10; printf("and={} or={} xor={}\n", a & b, a | b, a ^ b); let x: i64 = 1 << 4; printf("shl={} shr={}\n", x, x >> 2); let neg: i64 = 0 - 16; printf("sar={} not={} notnot={}\n", neg >> 2, ~a, ~~a); let mut m: i64 = 255; m &= 15; m |= 32; m ^= 4; printf("m={}\n", m); printf("mix={} eq={} neq={}\n", 1 | 2 & 3, (4 & 2) == 0, 5 ^ 0); let mut big: i64 = 1; big <<= 40; big >>= 2; printf("big={}\n", big); return 0; }' $'and=8 or=14 xor=6\nshl=16 shr=4\nsar=-4 not=-13 notnot=12\nm=43\nmix=3 eq=true neq=5\nbig=274877906944'
run_one labels 'fn main() -> i32 { let mut total = 0; let mut i = 0; outer: while i < 10 { i = i + 1; let mut j = 0; while j < 3 { j = j + 1; if j == 2 { continue outer; } if i == 7 { break outer; } total = total + 1; } } let mut k = 0; let mut hits = 0; scan: loop { k = k + 1; if k % 3 == 0 { continue scan; } if k > 8 { break scan; } hits = hits + 1; } printf("total={} i={} k={} hits={}\n", total, i, k, hits); return 0; }' 'total=6 i=7 k=10 hits=6'
run_one scirc 'fn pick(v: bool) -> i64 { if v { return 1; } return 2; } fn main() -> i32 { let mut i = 5; let mut safe = 0; while i >= 0 { if i == 0 || 100 / i > 10 { safe = safe + 1; } if i > 0 && 100 / i >= 20 { safe = safe + 10; } i = i - 1; } let a: i64 = 3; let b: i64 = 0; let both = a > 0 && b == 0; let either = a == 0 || b == 0; printf("safe={} both={} either={} cmparg={}\n", safe, both, either, pick(a == 3)); return 0; }' 'safe=56 both=true either=true cmparg=1'
run_one breakcont 'fn main() -> i32 { let mut i = 0; let mut sum = 0; while true { i = i + 1; if i > 10 { break; } if i % 2 == 0 { continue; } sum = sum + i; } let mut n = 0; loop { n = n + 1; if n >= 5 { break; } } printf("sum={} i={} n={}\n", sum, i, n); return 0; }' 'sum=25 i=11 n=5'
run_one strret 'fn join3(a: string, b: string, c: string) -> string { return a + b + c; } fn tag(s: string) -> string { if len(s) > 0 { return "[" + s + "]"; } printf("empty\n"); return s; } fn main() -> i32 { let s = join3("x", "-", "y"); printf("[{}]\n", s); printf("[{}]\n", join3(s, "!", s)); let a = tag("q"); let b = tag(""); printf("{}{}\n", a, b); return 0; }' $'[x-y]\n[x-y!x-y]\nempty\n[q]'
run_one verbatim 'fn main() -> i32 { let v: string = """ab""cd"""; printf("[{}] {}\n", v, len(v)); let multi: string = """
line1
line2"""; printf("[{}]\n", multi); printf("len={}\n", len(multi)); let rawslash: string = """x\ty\nz"""; printf("[{}] {}\n", rawslash, len(rawslash)); return 0; }' $'[ab""cd] 6\n[\nline1\nline2]\nlen=12\n[x\\ty\\nz] 7'
run_one struct1 'struct Rect { w: i32, h: i32, } fn Rect.area(self: *Rect) -> i32 { return self.w * self.h; } fn Rect.grow(self: *Rect, by: i32) { self.w += by; self.h += by; } fn Rect.square(n: i32) -> *Rect { return new Rect { w: n, h: n }; } fn main() -> i32 { let r = Rect.square(3); r.grow(1); let a: i32 = r.area(); printf("area={} w={} h={}\n", a, r.w, r.h); return a - 10; }' $'area=16 w=4 h=4' 6
run_one struct2 'struct A { lit: i32, n: i64, } fn f(a: *A) -> i64 { if a.lit == -1 { return 100; } return a.lit as i64; } fn main() -> i32 { let a: *A = new A { lit: 0 - 1, n: 5 }; printf("{}\n", f(a)); let b: *A = new A { lit: 7, n: 6 }; printf("{} {} {}\n", f(b), b.n, b.lit); return 0; }' $'100\n7 6 7'
# the checker rejects unknown names with diagnostics and exit 1
"$RHO" build libs/compiler/main.rho -o /tmp/rhoc-bad.wasm \
  --set 'SRC=fn main() -> i32 { let x = mystery(1); return 0; }' >/dev/null 2>&1
berr=$(wasmtime /tmp/rhoc-bad.wasm 2>&1 >/dev/null | head -1)
brc=$?
if [ "$brc" -eq 0 ] && [ "$berr" = "check: unknown fn 'mystery'" ]; then
  echo "  badsrc: ok (diagnosed, refused)"
else
  echo "FAIL selfhost/badsrc: rc=$brc err=[$berr]"
  FAILED=1
fi
if [ "$FAILED" -eq 0 ]; then
  echo "selfhost: ok (boot → rho compiler → program → run)"
else
  exit 1
fi
