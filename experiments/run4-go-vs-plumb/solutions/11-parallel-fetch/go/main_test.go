package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func run(t *testing.T, urls []string, conc int, timeout time.Duration) (string, int) {
	t.Helper()
	opts := options{concurrency: conc, timeout: timeout}
	res := fetchAll(urls, opts)
	var sb strings.Builder
	code := report(&sb, urls, res)
	return sb.String(), code
}

func TestBasics(t *testing.T) {
	var cur, peak int32
	mux := http.NewServeMux()
	mux.HandleFunc("/a", func(w http.ResponseWriter, r *http.Request) {
		n := atomic.AddInt32(&cur, 1)
		for {
			p := atomic.LoadInt32(&peak)
			if n <= p || atomic.CompareAndSwapInt32(&peak, p, n) {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
		atomic.AddInt32(&cur, -1)
		fmt.Fprint(w, "hello")
	})
	mux.HandleFunc("/r", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/a", 302) })
	mux.HandleFunc("/loop", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/loop", 302) })
	mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) {
		for i := 0; i < 50; i++ {
			fmt.Fprint(w, "x")
			w.(http.Flusher).Flush()
			time.Sleep(50 * time.Millisecond)
		}
	})
	mux.HandleFunc("/404", func(w http.ResponseWriter, r *http.Request) { http.Error(w, "nope", 404) })
	s := httptest.NewServer(mux)
	defer s.Close()
	u := s.URL
	urls := []string{u + "/a", u + "/a", u + "/a", u + "/a", u + "/r", u + "/loop", u + "/slow", u + "/404", "ftp://x", "http://", "http://127.0.0.1:1/"}
	out, code := run(t, urls, 2, 500*time.Millisecond)
	want := []string{
		u + "/a 200 5", u + "/a 200 5", u + "/a 200 5", u + "/a 200 5", u + "/r 200 5",
		u + "/loop error redirects", u + "/slow error timeout", u + "/404 404 5",
		"ftp://x error invalid-url", "http:// error invalid-url", "http://127.0.0.1:1/ error connect",
		"total 11 ok 5 bad-status 1 failed 5",
	}
	if out != strings.Join(want, "\n")+"\n" || code != 1 {
		t.Fatalf("got:\n%s\ncode %d", out, code)
	}
	if peak > 2 {
		t.Fatalf("peak concurrency %d", peak)
	}
}

func TestArgs(t *testing.T) {
	for _, bad := range [][]string{{"--concurrency", "0"}, {"--concurrency"}, {"--timeout", "0"}, {"--x"}, {"a", "b"}, {"--timeout", "abc"}} {
		if _, err := parseArgs(bad); err == nil {
			t.Errorf("%v accepted", bad)
		}
	}
	o, err := parseArgs([]string{"--timeout=0.5", "--concurrency", "3", "f"})
	if err != nil || o.timeout != 500*time.Millisecond || o.concurrency != 3 || o.file != "f" {
		t.Errorf("%+v %v", o, err)
	}
}
