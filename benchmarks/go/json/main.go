package main

import (
	"encoding/json"
	"fmt"
	"strconv"
)

var x uint32 = 2463534242

func rnd() uint32 { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x }

type User struct {
	ID     int      `json:"id"`
	Name   string   `json:"name"`
	Score  int      `json:"score"`
	Active bool     `json:"active"`
	Tags   []string `json:"tags"`
}

func main() {
	const n, rounds = 300_000, 10
	var users []User
	for i := 0; i < n; i++ {
		users = append(users, User{i, "user" + strconv.Itoa(i), int(rnd() % 1000), i%3 == 0, []string{"alpha", "beta"}})
	}
	total, length := 0, 0
	for r := 0; r < rounds; r++ {
		data, err := json.Marshal(users)
		if err != nil {
			panic(err)
		}
		length = len(data)
		var back []User
		if err := json.Unmarshal(data, &back); err != nil {
			panic(err)
		}
		for _, u := range back {
			if u.Active {
				total += u.Score
			}
		}
	}
	fmt.Println(length, total)
}
