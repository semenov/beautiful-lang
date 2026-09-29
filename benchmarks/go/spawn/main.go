package main

import (
	"fmt"
	"sync"
)

func work(i int) int {
	s := 0
	for k := 0; k < 100; k++ {
		s += (i * k) % 7
	}
	return s
}

func main() {
	total := 0
	for round := 0; round < 200; round++ {
		res := make([]int, 1000)
		var wg sync.WaitGroup
		for i := 0; i < 1000; i++ {
			wg.Add(1)
			go func(i int) { defer wg.Done(); res[i] = work(i + round) }(i)
		}
		wg.Wait()
		for _, v := range res {
			total += v
		}
	}
	fmt.Println(total)
}
