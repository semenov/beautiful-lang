package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"

	_ "github.com/mattn/go-sqlite3"
)

type Note struct {
	ID    int64   `json:"id"`
	Title string  `json:"title"`
	Body  string  `json:"body"`
	Tag   *string `json:"tag"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

type NotesListResponse struct {
	Notes []Note `json:"notes"`
}

var (
	db *sql.DB
	mu sync.Mutex
)

func init() {
	var err error
	dbPath := os.Getenv("DB_PATH")
	if dbPath == "" {
		fmt.Fprintf(os.Stderr, "DB_PATH not set\n")
		os.Exit(1)
	}

	db, err = sql.Open("sqlite3", dbPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to open database: %v\n", err)
		os.Exit(1)
	}

	// Enable WAL mode for better concurrency
	_, err = db.Exec("PRAGMA journal_mode=WAL")
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to set journal mode: %v\n", err)
		os.Exit(1)
	}

	// Create table if it doesn't exist
	_, err = db.Exec(`
		CREATE TABLE IF NOT EXISTS notes (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			title TEXT NOT NULL,
			body TEXT NOT NULL,
			tag TEXT
		)
	`)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create table: %v\n", err)
		os.Exit(1)
	}
}

func main() {
	defer db.Close()

	port := os.Getenv("PORT")
	if port == "" {
		fmt.Fprintf(os.Stderr, "PORT not set\n")
		os.Exit(1)
	}

	http.HandleFunc("/notes", handleNotes)
	http.HandleFunc("/notes/", handleNoteDetail)
	http.HandleFunc("/", handleCatchAll)

	addr := "127.0.0.1:" + port
	fmt.Fprintf(os.Stderr, "Listening on %s\n", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Fprintf(os.Stderr, "Server error: %v\n", err)
		os.Exit(1)
	}
}

func handleCatchAll(w http.ResponseWriter, r *http.Request) {
	respondError(w, http.StatusNotFound, "not found")
}

func handleNotes(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/notes" {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	switch r.Method {
	case http.MethodPost:
		handlePostNotes(w, r)
	case http.MethodGet:
		handleGetNotes(w, r)
	default:
		respondError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

func handleNoteDetail(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	if !strings.HasPrefix(path, "/notes/") {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	idStr := strings.TrimPrefix(path, "/notes/")
	if idStr == "" || strings.Contains(idStr, "/") {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil || id <= 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	switch r.Method {
	case http.MethodGet:
		handleGetNote(w, r, id)
	case http.MethodPatch:
		handlePatchNote(w, r, id)
	case http.MethodDelete:
		handleDeleteNote(w, r, id)
	default:
		respondError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

func handlePostNotes(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	var payload map[string]interface{}
	if err := json.Unmarshal(body, &payload); err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	// title is required
	titleVal, ok := payload["title"]
	if !ok {
		respondError(w, http.StatusBadRequest, "invalid field: title")
		return
	}

	title, ok := titleVal.(string)
	if !ok {
		respondError(w, http.StatusBadRequest, "invalid field: title")
		return
	}

	if len([]rune(title)) < 1 || len([]rune(title)) > 200 {
		respondError(w, http.StatusBadRequest, "invalid field: title")
		return
	}

	// body defaults to ""
	body_str := ""
	if bodyVal, ok := payload["body"]; ok {
		if bodyVal == nil {
			respondError(w, http.StatusBadRequest, "invalid field: body")
			return
		}
		if b, ok := bodyVal.(string); ok {
			body_str = b
		} else {
			respondError(w, http.StatusBadRequest, "invalid field: body")
			return
		}
	}

	// tag defaults to null
	var tag *string
	if tagVal, ok := payload["tag"]; ok {
		if tagVal == nil {
			tag = nil
		} else if t, ok := tagVal.(string); ok {
			tag = &t
		} else {
			respondError(w, http.StatusBadRequest, "invalid field: tag")
			return
		}
	}

	// Insert into database
	mu.Lock()
	result, err := db.Exec("INSERT INTO notes (title, body, tag) VALUES (?, ?, ?)",
		title, body_str, tag)
	mu.Unlock()

	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid field: title")
		return
	}

	id, err := result.LastInsertId()
	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	note := Note{
		ID:    id,
		Title: title,
		Body:  body_str,
		Tag:   tag,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(note)
}

func handleGetNotes(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query().Get("q")

	// URL decode the query
	if query != "" {
		query, _ = url.QueryUnescape(query)
	}

	var rows *sql.Rows
	var err error
	mu.Lock()
	if query == "" {
		rows, err = db.Query("SELECT id, title, body, tag FROM notes ORDER BY id ASC")
	} else {
		// Search: title or body contains the text as a substring (case-sensitive)
		// Escape wildcards so % and _ match literally
		escapedQuery := strings.NewReplacer("\\", "\\\\", "%", "\\%", "_", "\\_").Replace(query)
		rows, err = db.Query(`
			SELECT id, title, body, tag FROM notes
			WHERE title LIKE ? ESCAPE '\' OR body LIKE ? ESCAPE '\'
			ORDER BY id ASC
		`, "%"+escapedQuery+"%", "%"+escapedQuery+"%")
	}
	mu.Unlock()

	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	defer rows.Close()

	var notes []Note
	for rows.Next() {
		var note Note
		var tag *string
		if err := rows.Scan(&note.ID, &note.Title, &note.Body, &tag); err != nil {
			respondError(w, http.StatusBadRequest, "invalid JSON")
			return
		}
		note.Tag = tag
		notes = append(notes, note)
	}

	if notes == nil {
		notes = []Note{}
	}

	response := NotesListResponse{Notes: notes}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(response)
}

func handleGetNote(w http.ResponseWriter, r *http.Request, id int64) {
	mu.Lock()
	row := db.QueryRow("SELECT id, title, body, tag FROM notes WHERE id = ?", id)
	mu.Unlock()

	var note Note
	var tag *string
	err := row.Scan(&note.ID, &note.Title, &note.Body, &tag)
	if err == sql.ErrNoRows {
		respondError(w, http.StatusNotFound, "not found")
		return
	}
	if err != nil {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	note.Tag = tag
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(note)
}

func handlePatchNote(w http.ResponseWriter, r *http.Request, id int64) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	var payload map[string]interface{}
	if err := json.Unmarshal(body, &payload); err != nil {
		respondError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	// Get current note
	mu.Lock()
	row := db.QueryRow("SELECT id, title, body, tag FROM notes WHERE id = ?", id)
	mu.Unlock()

	var note Note
	var tag *string
	err = row.Scan(&note.ID, &note.Title, &note.Body, &tag)
	if err == sql.ErrNoRows {
		respondError(w, http.StatusNotFound, "not found")
		return
	}
	if err != nil {
		respondError(w, http.StatusNotFound, "not found")
		return
	}
	note.Tag = tag

	// Update fields if provided
	if titleVal, ok := payload["title"]; ok {
		if titleVal == nil {
			respondError(w, http.StatusBadRequest, "invalid field: title")
			return
		}
		if t, ok := titleVal.(string); ok {
			if len([]rune(t)) < 1 || len([]rune(t)) > 200 {
				respondError(w, http.StatusBadRequest, "invalid field: title")
				return
			}
			note.Title = t
		} else {
			respondError(w, http.StatusBadRequest, "invalid field: title")
			return
		}
	}

	if bodyVal, ok := payload["body"]; ok {
		if bodyVal == nil {
			respondError(w, http.StatusBadRequest, "invalid field: body")
			return
		}
		if b, ok := bodyVal.(string); ok {
			note.Body = b
		} else {
			respondError(w, http.StatusBadRequest, "invalid field: body")
			return
		}
	}

	if tagVal, ok := payload["tag"]; ok {
		if tagVal == nil {
			note.Tag = nil
		} else if t, ok := tagVal.(string); ok {
			note.Tag = &t
		} else {
			respondError(w, http.StatusBadRequest, "invalid field: tag")
			return
		}
	}

	// Update database
	mu.Lock()
	_, err = db.Exec("UPDATE notes SET title = ?, body = ?, tag = ? WHERE id = ?",
		note.Title, note.Body, note.Tag, id)
	mu.Unlock()

	if err != nil {
		respondError(w, http.StatusBadRequest, "invalid field: title")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(note)
}

func handleDeleteNote(w http.ResponseWriter, r *http.Request, id int64) {
	mu.Lock()
	result, err := db.Exec("DELETE FROM notes WHERE id = ?", id)
	mu.Unlock()

	if err != nil {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	rowsAffected, err := result.RowsAffected()
	if err != nil || rowsAffected == 0 {
		respondError(w, http.StatusNotFound, "not found")
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func respondError(w http.ResponseWriter, statusCode int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	json.NewEncoder(w).Encode(ErrorResponse{Error: message})
}
