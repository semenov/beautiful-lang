package main

import (
	"fmt"
	"sort"
	"strings"
)

var x uint32 = 2463534242

func rnd() uint32 { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x }

func main() {
	const vocabSize, n = 50_000, 5_000_000
	const letters = "abcdefghijklmnopqrstuvwxyz"
	vocab := make([]string, vocabSize)
	for k := range vocab {
		v, b := k, []byte{}
		for {
			b = append(b, letters[v%26])
			v /= 26
			if v == 0 {
				break
			}
		}
		vocab[k] = string(b)
	}

	for round := 0; round < 5; round++ {
		countRound(vocab)
	}
}

func countRound(vocab []string) {
	const n = 5_000_000
	var sb strings.Builder
	for i := 0; i < n; i++ {
		if i > 0 {
			sb.WriteByte(' ')
		}
		sb.WriteString(vocab[rnd()%uint32(len(vocab))])
	}
	text := sb.String()

	counts := map[string]int{}
	for _, w := range strings.Split(text, " ") {
		counts[w]++
	}
	type entry struct {
		w string
		c int
	}
	all := make([]entry, 0, len(counts))
	for w, c := range counts {
		all = append(all, entry{w, c})
	}
	sort.Slice(all, func(i, j int) bool {
		if all[i].c != all[j].c {
			return all[i].c > all[j].c
		}
		return all[i].w < all[j].w
	})
	fmt.Println(len(text))
	for _, e := range all[:5] {
		fmt.Println(e.w, e.c)
	}
}
