#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t X = 2463534242u;
static uint32_t rnd(void) { X ^= X << 13; X ^= X >> 17; X ^= X << 5; return X; }

typedef struct { int id; uint32_t key; } Item;

static int by_key(const void *a, const void *b) {
  uint32_t x = ((const Item *)a)->key, y = ((const Item *)b)->key;
  return (x > y) - (x < y);
}

int main(void) {
  const int n = 3000000;
  for (int r = 0; r < 5; r++) {
    Item *items = malloc(sizeof(Item) * n);
    for (int i = 0; i < n; i++) items[i] = (Item){ i, rnd() };
    qsort(items, n, sizeof(Item), by_key);
    printf("%u %u %u\n", items[0].key, items[n / 2].key, items[n - 1].key);
    free(items);
  }
  return 0;
}
