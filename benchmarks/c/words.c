#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOCAB 50000
#define N 5000000
#define TABLE 131072

static uint32_t X = 2463534242u;
static uint32_t rnd(void) { X ^= X << 13; X ^= X >> 17; X ^= X << 5; return X; }

typedef struct { const char *s; int len; int count; } Entry;
static Entry table[TABLE];
static char vocab[VOCAB][8];
static int vocab_len[VOCAB];

static int by_count(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  if (x->count != y->count) return y->count - x->count;
  int m = x->len < y->len ? x->len : y->len;
  int c = memcmp(x->s, y->s, m);
  return c ? c : x->len - y->len;
}

int main(void) {
  const char *letters = "abcdefghijklmnopqrstuvwxyz";
  for (int k = 0; k < VOCAB; k++) {
    int v = k, n = 0;
    do { vocab[k][n++] = letters[v % 26]; v /= 26; } while (v > 0);
    vocab_len[k] = n;
  }

  for (int round = 0; round < 5; round++) {
  memset(table, 0, sizeof table);
  size_t cap = 1 << 20, len = 0;
  char *text = malloc(cap);
  for (int i = 0; i < N; i++) {
    uint32_t k = rnd() % VOCAB;
    int wl = vocab_len[k];
    if (len + wl + 1 > cap) { cap *= 2; text = realloc(text, cap); }
    if (i > 0) text[len++] = ' ';
    memcpy(text + len, vocab[k], wl);
    len += wl;
  }

  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i < len && text[i] != ' ') continue;
    const char *w = text + start;
    int wl = (int)(i - start);
    uint32_t h = 2166136261u;
    for (int j = 0; j < wl; j++) { h ^= (unsigned char)w[j]; h *= 16777619u; }
    size_t slot = h & (TABLE - 1);
    while (table[slot].s && !(table[slot].len == wl && memcmp(table[slot].s, w, wl) == 0))
      slot = (slot + 1) & (TABLE - 1);
    if (!table[slot].s) { table[slot].s = w; table[slot].len = wl; }
    table[slot].count++;
    start = i + 1;
  }

  Entry *all = malloc(sizeof(Entry) * VOCAB);
  int n = 0;
  for (int i = 0; i < TABLE; i++) if (table[i].s) all[n++] = table[i];
  qsort(all, n, sizeof(Entry), by_count);
  printf("%zu\n", len);
  for (int i = 0; i < 5 && i < n; i++) printf("%.*s %d\n", all[i].len, all[i].s, all[i].count);
  free(all);
  free(text);
  }
  return 0;
}
