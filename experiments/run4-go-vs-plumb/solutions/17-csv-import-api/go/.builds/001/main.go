package main

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"unicode"
	"unicode/utf8"

	_ "github.com/mattn/go-sqlite3"
)

var errInvalidCSV = errors.New("invalid csv")

// parseCSV parses the restricted CSV dialect of the spec into records.
func parseCSV(data []byte) ([][]string, error) {
	var records [][]string
	i, n := 0, len(data)
	for i < n {
		var rec []string
		for {
			var sb strings.Builder
			if data[i] == '"' {
				i++
				closed := false
				for i < n {
					c := data[i]
					if c == '"' {
						if i+1 < n && data[i+1] == '"' {
							sb.WriteByte('"')
							i += 2
							continue
						}
						i++
						closed = true
						break
					}
					sb.WriteByte(c)
					i++
				}
				if !closed {
					return nil, errInvalidCSV
				}
				if i < n && data[i] != ',' && data[i] != '\n' && !(data[i] == '\r' && i+1 < n && data[i+1] == '\n') {
					return nil, errInvalidCSV
				}
			} else {
				for i < n {
					c := data[i]
					if c == ',' || c == '\n' || (c == '\r' && i+1 < n && data[i+1] == '\n') {
						break
					}
					if c == '"' {
						return nil, errInvalidCSV
					}
					sb.WriteByte(c)
					i++
				}
			}
			rec = append(rec, sb.String())
			if i < n && data[i] == ',' {
				i++
				if i == n { // trailing comma at EOF: one more empty field
					rec = append(rec, "")
					break
				}
				continue
			}
			break
		}
		// consume line break
		if i < n {
			if data[i] == '\r' {
				i += 2
			} else {
				i++
			}
		}
		records = append(records, rec)
	}
	return records, nil
}

type rowError struct {
	Row     int    `json:"row"`
	Field   string `json:"field"`
	Message string `json:"message"`
}

func validEmail(e string) bool {
	if utf8.RuneCountInString(e) > 254 {
		return false
	}
	for _, r := range e {
		if unicode.IsSpace(r) {
			return false
		}
	}
	if strings.Count(e, "@") != 1 {
		return false
	}
	at := strings.IndexByte(e, '@')
	if at == 0 {
		return false
	}
	domain := e[at+1:]
	// a '.' that is neither first nor last character of the domain
	if len(domain) < 3 {
		return false
	}
	return strings.Contains(domain[1:len(domain)-1], ".")
}

func validAge(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	t := strings.TrimLeft(s, "0")
	if len(t) == 0 || len(t) > 3 {
		return false
	}
	v := 0
	for i := 0; i < len(t); i++ {
		v = v*10 + int(t[i]-'0')
	}
	return v >= 13 && v <= 120
}

type server struct {
	db *sql.DB
	mu sync.Mutex // serializes imports
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.Encode(v)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}

func (s *server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch r.URL.Path {
	case "/import":
		if r.Method != http.MethodPost {
			writeJSON(w, 405, map[string]string{"error": "method not allowed"})
			return
		}
		s.handleImport(w, r)
	case "/users":
		if r.Method != http.MethodGet {
			writeJSON(w, 405, map[string]string{"error": "method not allowed"})
			return
		}
		s.handleUsers(w)
	default:
		writeJSON(w, 404, map[string]string{"error": "not found"})
	}
}

func badCSV(w http.ResponseWriter) {
	writeJSON(w, 400, map[string]string{"error": "invalid csv"})
}

