// file-store: an HTTP service that keeps uploaded files in a directory.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

type store struct {
	dir      string
	maxBytes int64
	// mu serialises the "is it new?" check + rename of a PUT and DELETE, so
	// the 201/200 answer is right. Bodies are written outside the lock.
	mu sync.Mutex
}

type fileInfo struct {
	Name   string `json:"name"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

func validName(n string) bool {
	if len(n) < 1 || len(n) > 100 || n[0] == '.' {
		return false
	}
	for i := 0; i < len(n); i++ {
		c := n[i]
		ok := c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
			c == '.' || c == '_' || c == '-'
		if !ok {
			return false
		}
	}
	return true
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

func (s *store) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	// Work on the escaped path so %2F and friends stay distinguishable.
	p := r.URL.EscapedPath()
	if p == "/files" {
		if r.Method != http.MethodGet {
			writeErr(w, 405, "method not allowed")
			return
		}
		s.list(w)
		return
	}
	if !strings.HasPrefix(p, "/files/") {
		writeErr(w, 404, "not found")
		return
	}
	switch r.Method {
	case http.MethodGet, http.MethodPut, http.MethodDelete:
	default:
		writeErr(w, 405, "method not allowed")
		return
	}
	name, err := url.PathUnescape(p[len("/files/"):])
	if err != nil || !validName(name) {
		writeErr(w, 400, "invalid name")
		return
	}
	switch r.Method {
	case http.MethodGet:
		s.get(w, name)
	case http.MethodPut:
		s.put(w, r, name)
	case http.MethodDelete:
		s.del(w, name)
	}
}

func (s *store) path(name string) string { return filepath.Join(s.dir, name) }

func (s *store) put(w http.ResponseWriter, r *http.Request, name string) {
	if r.ContentLength > s.maxBytes {
		writeErr(w, 413, "too large")
		return
	}
	tmp, err := os.CreateTemp(s.dir, ".upload-*")
	if err != nil {
		writeErr(w, 500, "internal error")
		return
	}
	tmpName := tmp.Name()
	done := false
	defer func() {
		if !done {
			tmp.Close()
			os.Remove(tmpName)
		}
	}()

	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r.Body, s.maxBytes+1))
	if err != nil {
		writeErr(w, 400, "could not read body")
		return
	}
	if n > s.maxBytes {
		writeErr(w, 413, "too large")
		return
	}
	if err := tmp.Close(); err != nil {
		writeErr(w, 500, "internal error")
		return
	}
	if err := os.Chmod(tmpName, 0o644); err != nil {
		writeErr(w, 500, "internal error")
		return
	}

	s.mu.Lock()
	_, statErr := os.Lstat(s.path(name))
	existed := statErr == nil
	err = os.Rename(tmpName, s.path(name))
	s.mu.Unlock()
	if err != nil {
		writeErr(w, 500, "internal error")
		return
	}
	done = true

	code := 201
	if existed {
		code = 200
	}
	writeJSON(w, code, fileInfo{name, n, hex.EncodeToString(h.Sum(nil))})
}

func (s *store) get(w http.ResponseWriter, name string) {
	f, err := os.Open(s.path(name))
	if err != nil {
		writeErr(w, 404, "not found")
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || !st.Mode().IsRegular() {
		writeErr(w, 404, "not found")
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(st.Size(), 10))
	w.WriteHeader(200)
	io.Copy(w, f)
}

func (s *store) del(w http.ResponseWriter, name string) {
	s.mu.Lock()
	st, err := os.Lstat(s.path(name))
	if err == nil && st.Mode().IsRegular() {
		err = os.Remove(s.path(name))
	} else if err == nil {
		err = os.ErrNotExist
	}
	s.mu.Unlock()
	if errors.Is(err, os.ErrNotExist) {
		writeErr(w, 404, "not found")
		return
	}
	if err != nil {
		writeErr(w, 500, "internal error")
		return
	}
	w.WriteHeader(204)
}

func (s *store) list(w http.ResponseWriter) {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		writeErr(w, 500, "internal error")
		return
	}
	files := []fileInfo{}
	for _, e := range entries {
		name := e.Name()
		if !validName(name) {
			continue
		}
		f, err := os.Open(s.path(name))
		if err != nil {
			continue // removed meanwhile
		}
		st, err := f.Stat()
		if err != nil || !st.Mode().IsRegular() {
			f.Close()
			continue
		}
		h := sha256.New()
		n, err := io.Copy(h, f)
		f.Close()
		if err != nil {
			continue
		}
		files = append(files, fileInfo{name, n, hex.EncodeToString(h.Sum(nil))})
	}
	sort.Slice(files, func(i, j int) bool { return files[i].Name < files[j].Name })
	writeJSON(w, 200, map[string]any{"files": files})
}

func main() {
	port := os.Getenv("PORT")
	dir := os.Getenv("STORE_DIR")
	maxStr := os.Getenv("MAX_BYTES")
	max, err := strconv.ParseInt(maxStr, 10, 64)
	if port == "" || dir == "" || err != nil || max < 0 {
		fmt.Fprintln(os.Stderr, "usage: PORT=8080 STORE_DIR=/data MAX_BYTES=1048576 app")
		os.Exit(2)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		log.Fatal(err)
	}
	// Clean up temp files left by a crash.
	if ents, err := os.ReadDir(dir); err == nil {
		for _, e := range ents {
			if strings.HasPrefix(e.Name(), ".upload-") {
				os.Remove(filepath.Join(dir, e.Name()))
			}
		}
	}
	s := &store{dir: dir, maxBytes: max}
	log.Fatal(http.ListenAndServe("127.0.0.1:"+port, s))
}
