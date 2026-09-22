// bench kernel: naive recursive fib(32) — fixed input, deterministic.
// Stresses call overhead, argument passing, and integer arithmetic.
#include <stdio.h>

static long long fib(long long n) {
  if (n < 2)
    return n;
  return fib(n - 1) + fib(n - 2);
}

int main(void) {
  printf("fib(32)=%lld\n", fib(32));
  return 0;
}
