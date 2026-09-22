// bench kernel: tight integer loop, ~1e8 iterations of
// acc = (acc + (i % 100000)^2) % 4294967291 over int64 — every step is
// exact in int64 AND in a JS double (t^2 <= 1e10, acc + t^2 < 2^53), so
// the three implementations must agree digit for digit. Stresses int64
// arithmetic, modulo, and loop-carried dependencies.
#include <stdio.h>

int main(void) {
  long long acc = 0;
  for (long long i = 0; i < 100000000LL; i++) {
    long long t = i % 100000;
    acc = (acc + t * t) % 4294967291LL;
  }
  printf("intloop acc=%lld\n", acc);
  return 0;
}
