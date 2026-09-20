// Every example that appears in the tutorial and the playground picker.
// Each entry's code is compiled and executed by tools/verify_examples.mjs,
// and `expect` (when present) is matched against the program's stdout.

export const EXAMPLES = [
  {
    id: "tour",
    title: "A taste of rho",
    code: `// Algebraic data, exhaustively matched.
enum Shape {
  Circle(f64),
  Rect { w: f64, h: f64 },
}

fn area(s: Shape) -> f64 {
  return match s {
    Shape.Circle(r) => 3.14159 * r * r,
    Shape.Rect { w, h } => w * h,
  };
}

// Closures capture by copy.
fn make_adder(n: i32) -> fn(i32) -> i32 {
  return fn(x: i32) -> i32 { return x + n; };
}

// Heap objects are reference-counted: freed the
// moment the last reference dies. No GC, no pauses.
struct Node {
  value: i32,
  next: *Node,
}

fn main() -> i32 {
  printf("{}\\n", area(Shape.Rect(w: 3.0, h: 4.0)));
  printf("{}\\n", area(Shape.Circle(2.0)));

  let add10: fn(i32) -> i32 = make_adder(10);
  printf("{}\\n", add10(5));

  let a: *Node = new Node { value: 1, next: null };
  let b: *Node = a; // one copy, one retain
  printf("{}\\n", b.value);
  return 0;
}
`,
    expect: `12.0\n12.56636\n15\n1\n`,
  },
  {
    id: "hello",
    title: "Hello, rho",
    code: `// Every rho program starts at main.
fn main() -> i32 {
  printf("hello, world\\n");
  return 0;
}
`,
  },
  {
    id: "values",
    title: "Numbers that stay defined",
    code: `fn main() -> i32 {
  let big: i32 = 2_147_483_647;
  printf("{}\\n", big + 1); // wraps, never UB

  let third: f64 = 1.0 / 3.0;
  printf("{}\\n", third); // 17 digits: enough to read the exact value back

  let hex: i32 = 0xFF;      // 255
  let bits: i32 = 0b1010;   // 10
  printf("{}\\n", hex + bits);
  return 0;
}
`,
    expect: `-2147483648\n0.33333333333333331\n265\n`,
  },
  {
    id: "bindings",
    title: "let and mut",
    code: `fn main() -> i32 {
  let x: i32 = 10;      // immutable by default
  let mut y: i32 = 0;   // mut to allow assignment
  y += x;
  // x = 5;             // rejected by the compiler
  printf("{}\\n", y);
  return 0;
}
`,
    expect: `10\n`,
  },
  {
    id: "control",
    title: "Control flow",
    code: `fn classify(n: i32) -> string {
  // if is an expression: both arms produce a value
  return if n < 10 { "small" } else { "big" };
}

fn main() -> i32 {
  printf("{}\\n", classify(5));

  let mut i: i32 = 0;
  let mut sum: i32 = 0;
  while i < 100 {
    sum += i;
    i += 1;
  }
  printf("{}\\n", sum);

  loop {
    sum += 1;
    if sum > 5000 {
      break;
    }
  }
  printf("{}\\n", sum);
  return 0;
}
`,
    expect: `small\n4950\n5001\n`,
  },
  {
    id: "functions",
    title: "Functions and recursion",
    code: `fn fib(n: i32) -> i32 {
  if n < 2 {
    return n;
  }
  return fib(n - 1) + fib(n - 2);
}

fn power(base: i32, exp: i32) -> i32 {
  let mut result: i32 = 1;
  let mut e: i32 = exp;
  while e > 0 {
    result *= base;
    e -= 1;
  }
  return result;
}

fn main() -> i32 {
  printf("{}\\n", fib(20));
  printf("{}\\n", power(2, 16));
  return 0;
}
`,
    expect: `6765\n65536\n`,
  },
  {
    id: "structs",
    title: "Structs on the heap",
    code: `struct Point {
  x: f64,
  y: f64,
}

// the first parameter named self makes it a method
fn Point.dist2(self: *Point, other: *Point) -> f64 {
  let dx: f64 = self.x - other.x;
  let dy: f64 = self.y - other.y;
  return dx * dx + dy * dy;
}

fn main() -> i32 {
  let a: *Point = new Point { x: 0.0, y: 0.0 };
  let b: *Point = new Point { x: 3.0, y: 4.0 };
  printf("{}\\n", a.dist2(b)); // 25: the 3-4-5 triangle
  return 0;
}
`,
    expect: `25.0\n`,
  },
  {
    id: "enums",
    title: "Enums and match",
    code: `enum Shape {
  Circle(f64),
  Rect { w: f64, h: f64 },
  Point,
}

fn area(s: Shape) -> f64 {
  // match is exhaustive: every variant is covered
  return match s {
    Shape.Circle(r) => 3.14159 * r * r,
    Shape.Rect { w, h } => w * h,
    Shape.Point => 0.0,
  };
}

fn main() -> i32 {
  let r: Shape = Shape.Rect(w: 3.0, h: 4.0);
  let c: Shape = Shape.Circle(2.0);
  printf("{}\\n", area(r));
  printf("{}\\n", area(c));
  printf("{}\\n", area(Shape.Point));
  return 0;
}
`,
    expect: `12.0\n12.56636\n0.0\n`,
  },
  {
    id: "slices",
    title: "Slices, safely",
    code: `fn sum(xs: []i32) -> i32 {
  let mut total: i32 = 0;
  let mut i: usize = 0;
  while i < len(xs) {
    total += xs[i];
    i += 1;
  }
  return total;
}

fn main() -> i32 {
  let xs: []i32 = make([]i32, 4);
  xs[0] = 10;
  xs[1] = 20;
  xs[2] = 30;
  xs[3] = 40;
  printf("{}\\n", sum(xs));
  let view: []i32 = xs[1..3]; // shares the buffer
  printf("{}\\n", sum(view));
  // xs[9] would panic: index out of bounds, never silent corruption
  return 0;
}
`,
    expect: `100\n50\n`,
  },
  {
    id: "errors",
    title: "Result, Option and ?",
    code: `fn safe_div(a: i32, b: i32) -> Result[i32, string] {
  if b == 0 {
    return Result.Err("division by zero");
  }
  return Result.Ok(a / b);
}

fn compute() -> Result[i32, string] {
  let q: i32 = safe_div(84, 2)?; // on Err, return early
  let r: i32 = safe_div(q, 3)?;
  return Result.Ok(q + r);
}

fn main() -> i32 {
  let v: Result[i32, string] = compute();
  let n: i32 = match v {
    Result.Ok(x) => x,
    Result.Err(_) => 0,
  };
  printf("{}\\n", n);
  printf("{}\\n", match safe_div(1, 0) {
    Result.Ok(_) => "ok",
    Result.Err(e) => e,
  });
  return 0;
}
`,
    expect: `56\ndivision by zero\n`,
  },
  {
    id: "closures",
    title: "Closures from day one",
    code: `fn make_adder(n: i32) -> fn(i32) -> i32 {
  // n is captured by copy
  return fn(x: i32) -> i32 { return x + n; };
}

fn main() -> i32 {
  let add10: fn(i32) -> i32 = make_adder(10);
  let add90: fn(i32) -> i32 = make_adder(90);
  printf("{}\\n", add10(5));
  printf("{}\\n", add90(5));
  printf("{}\\n", add10(1) + add90(1));
  return 0;
}
`,
    expect: `15\n95\n102\n`,
  },
  {
    id: "generics",
    title: "Generics, monomorphized",
    code: `fn id[T](x: T) -> T {
  return x;
}

struct Pair[A, B] {
  a: A,
  b: B,
}

fn swap[A, B](p: *Pair[A, B]) -> *Pair[B, A] {
  return new Pair[B, A] { a: p.b, b: p.a };
}

fn main() -> i32 {
  let a: i32 = id(7);
  let b: i64 = id(11); // a fresh specialization
  let p: *Pair[i32, i64] = new Pair[i32, i64] { a: 3, b: 4 };
  let q: *Pair[i64, i32] = swap(p);
  printf("{}\\n", a);
  printf("{}\\n", b);
  printf("{}\\n", q.a); // i64 payload survives the swap
  return 0;
}
`,
    expect: `7\n11\n4\n`,
  },
  {
    id: "defer",
    title: "defer runs on every exit",
    code: `fn work() -> i32 {
  defer printf("{}", "cleanup\\n");
  printf("{}", "working\\n");
  if true {
    return 7; // defer fires here too
  }
  return 0;
}

fn main() -> i32 {
  let r: i32 = work();
  printf("{}\\n", r);
  return 0;
}
`,
    expect: `working\ncleanup\n7\n`,
  },
  {
    id: "memory",
    title: "Ownership you can hold",
    code: `struct Node {
  value: i32,
  next: *Node,
}

fn main() -> i32 {
  // new returns a heap object with a reference count
  let a: *Node = new Node { value: 1, next: null };
  let b: *Node = a; // copies the pointer, bumps the count

  printf("{}\\n", b.value);

  // when the last reference dies the object is freed —
  // no garbage collector, no pauses, no use-after-free
  return 0;
}
`,
    expect: `1\n`,
  },
];

// Playground starter (what first-time visitors see).
export const STARTER = `// rho — a small, hand-forged systems language.
// Edit this program, press Run (or Ctrl/Cmd+Enter).

fn fib(n: i32) -> i32 {
  if n < 2 {
    return n;
  }
  return fib(n - 1) + fib(n - 2);
}

fn main() -> i32 {
  let mut i: i32 = 0;
  while i <= 10 {
    printf("fib({}) = {}\\n", i, fib(i));
    i += 1;
  }
  return 0;
}
`;
