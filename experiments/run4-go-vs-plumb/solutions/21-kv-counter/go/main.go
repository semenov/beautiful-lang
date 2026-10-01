// kv-counter: an in-memory HTTP key-value store with atomic counters and
// compare-and-set. See SPEC.md.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"unicode/utf8"
)

// value is a stored value: either a string or a 64-bit integer.
type value struct {
	isInt bool
	i     int64
	s     string
}

// MarshalJSON lets values be embedded directly in response bodies.
func (v value) MarshalJSON() ([]byte, error) {
	if v.isInt {
		return strconv.AppendInt(nil, v.i, 10), nil
	}
	return marshal(v.s)
}

func marshal(x any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(x); err != nil {
		return nil, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nil
}

// parseValue interprets raw JSON as a valid value (string or in-range integer).
func parseValue(raw json.RawMessage) (value, bool) {
	raw = bytes.TrimSpace(raw)
	if len(raw) == 0 {
		return value{}, false
	}
	switch {
	case raw[0] == '"':
		var s string
		if err := json.Unmarshal(raw, &s); err != nil {
			return value{}, false
		}
		return value{s: s}, true
	case raw[0] == '-' || (raw[0] >= '0' && raw[0] <= '9'):
		// Only plain integer literals: no fraction, no exponent.
		for _, c := range raw[1:] {
			if c < '0' || c > '9' {
				return value{}, false
			}
		}
		n, err := strconv.ParseInt(string(raw), 10, 64)
		if err != nil {
			return value{}, false
		}
		return value{isInt: true, i: n}, true
	}
	return value{}, false
}

// parseObject decodes a body that must be exactly one JSON object.
func parseObject(r *http.Request) (map[string]json.RawMessage, bool) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, false
	}
	dec := json.NewDecoder(bytes.NewReader(body))
	var obj map[string]json.RawMessage
	if err := dec.Decode(&obj); err != nil || obj == nil {
		return nil, false
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		return nil, false // trailing data
	}
	return obj, true
}

type store struct {
	mu sync.Mutex
	m  map[string]value
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	s := &store{m: map[string]value{}}
	log.Fatal(http.ListenAndServe("127.0.0.1:"+port, s))
}

func writeJSON(w http.ResponseWriter, status int, x any) {
	b, err := marshal(x)
	if err != nil {
		b, status = []byte(`{"error": "internal error"}`), 500
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(append(b, '\n'))
}

func errBody(msg string) map[string]any { return map[string]any{"error": msg} }

func errValue(msg string, cur *value) map[string]any {
	m := errBody(msg)
	if cur == nil {
		m["value"] = nil
	} else {
		m["value"] = *cur
	}
	return m
}

func (s *store) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path // already percent-decoded
	if path == "/snapshot" {
		if r.Method != http.MethodGet {
			writeJSON(w, 405, errBody("method not allowed"))
			return
		}
		s.snapshot(w)
		return
	}

	var kind, key string
	for _, p := range []string{"/kv/", "/incr/", "/cas/"} {
		if strings.HasPrefix(path, p) {
			kind, key = p, path[len(p):]
			break
		}
	}
	if kind == "" {
		writeJSON(w, 404, errBody("not found"))
		return
	}
	var allowed string
	switch kind {
	case "/kv/":
		switch r.Method {
		case http.MethodGet, http.MethodPut, http.MethodDelete:
			allowed = r.Method
		}
	default:
		if r.Method == http.MethodPost {
			allowed = r.Method
		}
	}
	if allowed == "" {
		writeJSON(w, 405, errBody("method not allowed"))
		return
	}
	if key == "" || !utf8.ValidString(key) {
		writeJSON(w, 400, errBody("invalid key"))
		return
	}

	switch {
	case kind == "/kv/" && allowed == http.MethodGet:
		s.get(w, key)
	case kind == "/kv/" && allowed == http.MethodPut:
		s.put(w, r, key)
	case kind == "/kv/":
		s.del(w, key)
	case kind == "/incr/":
		s.incr(w, r, key)
	default:
		s.cas(w, r, key)
	}
}

