package main

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	sqlite3 "github.com/mattn/go-sqlite3"
)

const (
	pbkdf2Iterations = 100000
	saltBytes        = 16
	hashBytes        = 32
	tokenLength      = 32 // alphanumeric chars, ~190 bits
	tokenAlphabet    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
)

type server struct {
	db  *sql.DB
	ttl time.Duration
	// hash of a random password, used to equalise timing for unknown users
	dummySalt []byte
	dummyHash []byte
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	dbPath := os.Getenv("DB_PATH")
	if dbPath == "" {
		dbPath = "auth.db"
	}
	ttlSeconds := int64(3600)
	if v := os.Getenv("SESSION_TTL_SECONDS"); v != "" {
		n, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		if err != nil || n < 1 {
			log.Fatalf("invalid SESSION_TTL_SECONDS %q", v)
		}
		ttlSeconds = n
	}

	db, err := openDB(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	s := &server{db: db, ttl: time.Duration(ttlSeconds) * time.Second}
	s.dummySalt = make([]byte, saltBytes)
	rand.Read(s.dummySalt)
	s.dummyHash, _ = hashPassword("dummy-password", s.dummySalt)

	go s.cleanupLoop()

	srv := &http.Server{
		Addr:              net.JoinHostPort("127.0.0.1", port),
		Handler:           s,
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Fatal(srv.ListenAndServe())
}

func openDB(path string) (*sql.DB, error) {
	dsn := "file:" + path + "?_busy_timeout=10000&_journal_mode=WAL&_synchronous=NORMAL&_foreign_keys=on"
	db, err := sql.Open("sqlite3", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(8)
	_, err = db.Exec(`
CREATE TABLE IF NOT EXISTS users (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	username TEXT NOT NULL,
	username_lower TEXT NOT NULL UNIQUE,
	salt TEXT NOT NULL,
	hash TEXT NOT NULL,
	iterations INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
	token_hash TEXT PRIMARY KEY,
	user_id INTEGER NOT NULL REFERENCES users(id),
	expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_expires ON sessions(expires_at);
`)
	if err != nil {
		return nil, err
	}
	return db, nil
}

func (s *server) cleanupLoop() {
	for {
		time.Sleep(time.Minute)
		s.db.Exec(`DELETE FROM sessions WHERE expires_at <= ?`, time.Now().UnixMilli())
	}
}

func hashPassword(password string, salt []byte) ([]byte, error) {
	return pbkdf2.Key(sha256.New, password, salt, pbkdf2Iterations, hashBytes)
}

func newToken() (string, error) {
	max := big.NewInt(int64(len(tokenAlphabet)))
	b := make([]byte, tokenLength)
	for i := range b {
		n, err := rand.Int(rand.Reader, max)
		if err != nil {
			return "", err
		}
		b[i] = tokenAlphabet[n.Int64()]
	}
	return string(b), nil
}

func tokenKey(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	body, _ := json.Marshal(v)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(body)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func (s *server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	var handler func(http.ResponseWriter, *http.Request)
	var method string
	switch r.URL.Path {
	case "/register":
		handler, method = s.register, http.MethodPost
	case "/login":
		handler, method = s.login, http.MethodPost
	case "/me":
		handler, method = s.me, http.MethodGet
	case "/logout":
		handler, method = s.logout, http.MethodPost
	default:
		writeError(w, 404, "not found")
		return
	}
	if r.Method != method {
		w.Header().Set("Allow", method)
		writeError(w, 405, "method not allowed")
		return
	}
	handler(w, r)
}

// readObject parses the request body as a JSON object.
func readObject(r *http.Request) (map[string]any, bool) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		return nil, false
	}
	var obj map[string]any
	if err := json.Unmarshal(body, &obj); err != nil || obj == nil {
		return nil, false
	}
	return obj, true
}

func validUsername(u string) bool {
	if len(u) < 3 || len(u) > 32 {
		return false
	}
	for i := 0; i < len(u); i++ {
		c := u[i]
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '_':
		default:
			return false
		}
	}
	return true
}

func validPassword(p string) bool {
	n := utf8.RuneCountInString(p)
	return n >= 8 && n <= 128
}

func (s *server) register(w http.ResponseWriter, r *http.Request) {
	obj, ok := readObject(r)
	if !ok {
		writeError(w, 400, "invalid JSON")
		return
	}
	username, ok := obj["username"].(string)
	if !ok || !validUsername(username) {
		writeError(w, 400, "invalid username")
		return
	}
	password, ok := obj["password"].(string)
	if !ok || !validPassword(password) {
		writeError(w, 400, "invalid password")
		return
	}

	salt := make([]byte, saltBytes)
	if _, err := rand.Read(salt); err != nil {
		writeError(w, 500, "internal error")
		return
	}
	hash, err := hashPassword(password, salt)
	if err != nil {
		writeError(w, 500, "internal error")
		return
	}
	_, err = s.db.Exec(`INSERT INTO users (username, username_lower, salt, hash, iterations) VALUES (?, ?, ?, ?, ?)`,
		username, strings.ToLower(username),
		base64.StdEncoding.EncodeToString(salt), base64.StdEncoding.EncodeToString(hash), pbkdf2Iterations)
	if err != nil {
		var se sqlite3.Error
		if errors.As(err, &se) && se.Code == sqlite3.ErrConstraint {
			writeError(w, 409, "username taken")
			return
		}
		log.Printf("register: %v", err)
		writeError(w, 500, "internal error")
		return
	}
	writeJSON(w, 201, map[string]string{"username": username})
}

func (s *server) login(w http.ResponseWriter, r *http.Request) {
	obj, ok := readObject(r)
	if !ok {
		writeError(w, 400, "invalid JSON")
		return
	}
	username, ok := obj["username"].(string)
	if !ok {
		writeError(w, 400, "invalid username")
		return
	}
	password, ok := obj["password"].(string)
	if !ok {
		writeError(w, 400, "invalid password")
		return
	}

	var id int64
	var saltB, hashB string
	var iters int
	err := s.db.QueryRow(`SELECT id, salt, hash, iterations FROM users WHERE username_lower = ?`,
		strings.ToLower(username)).Scan(&id, &saltB, &hashB, &iters)
	if errors.Is(err, sql.ErrNoRows) {
		hashPassword(password, s.dummySalt) // equalise timing
		writeError(w, 401, "invalid credentials")
		return
	}
	if err != nil {
		log.Printf("login: %v", err)
		writeError(w, 500, "internal error")
		return
	}
	salt, err1 := base64.StdEncoding.DecodeString(saltB)
	want, err2 := base64.StdEncoding.DecodeString(hashB)
	if err1 != nil || err2 != nil {
		writeError(w, 500, "internal error")
		return
	}
	got, _ := pbkdf2.Key(sha256.New, password, salt, iters, hashBytes)
	if subtle.ConstantTimeCompare(got, want) != 1 {
		writeError(w, 401, "invalid credentials")
		return
	}

	token, err := newToken()
	if err != nil {
		writeError(w, 500, "internal error")
		return
	}
	expires := time.Now().Add(s.ttl).UnixMilli()
	if _, err := s.db.Exec(`INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)`,
		tokenKey(token), id, expires); err != nil {
		log.Printf("login: %v", err)
		writeError(w, 500, "internal error")
		return
	}
	writeJSON(w, 200, map[string]string{"token": token})
}

// bearer extracts a well-formed token from the Authorization header.
func bearer(r *http.Request) (string, bool) {
	h := r.Header.Get("Authorization")
	if len(h) < 8 || !strings.EqualFold(h[:7], "Bearer ") {
		return "", false
	}
	t := h[7:]
	if t == "" || len(t) > 256 {
		return "", false
	}
	for i := 0; i < len(t); i++ {
		c := t[i]
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9') {
			return "", false
		}
	}
	return t, true
}

