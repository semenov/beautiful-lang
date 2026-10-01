package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"math/big"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
)

type sample struct {
	ts    int64
	value float64
}

type store struct {
	mu       sync.Mutex
	window   int64
	hasNow   bool
	now      int64
	accepted int64
	dropped  int64
	byName   map[string]*series
}

type series struct {
	samples   []sample
	pruneSize int // length after the last prune; used to amortize pruning
}

func newStore(window int64) *store {
	return &store{window: window, byName: map[string]*series{}}
}

// prune removes samples that have left the window. Caller holds mu.
func (s *store) prune(sr *series) {
	cut := s.now - s.window
	kept := sr.samples[:0]
	for _, x := range sr.samples {
		if x.ts > cut {
			kept = append(kept, x)
		}
	}
	sr.samples = kept
	sr.pruneSize = len(kept)
}

// ingest processes already-validated samples in order.
func (s *store) ingest(in []parsed) (acc, drop int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, p := range in {
		if !s.hasNow || p.ts > s.now {
			s.now = p.ts
			s.hasNow = true
		}
		if p.ts <= s.now-s.window {
			drop++
			continue
		}
		acc++
		sr := s.byName[p.name]
		if sr == nil {
			sr = &series{}
			s.byName[p.name] = sr
		}
		sr.samples = append(sr.samples, sample{p.ts, p.value})
		if len(sr.samples) > 2*sr.pruneSize+32 {
			s.prune(sr)
		}
	}
	s.accepted += acc
	s.dropped += drop
	return
}

func (s *store) aggregate(name string) []byte {
	s.mu.Lock()
	var vals []float64
	if sr := s.byName[name]; sr != nil {
		s.prune(sr)
		vals = make([]float64, len(sr.samples))
		for i, x := range sr.samples {
			vals[i] = x.value
		}
	}
	s.mu.Unlock()

	nameJSON, _ := json.Marshal(name)
	var b bytes.Buffer
	b.WriteString(`{"name": `)
	b.Write(nameJSON)
	if len(vals) == 0 {
		b.WriteString(`, "count": 0, "sum": 0, "min": null, "max": null, "p50": null, "p99": null}`)
		return b.Bytes()
	}
	sum := 0.0
	for _, v := range vals {
		sum += v
	}
	sort.Float64s(vals)
	c := len(vals)
	rank := func(p int) float64 {
		r := (p*c + 99) / 100
		if r < 1 {
			r = 1
		}
		return vals[r-1]
	}
	fmt.Fprintf(&b, `, "count": %d, "sum": %s, "min": %s, "max": %s, "p50": %s, "p99": %s}`,
		c, num(sum), num(vals[0]), num(vals[c-1]), num(rank(50)), num(rank(99)))
	return b.Bytes()
}

func num(f float64) string {
	if math.IsInf(f, 0) || math.IsNaN(f) {
		return "null" // cannot be represented in JSON
	}
	if f == 0 {
		return "0"
	}
	if math.Abs(f) < 1e21 {
		return strconv.FormatFloat(f, 'f', -1, 64)
	}
	return strconv.FormatFloat(f, 'g', -1, 64)
}

type parsed struct {
	name  string
	value float64
	ts    int64
}

var errBad = errors.New("invalid request body")

