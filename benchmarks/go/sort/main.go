package main

import (
	"cmp"
	"fmt"
	"slices"
)

var x uint32 = 2463534242

func rnd() uint32 { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x }

type Item struct {
	id  int
	key uint32
}

func main() {
	const n = 3_000_000
	for r := 0; r < 5; r++ {
		items := make([]Item, n)
		for i := range items {
			items[i] = Item{i, rnd()}
		}
		slices.SortFunc(items, func(a, b Item) int { return cmp.Compare(a.key, b.key) })
		fmt.Println(items[0].key, items[n/2].key, items[n-1].key)
	}
}
