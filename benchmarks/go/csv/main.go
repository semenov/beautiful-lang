package main

import (
	"fmt"
	"strconv"
	"strings"
)

var x uint32 = 2463534242

func rnd() uint32 { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x }

func main() {
	total, outLen := 0, 0
	for round := 0; round < 3; round++ {
		var sb strings.Builder
		for i := 0; i < 1_000_000; i++ {
			fmt.Fprintf(&sb, "%d,user%d,%d.%02d\n", i, rnd()%10000, rnd()%1000, rnd()%100)
		}
		text := sb.String()
		var out []string
		for _, line := range strings.Split(strings.TrimSpace(text), "\n") {
			f := strings.Split(line, ",")
			id, _ := strconv.Atoi(f[0])
			parts := strings.Split(f[2], ".")
			cents, _ := strconv.Atoi(parts[0])
			frac, _ := strconv.Atoi(parts[1])
			amount := cents*100 + frac
			total += amount + id%3
			out = append(out, strings.ToUpper(f[1])+"="+strconv.Itoa(amount))
		}
		outLen += len(strings.Join(out, ";"))
	}
	fmt.Println(total, outLen)
}
