// SUPPLEMENTARY kernel (reduced scale, 1.5k pieces — not the 200k kernel):
// naive string append the naive C way — strcat into a buffer, rescanning
// the string on every append (O(n^2), the rho cat path's C analog).
// Checksum: final length + byte sum.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PIECES[4] = {"a", "bc", "def", "ghij"};

int main(void) {
  char *buf = malloc(262144);
  if (!buf)
    return 1;
  buf[0] = '\0';
  for (int i = 0; i < 1500; i++)
    strcat(buf, PIECES[i & 3]);
  size_t n = strlen(buf);
  unsigned long long sum = 0;
  for (size_t j = 0; j < n; j++)
    sum += (unsigned char)buf[j];
  printf("naive n=%zu bytes=%llu\n", n, sum);
  free(buf);
  return 0;
}
