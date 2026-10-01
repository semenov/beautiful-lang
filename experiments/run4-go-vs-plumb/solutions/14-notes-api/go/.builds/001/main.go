// notes-api: an HTTP JSON API for short notes, stored in SQLite.
package main

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"unicode/utf8"

	_ "github.com/mattn/go-sqlite3"
)

type Note struct {
	ID    int64   `json:"id"`
	Title string  `json:"title"`
	Body  string  `json:"body"`
	Tag   *string `json:"tag"`
}

type server struct {
	db *sql.DB
}

func main() {
	port := os.Getenv("PORT")
	dbPath := os.Getenv("DB_PATH")
	if port == "" || dbPath == "" {
		log.Fatal("PORT and DB_PATH must be set")
	}
	db, err := openDB(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer db.Close()

	s := &server{db: db}
	log.Fatal(http.ListenAndServe("127.0.0.1:"+port, s))
}

func openDB(path string) (*sql.DB, error) {
	db, err := sql.Open("sqlite3", "file:"+path+"?_busy_timeout=10000&_journal_mode=WAL&_synchronous=NORMAL")
	if err != nil {
		return nil, err
	}
	// A single connection serializes all access; the load is small and this
	// avoids "database is locked" under concurrent requests.
	db.SetMaxOpenConns(1)
	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS notes (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		title TEXT NOT NULL,
		body TEXT NOT NULL,
		tag TEXT
	)`)
	if err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		status = http.StatusInternalServerError
		buf.Reset()
		buf.WriteString(`{"error": "internal error"}`)
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func (s *server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch {
	case path == "/notes":
		switch r.Method {
		case http.MethodPost:
			s.create(w, r)
		case http.MethodGet:
			s.list(w, r)
		default:
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
	case strings.HasPrefix(path, "/notes/"):
		id, ok := parseID(path[len("/notes/"):])
		if !ok {
			writeError(w, http.StatusNotFound, "not found")
			return
		}
		switch r.Method {
		case http.MethodGet:
			s.get(w, id)
		case http.MethodPatch:
			s.patch(w, r, id)
		case http.MethodDelete:
			s.delete(w, id)
		default:
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
	default:
		writeError(w, http.StatusNotFound, "not found")
	}
}

// parseID accepts only a positive decimal integer made of ASCII digits.
func parseID(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
	}
	id, err := strconv.ParseInt(s, 10, 64)
	if err != nil || id <= 0 {
		return 0, false
	}
	return id, true
}

// readObject parses the request body as a JSON object.
func readObject(r *http.Request) (map[string]json.RawMessage, bool) {
	data, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, false
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	var obj map[string]json.RawMessage
	if err := dec.Decode(&obj); err != nil || obj == nil {
		return nil, false
	}
	if _, err := dec.Token(); err != io.EOF { // trailing data
		return nil, false
	}
	return obj, true
}

func isNull(raw json.RawMessage) bool {
	return string(bytes.TrimSpace(raw)) == "null"
}

func decodeString(raw json.RawMessage) (string, bool) {
	var s string
	if isNull(raw) || json.Unmarshal(raw, &s) != nil {
		return "", false
	}
	return s, true
}

func validTitle(s string) bool {
	n := utf8.RuneCountInString(s)
	return n >= 1 && n <= 200
}

// fields holds the validated fields of a request; a nil pointer means the
// field was absent.
type fields struct {
	title, body *string
	tagSet      bool
	tag         *string
}

// parseFields validates title, body and tag. It returns the name of the first
// invalid field, or "" if all are fine.
func parseFields(obj map[string]json.RawMessage, titleRequired bool) (fields, string) {
	var f fields
	if raw, ok := obj["title"]; ok {
		s, ok := decodeString(raw)
		if !ok || !validTitle(s) {
			return f, "title"
		}
		f.title = &s
	} else if titleRequired {
		return f, "title"
	}
	if raw, ok := obj["body"]; ok {
		s, ok := decodeString(raw)
		if !ok {
			return f, "body"
		}
		f.body = &s
	}
	if raw, ok := obj["tag"]; ok {
		f.tagSet = true
		if !isNull(raw) {
			s, ok := decodeString(raw)
			if !ok {
				return f, "tag"
			}
			f.tag = &s
		}
	}
	return f, ""
}

func serverError(w http.ResponseWriter, err error) {
	log.Printf("internal error: %v", err)
	writeError(w, http.StatusInternalServerError, "internal error")
}

func (s *server) create(w http.ResponseWriter, r *http.Request) {
	obj, ok := readObject(r)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	f, bad := parseFields(obj, true)
	if bad != "" {
		writeError(w, http.StatusBadRequest, "invalid field: "+bad)
		return
	}
	n := Note{Title: *f.title, Tag: f.tag}
	if f.body != nil {
		n.Body = *f.body
	}
	res, err := s.db.Exec(`INSERT INTO notes (title, body, tag) VALUES (?, ?, ?)`, n.Title, n.Body, n.Tag)
	if err != nil {
		serverError(w, err)
		return
	}
	n.ID, err = res.LastInsertId()
	if err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, n)
}

func (s *server) list(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	query := `SELECT id, title, body, tag FROM notes`
	var args []any
	if q != "" {
		// instr is a literal, case-sensitive substring search.
		query += ` WHERE instr(title, ?) > 0 OR instr(body, ?) > 0`
		args = append(args, q, q)
	}
	query += ` ORDER BY id ASC`
	rows, err := s.db.Query(query, args...)
	if err != nil {
		serverError(w, err)
		return
	}
	defer rows.Close()
	notes := []Note{}
	for rows.Next() {
		var n Note
		if err := rows.Scan(&n.ID, &n.Title, &n.Body, &n.Tag); err != nil {
			serverError(w, err)
			return
		}
		notes = append(notes, n)
	}
	if err := rows.Err(); err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"notes": notes})
}

type queryer interface {
	QueryRow(query string, args ...any) *sql.Row
}

func getNote(q queryer, id int64) (Note, error) {
	var n Note
	err := q.QueryRow(`SELECT id, title, body, tag FROM notes WHERE id = ?`, id).
		Scan(&n.ID, &n.Title, &n.Body, &n.Tag)
	return n, err
}

func (s *server) get(w http.ResponseWriter, id int64) {
	n, err := getNote(s.db, id)
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusNotFound, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, n)
}

func (s *server) patch(w http.ResponseWriter, r *http.Request, id int64) {
	// Check that the note exists first, so a missing note is always 404.
	if _, err := getNote(s.db, id); errors.Is(err, sql.ErrNoRows) {
		writeError(w, http.StatusNotFound, "not found")
		return
	} else if err != nil {
		serverError(w, err)
		return
	}
	obj, ok := readObject(r)
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	f, bad := parseFields(obj, false)
	if bad != "" {
		writeError(w, http.StatusBadRequest, "invalid field: "+bad)
		return
	}

	tx, err := s.db.Begin()
	if err != nil {
		serverError(w, err)
		return
	}
	defer tx.Rollback()
	n, err := getNote(tx, id)
	if errors.Is(err, sql.ErrNoRows) { // deleted in the meantime
		writeError(w, http.StatusNotFound, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	if f.title != nil {
		n.Title = *f.title
	}
	if f.body != nil {
		n.Body = *f.body
	}
	if f.tagSet {
		n.Tag = f.tag
	}
	if _, err := tx.Exec(`UPDATE notes SET title = ?, body = ?, tag = ? WHERE id = ?`, n.Title, n.Body, n.Tag, id); err != nil {
		serverError(w, err)
		return
	}
	if err := tx.Commit(); err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, n)
}

func (s *server) delete(w http.ResponseWriter, id int64) {
	res, err := s.db.Exec(`DELETE FROM notes WHERE id = ?`, id)
	if err != nil {
		serverError(w, err)
		return
	}
	if n, _ := res.RowsAffected(); n == 0 {
		writeError(w, http.StatusNotFound, "not found")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
