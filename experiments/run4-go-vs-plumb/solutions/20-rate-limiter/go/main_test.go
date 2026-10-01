package main

import (
	"sync"
	"testing"
	"time"
)

func TestRefillAndRetry(t *testing.T) {
	l := newLimiter(2, 0.5)
	t0 := time.Now()
	for i, want := range []int64{1, 0} {
		ok, rem, _ := l.check("k", t0)
		if !ok || rem != want {
			t.Fatalf("check %d: %v %d", i, ok, rem)
		}
	}
	ok, _, retry := l.check("k", t0)
	if ok || retry != 2 {
		t.Fatalf("want denied retry 2, got %v %d", ok, retry)
	}
	if ok, _, retry = l.check("k", t0.Add(1900*time.Millisecond)); ok || retry != 1 {
		t.Fatalf("got %v %d", ok, retry)
	}
	if ok, rem, _ := l.check("k", t0.Add(2*time.Second)); !ok || rem != 0 {
		t.Fatalf("got %v %d", ok, rem)
	}
}

func TestFractionalAccumulates(t *testing.T) {
	l := newLimiter(1, 0.1)
	t0 := time.Now()
	l.check("k", t0)
	for i := 1; i <= 10; i++ {
		l.check("k", t0.Add(time.Duration(i)*time.Second-time.Millisecond))
	}
	if ok, _, _ := l.check("k", t0.Add(10*time.Second)); !ok {
		t.Fatal("expected allowed after 10s")
	}
}

func TestConcurrent(t *testing.T) {
	l := newLimiter(5, 0.0001)
	now := time.Now()
	var wg sync.WaitGroup
	var mu sync.Mutex
	n := 0
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if ok, _, _ := l.check("k", now); ok {
				mu.Lock()
				n++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if n != 5 {
		t.Fatalf("allowed %d", n)
	}
}
