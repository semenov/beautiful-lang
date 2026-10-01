package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type job struct {
	id     int
	ms     int
	input  string
	status string
	result *string
	cancel chan struct{} // closed when a running job is cancelled
	atStop bool          // was running when shutdown began
}

type queue struct {
	mu        sync.Mutex
	workers   int
	jobs      []*job // index = id-1
	pending   []*job // queued jobs in id order
	running   int
	stopping  bool
	completed int
	idle      chan struct{} // closed when stopping and nothing is running
}

func (q *queue) submit(ms int, input string) int {
	q.mu.Lock()
	defer q.mu.Unlock()
	j := &job{id: len(q.jobs) + 1, ms: ms, input: input, status: "queued"}
	q.jobs = append(q.jobs, j)
	q.pending = append(q.pending, j)
	q.dispatch()
	return j.id
}

// dispatch starts queued jobs while workers are free. Caller holds q.mu.
func (q *queue) dispatch() {
	for !q.stopping && q.running < q.workers && len(q.pending) > 0 {
		j := q.pending[0]
		q.pending = q.pending[1:]
		if j.status != "queued" {
			continue
		}
		j.status = "running"
		j.cancel = make(chan struct{})
		q.running++
		go q.run(j)
	}
	q.checkIdle()
}

func (q *queue) checkIdle() {
	if q.stopping && q.running == 0 {
		select {
		case <-q.idle:
		default:
			close(q.idle)
		}
	}
}

func (q *queue) run(j *job) {
	t := time.NewTimer(time.Duration(j.ms) * time.Millisecond)
	defer t.Stop()
	select {
	case <-j.cancel:
		return
	case <-t.C:
	}
	r := []rune(j.input)
	for a, b := 0, len(r)-1; a < b; a, b = a+1, b-1 {
		r[a], r[b] = r[b], r[a]
	}
	s := string(r)
	q.mu.Lock()
	defer q.mu.Unlock()
	if j.status != "running" {
		return
	}
	j.status = "done"
	j.result = &s
	q.running--
	if j.atStop {
		q.completed++
	}
	q.dispatch()
}

func (q *queue) get(id int) *job {
	if id < 1 || id > len(q.jobs) {
		return nil
	}
	return q.jobs[id-1]
}

func view(j *job) map[string]any {
	var res any
	if j.result != nil {
		res = *j.result
	}
	return map[string]any{"id": j.id, "status": j.status, "result": res}
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	enc.Encode(v)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	w.Write(b.Bytes())
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

var intRe = regexp.MustCompile(`^-?(0|[1-9][0-9]*)$`)
var idRe = regexp.MustCompile(`^[1-9][0-9]*$`)

func parseJob(body []byte) (int, string, string) {
	var m map[string]json.RawMessage
	if err := json.Unmarshal(body, &m); err != nil || m == nil {
		return 0, "", "body must be a JSON object"
	}
	rawMs, ok := m["ms"]
	if !ok {
		return 0, "", "ms is missing"
	}
	if !intRe.Match(rawMs) {
		return 0, "", "ms must be an integer"
	}
	ms, err := strconv.Atoi(string(rawMs))
	if err != nil || ms < 0 || ms > 60000 {
		return 0, "", "ms must be between 0 and 60000"
	}
	rawIn, ok := m["input"]
	if !ok {
		return 0, "", "input is missing"
	}
	var input string
	if len(rawIn) == 0 || rawIn[0] != '"' || json.Unmarshal(rawIn, &input) != nil {
		return 0, "", "input must be a string"
	}
	return ms, input, ""
}

func (q *queue) handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := r.URL.Path
		switch {
		case path == "/jobs":
			if r.Method != http.MethodPost {
				writeErr(w, 405, "method not allowed")
				return
			}
			buf := new(bytes.Buffer)
			if _, err := buf.ReadFrom(r.Body); err != nil {
				writeErr(w, 400, "cannot read body")
				return
			}
			ms, input, msg := parseJob(buf.Bytes())
			if msg != "" {
				writeErr(w, 400, msg)
				return
			}
			writeJSON(w, 202, map[string]int{"id": q.submit(ms, input)})
		case path == "/stats":
			if r.Method != http.MethodGet {
				writeErr(w, 405, "method not allowed")
				return
			}
			q.mu.Lock()
			c := map[string]int{"queued": 0, "running": 0, "done": 0, "cancelled": 0}
			for _, j := range q.jobs {
				c[j.status]++
			}
			q.mu.Unlock()
			writeJSON(w, 200, c)
		case strings.HasPrefix(path, "/jobs/"):
			rest := path[len("/jobs/"):]
			idStr, action, hasAction := strings.Cut(rest, "/")
			if !idRe.MatchString(idStr) || (hasAction && action != "cancel") {
				writeErr(w, 404, "not found")
				return
			}
			id, err := strconv.Atoi(idStr)
			if err != nil {
				writeErr(w, 404, "not found")
				return
			}
			want := http.MethodGet
			if hasAction {
				want = http.MethodPost
			}
			if r.Method != want {
				writeErr(w, 405, "method not allowed")
				return
			}
			q.mu.Lock()
			defer q.mu.Unlock()
			j := q.get(id)
			if j == nil {
				writeErr(w, 404, "no such job")
				return
			}
			if !hasAction {
				writeJSON(w, 200, view(j))
				return
			}
			switch j.status {
			case "queued":
				j.status = "cancelled"
			case "running":
				j.status = "cancelled"
				close(j.cancel)
				q.running--
				q.dispatch()
			default:
				writeErr(w, 409, "job is "+j.status)
				return
			}
			writeJSON(w, 200, view(j))
		default:
			writeErr(w, 404, "not found")
		}
	})
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func main() {
	port := envInt("PORT", 8080)
	workers := envInt("WORKERS", 2)
	if workers < 1 {
		workers = 1
	}
	grace := envInt("SHUTDOWN_GRACE_MS", 5000)

	q := &queue{workers: workers, idle: make(chan struct{})}
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	srv := &http.Server{Handler: q.handler()}
	go srv.Serve(ln)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	<-sig
	ln.Close()

	q.mu.Lock()
	q.stopping = true
	for _, j := range q.jobs {
		if j.status == "running" {
			j.atStop = true
		}
	}
	q.checkIdle()
	q.mu.Unlock()

	code := 0
	select {
	case <-q.idle:
	case <-time.After(time.Duration(grace) * time.Millisecond):
		code = 1
	}
	q.mu.Lock()
	abandoned := 0
	for _, j := range q.jobs {
		if j.status == "queued" || (j.status == "running" && j.atStop) {
			abandoned++
		}
	}
	completed := q.completed
	q.mu.Unlock()
	fmt.Printf("shutdown: completed %d, abandoned %d\n", completed, abandoned)
	os.Exit(code)
}