func (s *server) me(w http.ResponseWriter, r *http.Request) {
	token, ok := bearer(r)
	if !ok {
		writeError(w, 401, "unauthorized")
		return
	}
	var username string
	err := s.db.QueryRow(`SELECT u.username FROM sessions s JOIN users u ON u.id = s.user_id
		WHERE s.token_hash = ? AND s.expires_at > ?`, tokenKey(token), time.Now().UnixMilli()).Scan(&username)
	if errors.Is(err, sql.ErrNoRows) {
		writeError(w, 401, "unauthorized")
		return
	}
	if err != nil {
		log.Printf("me: %v", err)
		writeError(w, 500, "internal error")
		return
	}
	writeJSON(w, 200, map[string]string{"username": username})
}

func (s *server) logout(w http.ResponseWriter, r *http.Request) {
	token, ok := bearer(r)
	if !ok {
		writeError(w, 401, "unauthorized")
		return
	}
	res, err := s.db.Exec(`DELETE FROM sessions WHERE token_hash = ? AND expires_at > ?`,
		tokenKey(token), time.Now().UnixMilli())
	if err != nil {
		log.Printf("logout: %v", err)
		writeError(w, 500, "internal error")
		return
	}
	if n, _ := res.RowsAffected(); n == 0 {
		writeError(w, 401, "unauthorized")
		return
	}
	w.WriteHeader(204)
}

var _ = fmt.Sprint
