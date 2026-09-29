package main

import "fmt"

var x uint32 = 2463534242

func rnd() uint32 { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x }

func main() {
	total := 0
	for round := 0; round < 5; round++ {
		m := map[int]int{}
		for i := 0; i < 2_000_000; i++ {
			m[int(rnd()%1_000_000)] += i % 7
		}
		hits := 0
		for i := 0; i < 5_000_000; i++ {
			if v, ok := m[int(rnd()%2_000_000)]; ok {
				hits += v
			}
		}
		for k := 0; k < 1_000_000; k += 2 {
			delete(m, k)
		}
		total += hits + len(m)
	}
	fmt.Println(total)
}
