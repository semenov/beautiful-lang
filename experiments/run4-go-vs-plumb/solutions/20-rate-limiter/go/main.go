package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

// eps absorbs float rounding so that e.g. ten refills of 0.1 count as one token.
const eps = 1e-9

const maxBody = 1 << 20

type bucket struct {
	tokens float64
	last   time.Time // carries a monotonic reading
}

type limiter struct {
	mu       sync.Mutex
	capacity float64
	rate     float64
	buckets  map[string]*bucket
	allowed  int64
	denied   int64
}

func newLimiter(capacity int64, rate float64) *limiter {
	return &limiter{capacity: float64(capacity), rate: rate, buckets: map[string]*bucket{}}
}

// check takes a token if one whole token is available. It returns the whole
// tokens left, or, when denied, the seconds to wait (at least 1).
func (l *limiter) check(key string, now time.Time) (ok bool, remaining int64, retry int64) {
	l.mu.Lock()
	defer l.mu.Unlock()
	b := l.buckets[key]
	if b == nil {
		b = &bucket{tokens: l.capacity, last: now}
		l.buckets[key] = b
	} else {
		if dt := now.Sub(b.last).Seconds(); dt > 0 {
			b.tokens = math.Min(l.capacity, b.tokens+dt*l.rate)
			b.last = now
		}
	}
	if b.tokens >= 1-eps {
		b.tokens = math.Max(0, b.tokens-1)
		l.allowed++
		return true, int64(math.Floor(b.tokens + eps)), 0
	}
	l.denied++
	s := math.Ceil((1-b.tokens)/l.rate - eps)
	if s < 1 {
		s = 1
	}
	return false, 0, int64(s)
}

func (l *limiter) stats() (int64, int64, int) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.allowed, l.denied, len(l.buckets)
}

func writeJSON(w http.ResponseWriter, status int, body string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	io.WriteString(w, body+"\n")
}

func writeError(w http.ResponseWriter, status int, msg string) {
	b, _ := json.Marshal(map[string]string{"error": msg})
	writeJSON(w, status, string(b))
}

func (l *limiter) handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/check":
			if r.Method != http.MethodPost {
				w.Header().Set("Allow", "POST")
				writeError(w, 405, "method not allowed")
				return
			}
			l.handleCheck(w, r)
		case "/stats":
			if r.Method != http.MethodGet {
				w.Header().Set("Allow", "GET")
				writeError(w, 405, "method not allowed")
				return
			}
			a, d, k := l.stats()
			writeJSON(w, 200, fmt.Sprintf(`{"allowed": %d, "denied": %d, "keys": %d}`, a, d, k))
		default:
			writeError(w, 404, "not found")
		}
	})
}

func (l *limiter) handleCheck(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBody))
	if err != nil {
		writeError(w, 400, "could not read request body")
		return
	}
	body = bytes.TrimSpace(body)
	if len(body) == 0 || body[0] != '{' {
		writeError(w, 400, "body must be a JSON object")
		return
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(body, &obj); err != nil {
		writeError(w, 400, "body must be a JSON object")
		return
	}
	raw, present := obj["key"]
	if !present {
		writeError(w, 400, "key is required")
		return
	}
	var key string
	if err := json.Unmarshal(raw, &key); err != nil || len(raw) == 0 || raw[0] != '"' {
		writeError(w, 400, "key must be a string")
		return
	}
	if key == "" {
		writeError(w, 400, "key must not be empty")
		return
	}
	ok, rem, retry := l.check(key, time.Now())
	if ok {
		writeJSON(w, 200, fmt.Sprintf(`{"allowed": true, "remaining": %d}`, rem))
		return
	}
	w.Header().Set("Retry-After", strconv.FormatInt(retry, 10))
	writeJSON(w, 429, fmt.Sprintf(`{"allowed": false, "remaining": 0, "retry_after": %d}`, retry))
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "error: "+format+"\n", args...)
	os.Exit(2)
}

func config() (int64, float64) {
	capacity := int64(10)
	if s, set := os.LookupEnv("CAPACITY"); set {
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil || n < 1 {
			fail("CAPACITY must be a whole number >= 1, got %q", s)
		}
		capacity = n
	}
	rate := 1.0
	if s, set := os.LookupEnv("REFILL_PER_SEC"); set {
		f, err := strconv.ParseFloat(s, 64)
		if err != nil || math.IsNaN(f) || math.IsInf(f, 0) || f <= 0 {
			fail("REFILL_PER_SEC must be a number > 0, got %q", s)
		}
		rate = f
	}
	return capacity, rate
}

func main() {
	capacity, rate := config()
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	ln, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", port))
	if err != nil {
		fail("%v", err)
	}
	srv := &http.Server{Handler: newLimiter(capacity, rate).handler()}
	if err := srv.Serve(ln); err != nil {
		fail("%v", err)
	}
}
