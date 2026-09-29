package main

import "fmt"

type Node struct{ l, r *Node }

func build(d int) *Node {
	if d == 0 {
		return &Node{}
	}
	return &Node{build(d - 1), build(d - 1)}
}

func check(t *Node) int {
	if t.l == nil {
		return 1
	}
	return 1 + check(t.l) + check(t.r)
}

func main() {
	const max = 20
	fmt.Printf("stretch %d\n", check(build(max+1)))
	long := build(max)
	for d := 4; d <= max; d += 2 {
		iters := 1 << (max - d + 4)
		total := 0
		for i := 0; i < iters; i++ {
			total += check(build(d))
		}
		fmt.Printf("%d trees of depth %d: %d\n", iters, d, total)
	}
	fmt.Printf("long lived: %d\n", check(long))
}
