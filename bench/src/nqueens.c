// bench kernel: count all solutions to the 12-queens problem via the
// column/left-diagonal/right-diagonal bitmask recursion — fixed input,
// deterministic answer (14200). Stresses recursion, i32 bit ops.
#include <stdio.h>

static int count(int mask, int dl, int dr, int full) {
  if (mask == full)
    return 1;
  int total = 0;
  int avail = full & ~(mask | dl | dr);
  while (avail != 0) {
    int bit = avail & -avail;
    avail &= avail - 1;
    total += count(mask | bit, (dl | bit) << 1, (dr | bit) >> 1, full);
  }
  return total;
}

int main(void) {
  printf("queens(12)=%d\n", count(0, 0, 0, (1 << 12) - 1));
  return 0;
}