func (s *store) snapshot(w http.ResponseWriter) {
	s.mu.Lock()
	items := make(map[string]value, len(s.m))
	for k, v := range s.m {
		items[k] = v
	}
	s.mu.Unlock()
	writeJSON(w, 200, map[string]any{"count": len(items), "items": items})
}

func (s *store) get(w http.ResponseWriter, key string) {
	s.mu.Lock()
	v, ok := s.m[key]
	s.mu.Unlock()
	if !ok {
		writeJSON(w, 404, errBody("not found"))
		return
	}
	writeJSON(w, 200, map[string]any{"key": key, "value": v})
}

func (s *store) put(w http.ResponseWriter, r *http.Request, key string) {
	obj, ok := parseObject(r)
	if !ok {
		writeJSON(w, 400, errBody("body must be a JSON object"))
		return
	}
	raw, present := obj["value"]
	v, valid := parseValue(raw)
	if !present || !valid {
		writeJSON(w, 400, errBody("value must be a string or a 64-bit integer"))
		return
	}
	s.mu.Lock()
	s.m[key] = v
	s.mu.Unlock()
	writeJSON(w, 200, map[string]any{"key": key, "value": v})
}

func (s *store) del(w http.ResponseWriter, key string) {
	s.mu.Lock()
	_, ok := s.m[key]
	delete(s.m, key)
	s.mu.Unlock()
	if !ok {
		writeJSON(w, 404, errBody("not found"))
		return
	}
	w.WriteHeader(204)
}

func (s *store) incr(w http.ResponseWriter, r *http.Request, key string) {
	by := int64(1)
	if q := r.URL.Query(); q.Has("by") {
		str := q.Get("by")
		digits := strings.TrimPrefix(str, "-")
		valid := digits != ""
		for _, c := range digits {
			if c < '0' || c > '9' {
				valid = false
			}
		}
		n, err := strconv.ParseInt(str, 10, 64)
		if !valid || err != nil {
			writeJSON(w, 400, errBody("by must be a 64-bit integer"))
			return
		}
		by = n
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	cur, exists := s.m[key]
	if !exists {
		cur = value{isInt: true}
	}
	if !cur.isInt {
		writeJSON(w, 409, errValue("value is not an integer", &cur))
		return
	}
	sum := cur.i + by
	if (by > 0 && sum < cur.i) || (by < 0 && sum > cur.i) {
		writeJSON(w, 409, errValue("result out of 64-bit range", &cur))
		return
	}
	nv := value{isInt: true, i: sum}
	s.m[key] = nv
	writeJSON(w, 200, map[string]any{"key": key, "value": nv})
}

func (s *store) cas(w http.ResponseWriter, r *http.Request, key string) {
	obj, ok := parseObject(r)
	if !ok {
		writeJSON(w, 400, errBody("body must be a JSON object"))
		return
	}
	rawE, hasE := obj["expected"]
	rawV, hasV := obj["value"]
	if !hasE || !hasV {
		writeJSON(w, 400, errBody("expected and value are required"))
		return
	}
	expectNone := string(bytes.TrimSpace(rawE)) == "null"
	var exp value
	if !expectNone {
		var valid bool
		if exp, valid = parseValue(rawE); !valid {
			writeJSON(w, 400, errBody("expected must be null, a string or a 64-bit integer"))
			return
		}
	}
	nv, valid := parseValue(rawV)
	if !valid {
		writeJSON(w, 400, errBody("value must be a string or a 64-bit integer"))
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	cur, exists := s.m[key]
	match := (expectNone && !exists) || (!expectNone && exists && cur == exp)
	if !match {
		var p *value
		if exists {
			p = &cur
		}
		writeJSON(w, 409, errValue("current value does not match expected", p))
		return
	}
	s.m[key] = nv
	writeJSON(w, 200, map[string]any{"key": key, "value": nv})
}
