// Shared fixtures for the rho-lsp tests.

/** A valid program (rho corpus 001, the hello world). */
export const GOOD_RHO = `fn main() -> i32 {
  printf("hello, world\\n");
  return 0;
}
`;

/** The type-mismatch diagnostic fixture (tests/diag/d001_type_mismatch.rho):
 *  the compiler reports line 2 col 18 (1-based). */
export const BAD_RHO = `fn main() -> i32 {
    let x: i32 = "hello";
    return x;
}
`;

/** Deliberately messy formatting input. */
export const MESSY_RHO = 'fn   main( ) -> i32 {\n\tlet x=1;\nreturn x;}\n';

/** `rho fmt` canonical output of MESSY_RHO (measured from the compiler). */
export const CANONICAL_RHO = 'fn main() -> i32 {\n  let x = 1;\n  return x;\n}\n';

/** A syntactically broken document (parse errors, not type errors). */
export const BROKEN_RHO = 'fn main( -> i32 { return 0; }\n';

/** A richer document exercising the symbol index: methods, structs, enums,
 *  params, locals. Type-checked against the real compiler (exit 0). */
export const SYMBOLS_RHO = `struct Point {
  x: i32,
  y: i32,
}

enum Shape {
  Circle(f64),
  Unit,
}

fn area(s: Shape) -> f64 {
  match s {
    Circle(r) => r * r,
    Unit => 0.0,
  }
}

fn Point.dist(self: *Point) -> f64 {
  return 0.0;
}

fn norm(p: *Point) -> i32 {
  return p.x + p.y;
}

fn main() -> i32 {
  let p: *Point = new Point { x: 1, y: 2 };
  let n: i32 = 3;
  return norm(p) + p.x + n;
}
`;

/** Wait for a condition with polling; `fn` may be async; throws after timeoutMs. */
export async function waitFor(fn, timeoutMs = 15000, stepMs = 25) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > deadline) throw new Error('waitFor: timed out');
    await new Promise((r) => setTimeout(r, stepMs));
  }
}
