package main

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"

	_ "github.com/mattn/go-sqlite3"
)

// All database work is serialised by mu, and the pool holds one connection,
// so every check-then-write sequence is atomic.
var (
	db *sql.DB
	mu sync.Mutex
)

const schema = `
CREATE TABLE IF NOT EXISTS events (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	name TEXT NOT NULL,
	capacity INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS reservations (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	event_id INTEGER NOT NULL REFERENCES events(id),
	seats INTEGER NOT NULL,
	status TEXT NOT NULL,
	idem_key TEXT UNIQUE
);
CREATE INDEX IF NOT EXISTS reservations_event ON reservations(event_id, status);
`

type Event struct {
	ID        int64  `json:"id"`
	Name      string `json:"name"`
	Capacity  int64  `json:"capacity"`
	Remaining int64  `json:"remaining"`
}

type Reservation struct {
	ID      int64  `json:"id"`
	EventID int64  `json:"event_id"`
	Seats   int64  `json:"seats"`
	Status  string `json:"status"`
}

func main() {
	path := os.Getenv("DB_PATH")
	if path == "" {
		path = "reservations.db"
	}
	var err error
	db, err = sql.Open("sqlite3", "file:"+path+"?_busy_timeout=10000&_journal_mode=WAL&_synchronous=FULL&_foreign_keys=on")
	if err != nil {
		log.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	if _, err := db.Exec(schema); err != nil {
		log.Fatal(err)
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	ln, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", port))
	if err != nil {
		log.Fatal(err)
	}
	log.Fatal(http.Serve(ln, http.HandlerFunc(handle)))
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

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

var idRe = regexp.MustCompile(`^[0-9]+$`)

// parseID accepts only a positive integer made of digits.
func parseID(s string) (int64, bool) {
	if !idRe.MatchString(s) {
		return 0, false
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n < 1 {
		return 0, false
	}
	return n, true
}

func handle(w http.ResponseWriter, r *http.Request) {
	p := r.URL.Path
	parts := strings.Split(strings.TrimPrefix(p, "/"), "/")
	if !strings.HasPrefix(p, "/") {
		writeErr(w, 404, "not found")
		return
	}
	switch {
	case len(parts) == 1 && parts[0] == "events":
		if r.Method != http.MethodPost {
			writeErr(w, 405, "method not allowed")
			return
		}
		createEvent(w, r)
	case len(parts) == 2 && parts[0] == "events":
		id, ok := parseID(parts[1])
		if !ok {
			writeErr(w, 404, "not found")
			return
		}
		if r.Method != http.MethodGet {
			writeErr(w, 405, "method not allowed")
			return
		}
		getEvent(w, id)
	case len(parts) == 3 && parts[0] == "events" && parts[2] == "reservations":
		id, ok := parseID(parts[1])
		if !ok {
			writeErr(w, 404, "not found")
			return
		}
		if r.Method != http.MethodPost {
			writeErr(w, 405, "method not allowed")
			return
		}
		createReservation(w, r, id)
	case len(parts) == 2 && parts[0] == "reservations":
		id, ok := parseID(parts[1])
		if !ok {
			writeErr(w, 404, "not found")
			return
		}
		switch r.Method {
		case http.MethodGet:
			getReservation(w, id)
		case http.MethodDelete:
			cancelReservation(w, id)
		default:
			writeErr(w, 405, "method not allowed")
		}
	default:
		writeErr(w, 404, "not found")
	}
}

// readObject decodes the body as a single JSON object, keeping numbers as text.
func readObject(r *http.Request) (map[string]any, bool) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, false
	}
	dec := json.NewDecoder(bytes.NewReader(body))
	dec.UseNumber()
	var obj map[string]any
	if err := dec.Decode(&obj); err != nil || obj == nil {
		return nil, false
	}
	if _, err := dec.Token(); err != io.EOF {
		return nil, false
	}
	return obj, true
}

var intRe = regexp.MustCompile(`^-?[0-9]+$`)

// intField returns the value of an integer field. ok is false when the field
// is missing or not an integer; the value is arbitrary precision.
func intField(obj map[string]any, key string) (*big.Int, bool) {
	n, ok := obj[key].(json.Number)
	if !ok || !intRe.MatchString(n.String()) {
		return nil, false
	}
	v, ok := new(big.Int).SetString(n.String(), 10)
	return v, ok
}

func serverError(w http.ResponseWriter, err error) {
	log.Println("error:", err)
	writeErr(w, 500, "internal error")
}

func remaining(q interface {
	QueryRow(string, ...any) *sql.Row
}, id int64) (Event, error) {
	var e Event
	err := q.QueryRow(`SELECT e.id, e.name, e.capacity,
		e.capacity - COALESCE((SELECT SUM(seats) FROM reservations WHERE event_id = e.id AND status = 'active'), 0)
		FROM events e WHERE e.id = ?`, id).Scan(&e.ID, &e.Name, &e.Capacity, &e.Remaining)
	return e, err
}

func createEvent(w http.ResponseWriter, r *http.Request) {
	obj, ok := readObject(r)
	if !ok {
		writeErr(w, 400, "invalid JSON")
		return
	}
	name, ok := obj["name"].(string)
	if !ok || name == "" {
		writeErr(w, 400, "invalid name")
		return
	}
	c, ok := intField(obj, "capacity")
	if !ok || c.Cmp(big.NewInt(1)) < 0 || c.Cmp(big.NewInt(100000)) > 0 {
		writeErr(w, 400, "invalid capacity")
		return
	}
	capacity := c.Int64()
	mu.Lock()
	defer mu.Unlock()
	res, err := db.Exec(`INSERT INTO events (name, capacity) VALUES (?, ?)`, name, capacity)
	if err != nil {
		serverError(w, err)
		return
	}
	id, _ := res.LastInsertId()
	writeJSON(w, 201, Event{id, name, capacity, capacity})
}

func getEvent(w http.ResponseWriter, id int64) {
	mu.Lock()
	defer mu.Unlock()
	e, err := remaining(db, id)
	if errors.Is(err, sql.ErrNoRows) {
		writeErr(w, 404, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, 200, e)
}

func createReservation(w http.ResponseWriter, r *http.Request, eventID int64) {
	obj, ok := readObject(r)
	if !ok {
		writeErr(w, 400, "invalid JSON")
		return
	}
	s, ok := intField(obj, "seats")
	if !ok || s.Cmp(big.NewInt(1)) < 0 {
		writeErr(w, 400, "invalid seats")
		return
	}
	huge := !s.IsInt64()
	var seats int64
	if !huge {
		seats = s.Int64()
	}
	key := r.Header.Get("Idempotency-Key")

	mu.Lock()
	defer mu.Unlock()

	if key != "" {
		var rv Reservation
		err := db.QueryRow(`SELECT id, event_id, seats, status FROM reservations WHERE idem_key = ?`, key).
			Scan(&rv.ID, &rv.EventID, &rv.Seats, &rv.Status)
		if err == nil {
			if rv.EventID == eventID && !huge && rv.Seats == seats {
				writeJSON(w, 201, rv)
			} else {
				writeErr(w, 422, "idempotency key reused")
			}
			return
		}
		if !errors.Is(err, sql.ErrNoRows) {
			serverError(w, err)
			return
		}
	}

	tx, err := db.Begin()
	if err != nil {
		serverError(w, err)
		return
	}
	defer tx.Rollback()
	e, err := remaining(tx, eventID)
	if errors.Is(err, sql.ErrNoRows) {
		writeErr(w, 404, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	if huge || e.Remaining < seats {
		writeErr(w, 409, "not enough seats")
		return
	}
	var idem any
	if key != "" {
		idem = key
	}
	res, err := tx.Exec(`INSERT INTO reservations (event_id, seats, status, idem_key) VALUES (?, ?, 'active', ?)`, eventID, seats, idem)
	if err != nil {
		serverError(w, err)
		return
	}
	id, _ := res.LastInsertId()
	if err := tx.Commit(); err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, 201, Reservation{id, eventID, seats, "active"})
}

func getReservation(w http.ResponseWriter, id int64) {
	mu.Lock()
	defer mu.Unlock()
	var rv Reservation
	err := db.QueryRow(`SELECT id, event_id, seats, status FROM reservations WHERE id = ?`, id).
		Scan(&rv.ID, &rv.EventID, &rv.Seats, &rv.Status)
	if errors.Is(err, sql.ErrNoRows) {
		writeErr(w, 404, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	writeJSON(w, 200, rv)
}

func cancelReservation(w http.ResponseWriter, id int64) {
	mu.Lock()
	defer mu.Unlock()
	var rv Reservation
	err := db.QueryRow(`SELECT id, event_id, seats, status FROM reservations WHERE id = ?`, id).
		Scan(&rv.ID, &rv.EventID, &rv.Seats, &rv.Status)
	if errors.Is(err, sql.ErrNoRows) {
		writeErr(w, 404, "not found")
		return
	}
	if err != nil {
		serverError(w, err)
		return
	}
	if rv.Status == "cancelled" {
		writeErr(w, 409, "already cancelled")
		return
	}
	if _, err := db.Exec(`UPDATE reservations SET status = 'cancelled' WHERE id = ?`, id); err != nil {
		serverError(w, err)
		return
	}
	rv.Status = "cancelled"
	writeJSON(w, 200, rv)
}
