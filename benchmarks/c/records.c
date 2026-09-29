#include <stdio.h>
#include <stdlib.h>

typedef struct { long long x, y; } P;

int main(void) {
  long long n = 10000000, sum = 0;
  for (int r = 0; r < 10; r++) {
    long long len = 0, cap = 0;
    P *pts = NULL;
    for (long long i = 0; i < n; i++) {
      if (len == cap) { cap = cap ? cap * 2 : 4; pts = realloc(pts, cap * sizeof(P)); }
      pts[len++] = (P){ i, i * 2 + r };
    }
    for (long long i = 0; i < len; i++) sum += pts[i].x + pts[i].y;
    free(pts);
  }
  printf("%lld\n", sum);
  return 0;
}
