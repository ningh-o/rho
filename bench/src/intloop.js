// bench kernel: tight integer loop, ~1e8 iterations of
// acc = (acc + (i % 100000)^2) % 4294967291 — every step is exact in a JS
// double (t^2 <= 1e10, acc + t^2 < 2^53) and in an int64, so the three
// implementations must agree digit for digit. Stresses arithmetic and
// loop-carried dependencies.
"use strict";
let acc = 0;
for (let i = 0; i < 100000000; i++) {
  const t = i % 100000;
  acc = (acc + t * t) % 4294967291;
}
console.log(`intloop acc=${acc}`);
