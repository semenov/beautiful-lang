// A small notes service, shaped like a real app's backend: the same API as
// ../lang/server.lang, written the usual Go way (net/http, database/sql
// with mattn/go-sqlite3, encoding/json, log/slog).
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

type User struct {
	ID    int64  `json:"id"`
	Name  string `json:"name"`
	Email string `json:"email"`
}

type Note struct {
	ID        int64  `json:"id"`
	UserID    int64  `json:"user_id"`
	Title     string `json:"title"`
	Body      string `json:"body"`
	CreatedAt int64  `json:"created_at"`
}

var secret = []byte("bench-secret")

func sign(userID int64) string {
	m := hmac.New(sha256.New, secret)
	m.Write([]byte(strconv.FormatInt(userID, 10)))
	return strconv.FormatInt(userID, 10) + "." + hex.EncodeToString(m.Sum(nil))
}

func userOf(r *http.Request) (int64, bool) {
	auth := r.Header.Get("Authorization")
	token, ok := strings.CutPrefix(auth, "Bearer ")
	if !ok {
		return 0, false
	}
	idText, _, ok := strings.Cut(token, ".")
	if !ok {
		return 0, false
	}
	id, err := strconv.ParseInt(idText, 10, 64)
	if err != nil || sign(id) != token {
		return 0, false
	}
	return id, true
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func failure(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

type server struct{ db *sql.DB }

func (s *server) createUser(w http.ResponseWriter, r *http.Request) {
	var in struct{ Name, Email string }
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		failure(w, 400, err.Error())
		return
	}
	if strings.TrimSpace(in.Name) == "" || len([]rune(in.Name)) > 100 {
		failure(w, 422, "name: 1 to 100 characters")
		return
	}
	if !strings.Contains(in.Email, "@") || len([]rune(in.Email)) > 200 {
		failure(w, 422, "email: not an email address")
		return
	}
	res, err := s.db.Exec("insert into users (name, email) values (?, ?)", in.Name, in.Email)
	if err != nil {
		failure(w, 409, "email: already registered")
		return
	}
	id, _ := res.LastInsertId()
	writeJSON(w, 201, User{id, in.Name, in.Email})
}

func (s *server) login(w http.ResponseWriter, r *http.Request) {
	var in struct{ Email string }
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		failure(w, 400, err.Error())
		return
	}
	var u User
	err := s.db.QueryRow("select id, name, email from users where email = ?", in.Email).Scan(&u.ID, &u.Name, &u.Email)
	if errors.Is(err, sql.ErrNoRows) {
		failure(w, 404, "no such user")
		return
	}
	if err != nil {
		failure(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"token": sign(u.ID), "user_id": u.ID})
}

func (s *server) listNotes(w http.ResponseWriter, r *http.Request) {
	user, ok := userOf(r)
	if !ok {
		failure(w, 401, "log in first")
		return
	}
	limit, err := strconv.Atoi(r.URL.Query().Get("limit"))
	if err != nil {
		limit = 20
	}
	limit = max(1, min(limit, 100))
	rows, err := s.db.Query("select id, user_id, title, body, created_at from notes where user_id = ? order by id desc limit ?", user, limit)
	if err != nil {
		failure(w, 500, err.Error())
		return
	}
	defer rows.Close()
	notes := []Note{}
	for rows.Next() {
		var n Note
		if err := rows.Scan(&n.ID, &n.UserID, &n.Title, &n.Body, &n.CreatedAt); err != nil {
			failure(w, 500, err.Error())
			return
		}
		notes = append(notes, n)
	}
	writeJSON(w, 200, notes)
}

func (s *server) getNote(w http.ResponseWriter, r *http.Request) {
	user, ok := userOf(r)
	if !ok {
		failure(w, 401, "log in first")
		return
	}
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		failure(w, 400, "id: not a number")
		return
	}
	var n Note
	err = s.db.QueryRow("select id, user_id, title, body, created_at from notes where id = ? and user_id = ?", id, user).Scan(&n.ID, &n.UserID, &n.Title, &n.Body, &n.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		failure(w, 404, "no such note")
		return
	}
	if err != nil {
		failure(w, 500, err.Error())
		return
	}
	writeJSON(w, 200, n)
}

func (s *server) createNote(w http.ResponseWriter, r *http.Request) {
	user, ok := userOf(r)
	if !ok {
		failure(w, 401, "log in first")
		return
	}
	var in struct{ Title, Body string }
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		failure(w, 400, err.Error())
		return
	}
	if strings.TrimSpace(in.Title) == "" || len([]rune(in.Title)) > 200 {
		failure(w, 422, "title: 1 to 200 characters")
		return
	}
	if len([]rune(in.Body)) > 10000 {
		failure(w, 422, "body: at most 10000 characters")
		return
	}
	now := time.Now().Unix()
	res, err := s.db.Exec("insert into notes (user_id, title, body, created_at) values (?, ?, ?, ?)", user, in.Title, in.Body, now)
	if err != nil {
		failure(w, 500, err.Error())
		return
	}
	id, _ := res.LastInsertId()
	writeJSON(w, 201, Note{id, user, in.Title, in.Body, now})
}

// request logs, as lang's http.log_requests: one line per request
func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: 200}
		next.ServeHTTP(rec, r)
		slog.Info("request", "method", r.Method, "path", r.URL.Path, "status", rec.status, "ms", time.Since(start).Milliseconds())
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8301"
	}
	path := os.Getenv("DB")
	if path == "" {
		path = "notes.db"
	}
	db, err := sql.Open("sqlite3", path+"?_journal_mode=WAL&_synchronous=NORMAL&_busy_timeout=5000")
	if err != nil {
		panic(err)
	}
	for _, q := range []string{
		"create table if not exists users (id integer primary key, name text not null, email text not null unique)",
		"create table if not exists notes (id integer primary key, user_id integer not null, title text not null, body text not null, created_at integer not null)",
		"create index if not exists notes_user on notes (user_id, id)",
	} {
		if _, err := db.Exec(q); err != nil {
			panic(err)
		}
	}
	s := &server{db}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /users", s.createUser)
	mux.HandleFunc("POST /login", s.login)
	mux.HandleFunc("GET /notes", s.listNotes)
	mux.HandleFunc("POST /notes", s.createNote)
	mux.HandleFunc("GET /notes/{id}", s.getNote)
	slog.Info("listening", "port", port)
	if err := http.ListenAndServe(":"+port, logRequests(mux)); err != nil {
		panic(err)
	}
}
