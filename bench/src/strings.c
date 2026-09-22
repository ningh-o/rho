// bench kernel: build one string by appending 200k small pieces (cycled
// "a", "bc", "def", "ghij" -> 500000 bytes). C's idiomatic amortized
// append: one malloc (the final size is known here) + memcpy per piece.
// Checksum: final length + byte sum.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PIECES[4] = {"a", "bc", "def", "ghij"};

int main(void) {
  char *buf = malloc(500000);
  if (!buf)
    return 1;
  size_t n = 0;
  for (size_t i = 0; i < 200000; i++) {
    const char *p = PIECES[i & 3];
    size_t l = strlen(p);
    memcpy(buf + n, p, l);
    n += l;
  }
  unsigned long long sum = 0;
  for (size_t j = 0; j < n; j++)
    sum += (unsigned char)buf[j];
  printf("strings n=%zu bytes=%llu\n", n, sum);
  free(buf);
  return 0;
}
