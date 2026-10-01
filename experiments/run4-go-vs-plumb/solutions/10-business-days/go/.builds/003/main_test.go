package main

import (
	"math/rand"
	"testing"
	"time"
)

func TestAgainstNaive(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	base := time.Date(1, 1, 1, 0, 0, 0, 0, time.UTC)
	for it := 0; it < 300; it++ {
		var weekend [7]bool
		for i := range weekend {
			weekend[i] = r.Intn(3) == 0
		}
		weekend[r.Intn(7)] = false
		var hol []int
		hs := map[int]bool{}
		for i := 0; i < 20; i++ {
			d := 700000 + r.Intn(200)
			hol = append(hol, d)
			if !weekend[d%7] {
				hs[d] = true
			}
		}
		cal := newCalendar(weekend, hol)
		biz := func(d int) bool { return !weekend[d%7] && !hs[d] }
		for k := 0; k < 30; k++ {
			a, b := 700000+r.Intn(250), 700000+r.Intn(250)
			want := 0
			for d := a; d < b; d++ {
				if biz(d) {
					want++
				}
			}
			for d := b; d < a; d++ {
				if biz(d) {
					want--
				}
			}
			if got := cal.before(b) - cal.before(a); got != want {
				t.Fatalf("between %d %d: %d != %d", a, b, got, want)
			}
			n := r.Intn(41) - 20
			cur, steps := a, n
			if n == 0 {
				for !biz(cur) {
					cur++
				}
			}
			for ; steps > 0; steps-- {
				cur++
				for !biz(cur) {
					cur++
				}
			}
			for ; steps < 0; steps++ {
				cur--
				for !biz(cur) {
					cur--
				}
			}
			got, ok := cal.add(a, int64(n))
			if !ok || got != cur {
				t.Fatalf("add %d %d: %d != %d", a, n, got, cur)
			}
		}
	}
	for d := 0; d <= maxDay; d += 997 {
		want := base.AddDate(0, 0, d).Format("2006-01-02")
		if formatDate(d) != want {
			t.Fatalf("format %d: %s != %s", d, formatDate(d), want)
		}
		if p, ok := parseDate(want); !ok || p != d {
			t.Fatalf("parse %s", want)
		}
	}
}