func parseSample(raw json.RawMessage) (parsed, error) {
	var p parsed
	var obj map[string]json.RawMessage
	dec := json.NewDecoder(bytes.NewReader(raw))
	if err := dec.Decode(&obj); err != nil || obj == nil {
		return p, errors.New("sample must be a JSON object")
	}
	nr, ok := obj["name"]
	if !ok {
		return p, errors.New("name is missing")
	}
	if err := json.Unmarshal(nr, &p.name); err != nil || len(nr) == 0 || nr[0] != '"' {
		return p, errors.New("name must be a string")
	}
	if p.name == "" {
		return p, errors.New("name must not be empty")
	}
	vr, ok := obj["value"]
	if !ok {
		return p, errors.New("value is missing")
	}
	if len(vr) == 0 || !(vr[0] == '-' || (vr[0] >= '0' && vr[0] <= '9')) {
		return p, errors.New("value must be a number")
	}
	v, err := strconv.ParseFloat(string(vr), 64)
	if err != nil || math.IsInf(v, 0) || math.IsNaN(v) {
		return p, errors.New("value must be a finite number")
	}
	p.value = v
	tr, ok := obj["ts"]
	if !ok {
		return p, errors.New("ts is missing")
	}
	if len(tr) == 0 || !(tr[0] == '-' || (tr[0] >= '0' && tr[0] <= '9')) {
		return p, errors.New("ts must be an integer")
	}
	r, ok := new(big.Rat).SetString(string(tr))
	if !ok || !r.IsInt() {
		return p, errors.New("ts must be an integer")
	}
	if r.Sign() < 0 {
		return p, errors.New("ts must not be negative")
	}
	if !r.Num().IsInt64() {
		return p, errors.New("ts is too large")
	}
	p.ts = r.Num().Int64()
	return p, nil
}

func parseBody(body []byte) ([]parsed, error) {
	body = bytes.TrimSpace(body)
	if len(body) == 0 {
		return nil, errBad
	}
	var raws []json.RawMessage
	switch body[0] {
	case '[':
		if err := json.Unmarshal(body, &raws); err != nil {
			return nil, errBad
		}
	case '{':
		if !json.Valid(body) {
			return nil, errBad
		}
		raws = []json.RawMessage{body}
	default:
		return nil, errors.New("body must be a JSON object or array of objects")
	}
	out := make([]parsed, 0, len(raws))
	for _, r := range raws {
		p, err := parseSample(r)
		if err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, nil
}

func writeJSON(w http.ResponseWriter, code int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	w.Write(body)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	b, _ := json.Marshal(map[string]string{"error": msg})
	writeJSON(w, code, b)
}

func (s *store) handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/metrics":
			if r.Method != http.MethodPost {
				w.Header().Set("Allow", "POST")
				writeErr(w, 405, "method not allowed")
				return
			}
			body, err := io.ReadAll(r.Body)
			if err != nil {
				writeErr(w, 400, "cannot read body")
				return
			}
			in, err := parseBody(body)
			if err != nil {
				writeErr(w, 400, err.Error())
				return
			}
			a, d := s.ingest(in)
			writeJSON(w, 200, []byte(fmt.Sprintf(`{"accepted": %d, "dropped": %d}`, a, d)))
		case "/aggregate":
			if r.Method != http.MethodGet {
				w.Header().Set("Allow", "GET")
				writeErr(w, 405, "method not allowed")
				return
			}
			name := r.URL.Query().Get("name")
			if name == "" {
				writeErr(w, 400, "name is required")
				return
			}
			writeJSON(w, 200, s.aggregate(name))
		case "/stats":
			if r.Method != http.MethodGet {
				w.Header().Set("Allow", "GET")
				writeErr(w, 405, "method not allowed")
				return
			}
			s.mu.Lock()
			now := "null"
			if s.hasNow {
				now = strconv.FormatInt(s.now, 10)
			}
			out := fmt.Sprintf(`{"accepted": %d, "dropped": %d, "now": %s}`, s.accepted, s.dropped, now)
			s.mu.Unlock()
			writeJSON(w, 200, []byte(out))
		default:
			writeErr(w, 404, "not found")
		}
	})
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	window := int64(60000)
	if v := strings.TrimSpace(os.Getenv("WINDOW_MS")); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 1 {
			fmt.Fprintln(os.Stderr, "WINDOW_MS must be a whole number >= 1")
			os.Exit(1)
		}
		window = n
	}
	s := newStore(window)
	if err := http.ListenAndServe("127.0.0.1:"+port, s.handler()); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
