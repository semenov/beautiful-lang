package main

import "fmt"

func main() {
	total := 0
	for round := 0; round < 5; round++ {
		ch := make(chan int, 100)
		done := make(chan int)
		go func() {
			s := 0
			for v := range ch {
				s += v
			}
			done <- s
		}()
		for i := 0; i < 1_000_000; i++ {
			ch <- i
		}
		close(ch)
		total += <-done
	}
	fmt.Println(total)
}
