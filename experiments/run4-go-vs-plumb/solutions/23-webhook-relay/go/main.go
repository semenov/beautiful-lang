package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type event struct {
	id       int
	source   string
	body     []byte
	status   string
	attempts int
}

type relay struct {
	target      string
	secret      []byte
	maxAttempts int
	backoff     time.Duration
	timeout     time.Duration
	client      *http.Client

	mu      sync.Mutex
	nextID  int
	events  map[int]*event
	queues  map[string][]*event
	running map[string]bool
}

func envInt(name string, def int) int {
	v := os.Getenv(name)
	if v == "" {
		return def
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < 1 {
		fmt.Fprintf(os.Stderr, "%s must be a whole number >= 1\n", name)
		os.Exit(2)
	}
	return n
}

func main() {
	target := os.Getenv("TARGET_URL")
	secret := os.Getenv("SECRET")
	if target == "" || secret == "" {
		fmt.Fprintln(os.Stderr, "TARGET_URL and SECRET are required")
		os.Exit(2)
	}
	r := &relay{
		target:      target,
		secret:      []byte(secret),
		maxAttempts: envInt("MAX_ATTEMPTS", 5),
		backoff:     time.Duration(envInt("BACKOFF_MS", 1000)) * time.Millisecond,
		timeout:     time.Duration(envInt("ATTEMPT_TIMEOUT_MS", 5000)) * time.Millisecond,
		nextID:      1,
		events:      map[int]*event{},
		queues:      map[string][]*event{},
		running:     map[string]bool{},
		client: &http.Client{
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
			Transport:     &http.Transport{MaxIdleConnsPerHost: 64},
		},
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := http.Serve(ln, r); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	b, _ := json.Marshal(v)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	w.Write(b)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func (r *relay) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	path := req.URL.Path
	switch {
	case path == "/events" && req.Method == http.MethodPost:
		r.accept(w, req)
	case strings.HasPrefix(path, "/events/") && req.Method == http.MethodGet:
		r.status(w, path[len("/events/"):])
	default:
		writeErr(w, 404, "not found")
	}
}

func (r *relay) accept(w http.ResponseWriter, req *http.Request) {
	body, err := io.ReadAll(req.Body)
	if err != nil {
		writeErr(w, 400, "cannot read body")
		return
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(body, &obj); err != nil || obj == nil {
		writeErr(w, 400, "body must be a JSON object")
		return
	}
	var source string
	if raw, ok := obj["source"]; !ok || json.Unmarshal(raw, &source) != nil || source == "" {
		writeErr(w, 400, "source must be a non-empty string")
		return
	}
	if _, ok := obj["payload"]; !ok {
		writeErr(w, 400, "payload is required")
		return
	}
	r.mu.Lock()
	ev := &event{id: r.nextID, source: source, body: body, status: "pending"}
	r.nextID++
	r.events[ev.id] = ev
	r.queues[source] = append(r.queues[source], ev)
	if !r.running[source] {
		r.running[source] = true
		go r.worker(source)
	}
	r.mu.Unlock()
	writeJSON(w, 202, map[string]int{"id": ev.id})
}

func (r *relay) status(w http.ResponseWriter, s string) {
	id, err := strconv.Atoi(s)
	if err != nil || s != strconv.Itoa(id) {
		writeErr(w, 404, "not found")
		return
	}
	r.mu.Lock()
	ev := r.events[id]
	var out map[string]any
	if ev != nil {
		out = map[string]any{"id": ev.id, "source": ev.source, "status": ev.status, "attempts": ev.attempts}
	}
	r.mu.Unlock()
	if out == nil {
		writeErr(w, 404, "not found")
		return
	}
	writeJSON(w, 200, out)
}

func (r *relay) worker(source string) {
	for {
		r.mu.Lock()
		q := r.queues[source]
		if len(q) == 0 {
			delete(r.queues, source)
			delete(r.running, source)
			r.mu.Unlock()
			return
		}
		ev := q[0]
		r.queues[source] = q[1:]
		r.mu.Unlock()
		r.deliver(ev)
	}
}

func (r *relay) deliver(ev *event) {
	mac := hmac.New(sha256.New, r.secret)
	mac.Write(ev.body)
	sig := "sha256=" + hex.EncodeToString(mac.Sum(nil))
	final := "failed"
	for k := 1; k <= r.maxAttempts; k++ {
		r.mu.Lock()
		ev.attempts = k
		r.mu.Unlock()
		if r.attempt(ev, sig) {
			final = "delivered"
			break
		}
		if k < r.maxAttempts {
			time.Sleep(r.backoff << uint(k-1))
		}
	}
	r.mu.Lock()
	ev.status = final
	r.mu.Unlock()
}

func (r *relay) attempt(ev *event, sig string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), r.timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, r.target, bytes.NewReader(ev.body))
	if err != nil {
		return false
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Event-Id", strconv.Itoa(ev.id))
	req.Header.Set("X-Signature", sig)
	resp, err := r.client.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	if _, err := io.Copy(io.Discard, resp.Body); err != nil {
		return false
	}
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}