func (s *server) handleImport(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(r.Body)
	if err != nil || len(body) == 0 || !utf8.Valid(body) {
		badCSV(w)
		return
	}
	records, err := parseCSV(body)
	if err != nil || len(records) == 0 {
		badCSV(w)
		return
	}
	header := records[0]
	if len(header) != 3 {
		badCSV(w)
		return
	}
	col := map[string]int{}
	for i, h := range header {
		if h != "email" && h != "name" && h != "age" {
			badCSV(w)
			return
		}
		if _, dup := col[h]; dup {
			badCSV(w)
			return
		}
		col[h] = i
	}
	rows := records[1:]

	s.mu.Lock()
	defer s.mu.Unlock()

	tx, err := s.db.Begin()
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "internal error"})
		return
	}
	defer tx.Rollback()

	type user struct {
		email, name, age string
	}
	var users []user
	errs := []rowError{}
	seen := map[string]bool{}
	for idx, rec := range rows {
		rn := idx + 1
		if len(rec) != 3 {
			errs = append(errs, rowError{rn, "row", "wrong number of fields"})
			continue
		}
		email, name, age := rec[col["email"]], rec[col["name"]], rec[col["age"]]
		if !validEmail(email) {
			errs = append(errs, rowError{rn, "email", "invalid email"})
		} else {
			lower := strings.ToLower(email)
			var one int
			err := tx.QueryRow("SELECT 1 FROM users WHERE email = ?", lower).Scan(&one)
			switch {
			case err == nil:
				errs = append(errs, rowError{rn, "email", "email already exists"})
			case err != sql.ErrNoRows:
				writeJSON(w, 500, map[string]string{"error": "internal error"})
				return
			case seen[lower]:
				errs = append(errs, rowError{rn, "email", "duplicate email in file"})
			}
			seen[lower] = true
			email = lower
		}
		if n := utf8.RuneCountInString(name); n < 1 || n > 100 {
			errs = append(errs, rowError{rn, "name", "invalid name"})
		}
		if !validAge(age) {
			errs = append(errs, rowError{rn, "age", "invalid age"})
		}
		users = append(users, user{email, name, age})
	}
	if len(errs) > 0 {
		writeJSON(w, 422, map[string]any{"imported": 0, "errors": errs})
		return
	}
	for _, u := range users {
		a := 0
		for _, c := range strings.TrimLeft(u.age, "0") {
			a = a*10 + int(c-'0')
		}
		if _, err := tx.Exec("INSERT INTO users (email, name, age) VALUES (?, ?, ?)", u.email, u.name, a); err != nil {
			writeJSON(w, 500, map[string]string{"error": "internal error"})
			return
		}
	}
	if err := tx.Commit(); err != nil {
		writeJSON(w, 500, map[string]string{"error": "internal error"})
		return
	}
	writeJSON(w, 201, map[string]int{"imported": len(users)})
}

func (s *server) handleUsers(w http.ResponseWriter) {
	rows, err := s.db.Query("SELECT email, name, age FROM users")
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "internal error"})
		return
	}
	defer rows.Close()
	type user struct {
		Email string `json:"email"`
		Name  string `json:"name"`
		Age   int    `json:"age"`
	}
	users := []user{}
	for rows.Next() {
		var u user
		if err := rows.Scan(&u.Email, &u.Name, &u.Age); err != nil {
			writeJSON(w, 500, map[string]string{"error": "internal error"})
			return
		}
		users = append(users, u)
	}
	// Go string comparison is bytewise, which for UTF-8 is code point order.
	sort.Slice(users, func(i, j int) bool { return users[i].Email < users[j].Email })
	writeJSON(w, 200, map[string]any{"users": users})
}

func main() {
	port := os.Getenv("PORT")
	path := os.Getenv("DB_PATH")
	if port == "" || path == "" {
		log.Fatal("PORT and DB_PATH must be set")
	}
	db, err := sql.Open("sqlite3", path+"?_busy_timeout=10000&_journal_mode=WAL&_synchronous=FULL")
	if err != nil {
		log.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	_, err = db.Exec(`CREATE TABLE IF NOT EXISTS users (
		email TEXT PRIMARY KEY,
		name TEXT NOT NULL,
		age INTEGER NOT NULL)`)
	if err != nil {
		log.Fatal(err)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		log.Fatal(err)
	}
	log.Fatal(http.Serve(ln, &server{db: db}))
}
