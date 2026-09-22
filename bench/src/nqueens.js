// bench kernel: count all solutions to the 12-queens problem via the
// column/left-diagonal/right-diagonal bitmask recursion — fixed input,
// deterministic answer (14200). Stresses recursion, int32 bit ops
// (JS bitwise operators already coerce to int32).
"use strict";
const FULL = 4095; // (1 << 12) - 1
function count(mask, dl, dr) {
  if (mask === FULL) return 1;
  let total = 0;
  let avail = FULL & ~(mask | dl | dr);
  while (avail !== 0) {
    const bit = avail & -avail;
    avail &= avail - 1;
    total += count(mask | bit, (dl | bit) << 1, (dr | bit) >> 1);
  }
  return total;
}
console.log(`queens(12)=${count(0, 0, 0)}`);
