package main

import "fmt"

type P struct{ x, y int64 }

func main() {
	const n, rounds = 10_000_000, 10
	var sum int64
	for r := int64(0); r < rounds; r++ {
		var pts []P
		for i := int64(0); i < n; i++ {
			pts = append(pts, P{i, i*2 + r})
		}
		for _, p := range pts {
			sum += p.x + p.y
		}
	}
	fmt.Println(sum)
}
