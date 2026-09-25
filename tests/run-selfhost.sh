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
run_one bools 'fn main() -> i32 { let flag = true; let off = false; if flag { printf("on\n"); } if !off { printf("not off\n"); } if !flag { printf("no\n"); } else { printf("yes\n"); } printf("f={} nf={}\n", off, !flag); return 0; }' $'on\nnot off\nyes\nf=false nf=false'
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
run_one struct3 'struct Node { v: i32, } struct Bundle { xs: []i32, name: string, node: *Node, } fn main() -> i32 { let mut xs: []i32 = make([]i32, 3); xs[0] = 5; xs[1] = 6; xs[2] = 7; let b: *Bundle = new Bundle { xs: xs, name: "hold", node: new Node { v: 9 } }; let st: Bundle = (*b); st.xs[1] = 60; st.node.v = 90; printf("shared: {} {}\n", b.xs[1], b.node.v); printf("identity: {} {}\n", st.node == b.node, st.name == b.name); let xs2: []i32 = b.xs; xs2[2] = 70; printf("field copies: {} {}\n", b.xs[2], len2(xs2)); return 0; } fn len2(xs: []i32) -> i64 { return xs[2]; }' $'shared: 60 90\nidentity: true true\nfield copies: 70 70'
run_one enum1 'enum Shape { Circle, Square, Triangle, } fn code(s: Shape) -> i32 { return match s { Shape.Circle => 1, Shape.Square => 2, Shape.Triangle => 3, }; } fn kind(n: i32) -> string { return match n { 0 => "zero", 7 => "lucky", _ => "many", }; } fn main() -> i32 { let t: Shape = Shape.Triangle; printf("{} {} {} as={} eq={} ne={}\n", code(Shape.Circle), code(Shape.Square), code(t), Shape.Triangle as i32, Shape.Circle == Shape.Circle, Shape.Circle != Shape.Square); printf("{} {} {}\n", kind(0), kind(7), kind(9)); return 0; }' $'1 2 3 as=2 eq=true ne=true\nzero lucky many'
run_one enum2 'enum Op { Lit(i32), Sum(i32, i32), } fn eval(o: Op) -> i32 { return match o { Op.Lit(v) => { let doubled: i32 = (v * 2); (doubled + 1) }, Op.Sum(a, b) => { let partial: i32 = match a { 0 => b, _ => (a + b), }; ((partial * 10) + a) }, }; } fn main() -> i32 { printf("{} {} {}\n", eval(Op.Lit(20)), eval(Op.Sum(0, 7)), eval(Op.Sum(3, 4))); return 0; }' '41 70 73'
run_one opt1 'fn safe_div(a: i32, b: i32) -> Result[i32, string] { if b == 0 { return Result.Err("division by zero"); } return Result.Ok(a / b); } fn compute() -> Result[i32, string] { let q: i32 = safe_div(12, 2)?; let r: i32 = safe_div(q, 3)?; return Result.Ok(q + r); } fn main() -> i32 { let v: Result[i32, string] = compute(); let a: i32 = match v { Result.Ok(x) => x, Result.Err(_) => 0 - 1, }; printf("a={}\n", a); match compute() { Result.Ok(n) => printf("ok={}\n", n), Result.Err(_) => printf("err\n"), }; return a; }' $'a=8\nok=8' 8
run_one float1 'fn main() -> i32 { let a: f64 = 2.5; let b: f64 = 4.0; let mut r: i32 = 0; if a * b == 10.0 { r += 1; } if b / a == 1.6 { r += 2; } let c: f32 = 1.5; if c as f64 + a == 4.0 { r += 4; } if a < b { r += 8; } if a > b { r += 100; } let big: f64 = 1e100; let neg: f64 = 0.0 - 300.5; let mut s2: i32 = big as i8; s2 += neg as i32; s2 += 180; let half: f64 = 5; return r + s2 + (half as i32) / 2; }' '' 24
run_one ifexpr 'enum Countdown { Done, Step(i32), } fn burn(c: Countdown) -> i64 { return match c { Countdown.Done => 0, Countdown.Step(n) => { let stop: bool = (n <= 0); if stop { 0 } else { (n as i64 + burn(Countdown.Step((n - 1)))) } }, }; } fn main() -> i32 { let a: i32 = if 3 > 2 { 10 } else { 20 }; let b: i32 = if 1 > 2 { 5 } else { if 2 > 1 { 6 } else { 7 } }; let w: i64 = burn(Countdown.Step(4)); printf("a={} b={} burn={} t={} f={}\n", a, b, w, true, 1 > 2); return a + b; }' $'a=10 b=6 burn=10 t=true f=false' 16
run_one arrlit 'fn total(xs: []i64) -> i64 { let mut t: i64 = 0; let mut i: usize = 0; while i < len(xs) { t += xs[i]; i += 1; } return t; } fn main() -> i32 { let s: []i64 = [10, 20, 30]; printf("{} {} {}\n", s[0], s[2], total(s)); let w: string = if 1 > 0 { "yes" } else { "no" }; printf("[{}]\n", w); return 0; }' $'10 30 60\n[yes]'
run_one boolconst 'const ON: bool = true; static mut FLAG: bool = false; fn flip() -> i64 { FLAG = true; return 0; } fn main() -> i32 { flip(); printf("on={} flag={} not={}\n", ON, FLAG, !ON); return 0; }' 'on=true flag=true not=false'
run_one strconst 'const NAME: string = "rho"; const TAG: string = "[" + NAME + "]"; fn greet() -> i64 { printf("hi {} in {}\n", NAME, TAG); return 0; } fn main() -> i32 { greet(); let s = format("[{}]!", NAME); printf("{}\n", s); return 0; }' $'hi rho in [rho]\n[rho]!'
run_one f64print 'const RATE: f64 = 0.5; fn main() -> i32 { let a: f64 = 2.5; printf("{} {} {}\n", a, 4.0, 0.1 + 0.2); printf("{} {}\n", 1.0 / 3.0, 123.456); printf("{} {} {}\n", 1e-5, 1.7976931348623157e+308, 0.0 - 2.5000000000000001e-09); let c: f32 = 2.25; printf("{}\n", c); let s = format("[{}]", a); printf("{}\n", s); printf("r={}\n", RATE); return 0; }' $'2.5 4.0 0.30000000000000004\n0.33333333333333331 123.456\n1.0000000000000001e-05 1.7976931348623157e+308 -2.5000000000000001e-09\n2.25\n[2.5]\nr=0.5'
run_one f64conv 'fn main() -> i32 { let big: u64 = 18446744073709551615; printf("{}\n", big as f64); let neg: i64 = 0 - 7; printf("{} {}\n", neg as f64, (neg as f64) as i32); let mut pre: string = "0."; let mut j: i64 = 0; while j > 0 { pre = pre + "0"; j = j - 1; } printf("[{}]\n", pre + "5"); return 0; }' $'1.8446744073709552e+19\n-7.0 -7\n[0.5]'
run_one enumstr 'fn parse(n: i32) -> Result[i32, string] { if n < 0 { return Result.Err("negative"); } return Result.Ok(n * 2); } enum Tag { Plain, Bad(string), } fn label(t: Tag) -> string { return match t { Tag.Plain => "ok", Tag.Bad(why) => why, }; } fn main() -> i32 { match parse(21) { Result.Ok(v) => printf("ok={}\n", v), Result.Err(e) => printf("err={}\n", e), }; match parse(0 - 5) { Result.Ok(v) => printf("ok={}\n", v), Result.Err(e) => printf("err={}\n", e), }; printf("{} {}\n", label(Tag.Plain), label(Tag.Bad("why not"))); return 0; }' $'ok=42\nerr=negative\nok why not'
run_one opt2 'struct Node { v: i64, } enum Item { Held(*Node), Count(i64), Nothing, } fn mk() -> Item { return Item.Held(new Node { v: 11 }); } fn bump(n: *Node) -> i64 { n.v = 110; return n.v; } fn main() -> i32 { let it: Item = mk(); let it2: Item = it; let s: i64 = match it2 { Item.Held(n) => bump(n), Item.Count(c) => c, Item.Nothing => 0, }; let t: i64 = match it { Item.Held(n) => n.v, _ => 0, }; printf("s={} t={} eq={}\n", s, t, it == it2); let mut xs: []?*Node = make([]?*Node, 2); match xs[0] { Option.Some(h) => printf("bad {}\n", h.v), Option.None => printf("none\n"), }; xs[1] = Option.Some(new Node { v: 5 }); match xs[1] { Option.Some(h) => printf("five={}\n", h.v), Option.None => printf("none\n"), }; return 0; }' $'s=110 t=110 eq=true\nnone\nfive=5'
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
