package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"

	_ "github.com/mattn/go-sqlite3"
)

var (
	db *sql.DB
	mu sync.Mutex // Protects concurrent access to idempotency operations
)

// Event represents an event
type Event struct {
	ID        int    `json:"id"`
	Name      string `json:"name"`
	Capacity  int    `json:"capacity"`
	Remaining int    `json:"remaining"`
}

// Reservation represents a reservation
type Reservation struct {
	ID      int    `json:"id"`
	EventID int    `json:"event_id"`
	Seats   int    `json:"seats"`
	Status  string `json:"status"`
}

// ErrorResponse represents an error response
type ErrorResponse struct {
	Error string `json:"error"`
}

func init() {
	var err error
	dbPath := os.Getenv("DB_PATH")
	if dbPath == "" {
		log.Fatal("DB_PATH not set")
	}

	db, err = sql.Open("sqlite3", dbPath)
	if err != nil {
		log.Fatal(err)
	}

	if err := db.Ping(); err != nil {
		log.Fatal(err)
	}

	// Enable WAL mode for better concurrency
	if _, err := db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		log.Fatal(err)
	}

	// Create tables if they don't exist
	schema := `
	CREATE TABLE IF NOT EXISTS events (
		id INTEGER PRIMARY KEY,
		name TEXT NOT NULL,
		capacity INTEGER NOT NULL
	);
	CREATE TABLE IF NOT EXISTS reservations (
		id INTEGER PRIMARY KEY,
		event_id INTEGER NOT NULL,
		seats INTEGER NOT NULL,
		status TEXT NOT NULL DEFAULT 'active',
		FOREIGN KEY(event_id) REFERENCES events(id)
	);
	CREATE TABLE IF NOT EXISTS idempotency_keys (
		key TEXT PRIMARY KEY,
		event_id INTEGER NOT NULL,
		seats INTEGER NOT NULL,
		reservation_id INTEGER NOT NULL,
		FOREIGN KEY(reservation_id) REFERENCES reservations(id)
	);
	`

	if _, err := db.Exec(schema); err != nil {
		log.Fatal(err)
	}
}

func main() {
	defer db.Close()

	port := os.Getenv("PORT")
	if port == "" {
		log.Fatal("PORT not set")
	}

	http.HandleFunc("/", handleRequest)

	addr := "127.0.0.1:" + port
	log.Printf("Listening on %s\n", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatal(err)
	}
}

