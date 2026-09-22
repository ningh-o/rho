// SUPPLEMENTARY kernel: the task's original 192x192 scale. Measured first; the
// headline matmul kernel escalated to 256 because C compute here (~4ms) sat
// below wasmtime's own process overhead. Same op order and checksum scheme.
// Entries are small deterministic integers stored as f64. The checksum is
// the exact IEEE-754 bit pattern of the row-major sum of C plus its
// truncation, so any reassociation of the float ops shows up as a mismatch.
"use strict";
const N = 192;
const A = new Float64Array(N * N);
const B = new Float64Array(N * N);
const C = new Float64Array(N * N);
for (let i = 0; i < N; i++)
  for (let j = 0; j < N; j++) {
    A[i * N + j] = ((i * 7 + j * 13) % 19) - 9;
    B[i * N + j] = ((i * 5 + j * 3) % 17) - 8;
  }
for (let i = 0; i < N; i++)
  for (let j = 0; j < N; j++) {
    let s = 0;
    for (let k = 0; k < N; k++) s += A[i * N + k] * B[k * N + j];
    C[i * N + j] = s;
  }
let total = 0;
for (let i = 0; i < N * N; i++) total += C[i];
const bits = new BigUint64Array(new Float64Array([total]).buffer)[0];
console.log(`matmul(${N}) sum_bits=${bits} sum_int=${Math.trunc(total)}`);
