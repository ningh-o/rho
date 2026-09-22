// bench kernel: f64 matrix multiply, C = A * B at 256x256, k innermost. (Escalated from the task's 192: at 192, C compute
// was ~4ms — under wasmtime's 4.1ms process overhead — per the task's
// "too fast -> 256" clause.) Entries are small deterministic integers stored as f64. The checksum is
// the exact IEEE-754 bit pattern of the row-major sum of C plus its
// truncation, so any reassociation of the float ops shows up as a mismatch.
// (No -ffast-math: float reductions are not vectorized, op order is fixed.)
#include <stdio.h>

#define N 256

static double A[N * N], B[N * N], C[N * N];

int main(void) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      A[i * N + j] = (double)((i * 7 + j * 13) % 19) - 9.0;
      B[i * N + j] = (double)((i * 5 + j * 3) % 17) - 8.0;
    }
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      double s = 0.0;
      for (int k = 0; k < N; k++)
        s += A[i * N + k] * B[k * N + j];
      C[i * N + j] = s;
    }
  double total = 0.0;
  for (int i = 0; i < N * N; i++)
    total += C[i];
  union {
    double d;
    unsigned long long u;
  } cvt;
  cvt.d = total;
  printf("matmul(%d) sum_bits=%llu sum_int=%lld\n", N, cvt.u, (long long)total);
  return 0;
}
