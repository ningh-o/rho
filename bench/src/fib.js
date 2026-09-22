// bench kernel: naive recursive fib(32) — fixed input, deterministic.
// Stresses call overhead, argument passing, and integer arithmetic.
"use strict";
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
console.log(`fib(32)=${fib(32)}`);
