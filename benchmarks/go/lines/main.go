package main

import (
	"bufio"
	"fmt"
	"os"
)

func main() {
	path := os.TempDir() + "/bench-lines.txt"
	total := 0
	for round := 0; round < 3; round++ {
		f, _ := os.Create(path)
		w := bufio.NewWriter(f)
		for i := 0; i < 2_000_000; i++ {
			fmt.Fprintf(w, "line %d of the file, with some text after it\n", i)
		}
		w.Flush()
		f.Close()
		r, _ := os.Open(path)
		s := bufio.NewScanner(r)
		n := 0
		for s.Scan() {
			n += len(s.Text())
		}
		r.Close()
		total += n
	}
	os.Remove(path)
	fmt.Println(total)
}