func handleRequest(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	method := r.Method

	// Handle specific endpoints
	if strings.HasPrefix(path, "/events/") && strings.HasSuffix(path, "/reservations") {
		// POST /events/<id>/reservations
		if method == http.MethodPost {
			handleCreateReservation(w, r)
			return
		}
		// Other methods on this path
		if method == http.MethodGet || method == http.MethodDelete || method == http.MethodPut || method == http.MethodPatch {
			respondError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	if strings.HasPrefix(path, "/events/") && !strings.Contains(strings.TrimPrefix(path, "/events/"), "/") {
		// GET /events/<id>
		if method == http.MethodGet {
			handleGetEvent(w, r)
			return
		}
		// POST and other methods on this path
		if method == http.MethodPost || method == http.MethodDelete || method == http.MethodPut || method == http.MethodPatch {
			respondError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	if path == "/events" {
		// POST /events
		if method == http.MethodPost {
			handleCreateEvent(w, r)
			return
		}
		// Other methods
		if method == http.MethodGet || method == http.MethodDelete || method == http.MethodPut || method == http.MethodPatch {
			respondError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	if strings.HasPrefix(path, "/reservations/") && !strings.Contains(strings.TrimPrefix(path, "/reservations/"), "/") {
		// GET /reservations/<id> or DELETE /reservations/<id>
		if method == http.MethodGet {
			handleGetReservation(w, r)
			return
		}
		if method == http.MethodDelete {
			handleCancelReservation(w, r)
			return
		}
		// Other methods
		if method == http.MethodPost || method == http.MethodPut || method == http.MethodPatch {
			respondError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	// Unknown path
	respondError(w, http.StatusNotFound, "not found")
}

func handleCreateEvent(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name     string `json:"name"`
		Capacity int    `json:"capacity"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	// Validate name
	if req.Name == "" {
		respondError(w, http.StatusBadRequest, "invalid name")
		return
	}

	// Validate capacity
	if req.Capacity < 1 || req.Capacity > 100000 {
		respondError(w, http.StatusBadRequest, "invalid capacity")
		return
	}

	result, err := db.Exec("INSERT INTO events (name, capacity) VALUES (?, ?)", req.Name, req.Capacity)
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	id, err := result.LastInsertId()
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	event := Event{
		ID:        int(id),
		Name:      req.Name,
		Capacity:  req.Capacity,
		Remaining: req.Capacity,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(event)
}

func handleGetEvent(w http.ResponseWriter, r *http.Request) {
	idStr := strings.TrimPrefix(r.URL.Path, "/events/")
	id, err := strconv.Atoi(idStr)
	if err != nil || id <= 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	var name string
	var capacity int
	err = db.QueryRow("SELECT name, capacity FROM events WHERE id = ?", id).Scan(&name, &capacity)
	if err != nil {
		if err == sql.ErrNoRows {
			respondError(w, http.StatusNotFound, "not found")
		} else {
			respondError(w, http.StatusInternalServerError, "internal error")
		}
		return
	}

	// Calculate remaining seats
	var reservedSeats int
	err = db.QueryRow(`
		SELECT COALESCE(SUM(seats), 0) FROM reservations
		WHERE event_id = ? AND status = 'active'
	`, id).Scan(&reservedSeats)
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	remaining := capacity - reservedSeats

	event := Event{
		ID:        id,
		Name:      name,
		Capacity:  capacity,
		Remaining: remaining,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(event)
}

func handleCreateReservation(w http.ResponseWriter, r *http.Request) {
	// Extract event ID from path /events/<id>/reservations
	pathParts := strings.Split(strings.TrimPrefix(r.URL.Path, "/events/"), "/")
	eventIDStr := pathParts[0]
	eventID, err := strconv.Atoi(eventIDStr)
	if err != nil || eventID <= 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	var req struct {
		Seats int `json:"seats"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	// Validate seats
	if req.Seats < 1 {
		respondError(w, http.StatusBadRequest, "invalid seats")
		return
	}

	// Get idempotency key if present
	idempotencyKey := r.Header.Get("Idempotency-Key")

	mu.Lock()
	defer mu.Unlock()

	// Check if event exists (validation before idempotency check)
	var eventCapacity int
	err = db.QueryRow("SELECT capacity FROM events WHERE id = ?", eventID).Scan(&eventCapacity)
	if err != nil {
		if err == sql.ErrNoRows {
			respondError(w, http.StatusNotFound, "not found")
		} else {
			respondError(w, http.StatusInternalServerError, "internal error")
		}
		return
	}

	// Handle idempotency
	if idempotencyKey != "" {
		var existingReservationID int
		var existingEventID int
		var existingSeats int
		err := db.QueryRow(`
			SELECT reservation_id, event_id, seats FROM idempotency_keys WHERE key = ?
		`, idempotencyKey).Scan(&existingReservationID, &existingEventID, &existingSeats)

		if err == nil {
			// Key already exists
			if existingEventID != eventID || existingSeats != req.Seats {
				respondError(w, http.StatusUnprocessableEntity, "idempotency key reused")
				return
			}
			// Return the existing reservation
			var reservation Reservation
			err := db.QueryRow(`
				SELECT id, event_id, seats, status FROM reservations WHERE id = ?
			`, existingReservationID).Scan(&reservation.ID, &reservation.EventID, &reservation.Seats, &reservation.Status)
			if err != nil {
				respondError(w, http.StatusInternalServerError, "internal error")
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			json.NewEncoder(w).Encode(reservation)
			return
		} else if err != sql.ErrNoRows {
			respondError(w, http.StatusInternalServerError, "internal error")
			return
		}
		// Key doesn't exist, proceed to create reservation
	}

	// Begin transaction
	tx, err := db.Begin()
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}
	defer tx.Rollback()

	// Get current active reservations
	var reservedSeats int
	err = tx.QueryRow(`
		SELECT COALESCE(SUM(seats), 0) FROM reservations
		WHERE event_id = ? AND status = 'active'
	`, eventID).Scan(&reservedSeats)
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	remaining := eventCapacity - reservedSeats

	// Check if enough seats
	if req.Seats > remaining {
		respondError(w, http.StatusConflict, "not enough seats")
		return
	}

	// Create reservation
	result, err := tx.Exec(`
		INSERT INTO reservations (event_id, seats, status) VALUES (?, ?, 'active')
	`, eventID, req.Seats)
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	reservationID, err := result.LastInsertId()
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	// Store idempotency key if provided
	if idempotencyKey != "" {
		_, err := tx.Exec(`
			INSERT INTO idempotency_keys (key, event_id, seats, reservation_id) VALUES (?, ?, ?, ?)
		`, idempotencyKey, eventID, req.Seats, reservationID)
		if err != nil {
			respondError(w, http.StatusInternalServerError, "internal error")
			return
		}
	}

	if err := tx.Commit(); err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	reservation := Reservation{
		ID:      int(reservationID),
		EventID: eventID,
		Seats:   req.Seats,
		Status:  "active",
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(reservation)
}

func handleGetReservation(w http.ResponseWriter, r *http.Request) {
	idStr := strings.TrimPrefix(r.URL.Path, "/reservations/")
	id, err := strconv.Atoi(idStr)
	if err != nil || id <= 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	var eventID int
	var seats int
	var status string
	err = db.QueryRow(`
		SELECT id, event_id, seats, status FROM reservations WHERE id = ?
	`, id).Scan(&id, &eventID, &seats, &status)
	if err != nil {
		if err == sql.ErrNoRows {
			respondError(w, http.StatusNotFound, "not found")
		} else {
			respondError(w, http.StatusInternalServerError, "internal error")
		}
		return
	}

	reservation := Reservation{
		ID:      id,
		EventID: eventID,
		Seats:   seats,
		Status:  status,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(reservation)
}

func handleCancelReservation(w http.ResponseWriter, r *http.Request) {
	idStr := strings.TrimPrefix(r.URL.Path, "/reservations/")
	id, err := strconv.Atoi(idStr)
	if err != nil || id <= 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	mu.Lock()
	defer mu.Unlock()

	// Get the reservation
	var eventID int
	var seats int
	var status string
	err = db.QueryRow(`
		SELECT id, event_id, seats, status FROM reservations WHERE id = ?
	`, id).Scan(&id, &eventID, &seats, &status)
	if err != nil {
		if err == sql.ErrNoRows {
			respondError(w, http.StatusNotFound, "not found")
		} else {
			respondError(w, http.StatusInternalServerError, "internal error")
		}
		return
	}

	// Check if already cancelled
	if status == "cancelled" {
		respondError(w, http.StatusConflict, "already cancelled")
		return
	}

	// Update status to cancelled
	_, err = db.Exec("UPDATE reservations SET status = 'cancelled' WHERE id = ?", id)
	if err != nil {
		respondError(w, http.StatusInternalServerError, "internal error")
		return
	}

	reservation := Reservation{
		ID:      id,
		EventID: eventID,
		Seats:   seats,
		Status:  "cancelled",
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(reservation)
}

func respondError(w http.ResponseWriter, statusCode int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	json.NewEncoder(w).Encode(ErrorResponse{Error: message})
}
