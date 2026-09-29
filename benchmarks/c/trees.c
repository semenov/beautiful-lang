#include <stdio.h>
#include <stdlib.h>

typedef struct Node { struct Node *l, *r; } Node;

static Node *make(int d) {
  Node *n = malloc(sizeof *n);
  if (d == 0) { n->l = n->r = NULL; } else { n->l = make(d - 1); n->r = make(d - 1); }
  return n;
}
static long check(Node *t) { return t->l ? 1 + check(t->l) + check(t->r) : 1; }
static void drop(Node *t) { if (t->l) { drop(t->l); drop(t->r); } free(t); }

int main(void) {
  const int MAX = 20;
  Node *stretch = make(MAX + 1);
  printf("stretch %ld\n", check(stretch));
  drop(stretch);
  Node *long_lived = make(MAX);
  for (int d = 4; d <= MAX; d += 2) {
    int iters = 1 << (MAX - d + 4);
    long total = 0;
    for (int i = 0; i < iters; i++) { Node *t = make(d); total += check(t); drop(t); }
    printf("%d trees of depth %d: %ld\n", iters, d, total);
  }
  printf("long lived: %ld\n", check(long_lived));
  drop(long_lived);
  return 0;
}
