// SUPPLEMENTARY kernel (reduced scale, 1.5k pieces — not the 200k kernel):
// naive string append with `+=` (V8 cons strings — amortized near-linear,
// not quadratic). Included so the three languages' naive append paths can
// be compared against rho's cat. Checksum: final length + char-code sum.
"use strict";
const PIECES = ["a", "bc", "def", "ghij"];
let s = "";
for (let i = 0; i < 1500; i++) s += PIECES[i & 3];
let sum = 0;
for (let j = 0; j < s.length; j++) sum += s.charCodeAt(j);
console.log(`naive n=${s.length} bytes=${sum}`);
