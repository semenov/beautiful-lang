package main

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

var db *sql.DB
var sessionTTL int

type ErrorResponse struct {
	Error string `json:"error"`
}

type SuccessResponse struct {
	Username string `json:"username,omitempty"`
	Token    string `json:"token,omitempty"`
}

func main() {
	port := os.Getenv("PORT")
	dbPath := os.Getenv("DB_PATH")
	ttlStr := os.Getenv("SESSION_TTL_SECONDS")

	if port == "" || dbPath == "" {
		fmt.Fprintf(os.Stderr, "PORT and DB_PATH must be set\n")
		os.Exit(1)
	}

	sessionTTL = 3600
	if ttlStr != "" {
		ttl, err := strconv.Atoi(ttlStr)
		if err != nil || ttl < 1 {
			fmt.Fprintf(os.Stderr, "Invalid SESSION_TTL_SECONDS\n")
			os.Exit(1)
		}
		sessionTTL = ttl
	}

	var err error
	db, err = sql.Open("sqlite3", dbPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to open database: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	// Set connection pool to handle concurrent requests
	db.SetMaxOpenConns(25)
	db.SetMaxIdleConns(5)

	if err := initDB(); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to initialize database: %v\n", err)
		os.Exit(1)
	}

	http.HandleFunc("/register", handleRegister)
	http.HandleFunc("/login", handleLogin)
	http.HandleFunc("/me", handleMe)
	http.HandleFunc("/logout", handleLogout)
	http.HandleFunc("/", handleNotFound)

	addr := fmt.Sprintf("127.0.0.1:%s", port)
	fmt.Printf("Listening on %s\n", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to start server: %v\n", err)
		os.Exit(1)
	}
}

func initDB() error {
	schema := `
	CREATE TABLE IF NOT EXISTS users (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		username TEXT NOT NULL UNIQUE COLLATE NOCASE,
		username_display TEXT NOT NULL,
		password_hash TEXT NOT NULL
	);

	CREATE TABLE IF NOT EXISTS sessions (
		token TEXT PRIMARY KEY,
		user_id INTEGER NOT NULL,
		created_at INTEGER NOT NULL,
		expires_at INTEGER NOT NULL,
		FOREIGN KEY(user_id) REFERENCES users(id)
	);

	CREATE INDEX IF NOT EXISTS idx_sessions_expires ON sessions(expires_at);
	`

	_, err := db.Exec(schema)
	return err
}

func handleRegister(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		sendError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}

	if err := parseJSON(r, &req); err != nil {
		sendError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	// Validate username
	if !isValidUsername(req.Username) {
		sendError(w, http.StatusBadRequest, "invalid username")
		return
	}

	// Validate password
	if !isValidPassword(req.Password) {
		sendError(w, http.StatusBadRequest, "invalid password")
		return
	}

	// Hash password
	hash, err := hashPassword(req.Password)
	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	// Insert user
	result, err := db.Exec(
		"INSERT INTO users (username, username_display, password_hash) VALUES (?, ?, ?)",
		strings.ToLower(req.Username),
		req.Username,
		hash,
	)

	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE constraint failed") {
			sendError(w, http.StatusConflict, "username taken")
		} else {
			sendError(w, http.StatusInternalServerError, "internal error")
		}
		return
	}

	// Get the inserted user's display username
	userID, err := result.LastInsertId()
	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	var displayUsername string
	err = db.QueryRow(
		"SELECT username_display FROM users WHERE id = ?",
		userID,
	).Scan(&displayUsername)

	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]string{"username": displayUsername})
}

func handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		sendError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}

	if err := parseJSON(r, &req); err != nil {
		sendError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	if req.Username == "" || !isString(req.Username) {
		sendError(w, http.StatusBadRequest, "invalid username")
		return
	}

	if req.Password == "" || !isString(req.Password) {
		sendError(w, http.StatusBadRequest, "invalid password")
		return
	}

	// Find user
	var userID int
	var displayUsername string
	var hash string

	err := db.QueryRow(
		"SELECT id, username_display, password_hash FROM users WHERE username = ?",
		strings.ToLower(req.Username),
	).Scan(&userID, &displayUsername, &hash)

	if err == sql.ErrNoRows {
		sendError(w, http.StatusUnauthorized, "invalid credentials")
		return
	}

	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	// Verify password
	if !verifyPassword(req.Password, hash) {
		sendError(w, http.StatusUnauthorized, "invalid credentials")
		return
	}

	// Create session token
	token := generateToken()
	now := time.Now().Unix()
	expiresAt := now + int64(sessionTTL)

	_, err = db.Exec(
		"INSERT INTO sessions (token, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)",
		token,
		userID,
		now,
		expiresAt,
	)

	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]string{"token": token})
}

func handleMe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		sendError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	token := extractBearerToken(r)
	if token == "" {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	// Find session
	var userID int
	var expiresAt int64

	err := db.QueryRow(
		"SELECT user_id, expires_at FROM sessions WHERE token = ?",
		token,
	).Scan(&userID, &expiresAt)

	if err == sql.ErrNoRows {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	// Check if session expired
	if expiresAt < time.Now().Unix() {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	// Get username
	var username string
	err = db.QueryRow(
		"SELECT username_display FROM users WHERE id = ?",
		userID,
	).Scan(&username)

	if err != nil {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]string{"username": username})
}

func handleLogout(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		sendError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	token := extractBearerToken(r)
	if token == "" {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	// Delete session
	result, err := db.Exec("DELETE FROM sessions WHERE token = ?", token)
	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	rows, err := result.RowsAffected()
	if err != nil {
		sendError(w, http.StatusInternalServerError, "internal error")
		return
	}

	if rows == 0 {
		sendError(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func handleNotFound(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/" {
		sendError(w, http.StatusNotFound, "not found")
		return
	}

	http.NotFound(w, r)
	sendError(w, http.StatusNotFound, "not found")
}

func isValidUsername(username string) bool {
	if len(username) < 3 || len(username) > 32 {
		return false
	}

	for _, r := range username {
		if !isASCIILetterDigitUnderscore(r) {
			return false
		}
	}

	return true
}

func isValidPassword(password string) bool {
	if len(password) < 8 || len(password) > 128 {
		return false
	}
	return true
}

func isASCIILetterDigitUnderscore(r rune) bool {
	return (r >= 'a' && r <= 'z') ||
		(r >= 'A' && r <= 'Z') ||
		(r >= '0' && r <= '9') ||
		r == '_'
}

func isString(v interface{}) bool {
	_, ok := v.(string)
	return ok
}

func generateToken() string {
	// Generate at least 128 random bits (16 bytes)
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return base64.RawURLEncoding.EncodeToString(b)
}

func extractBearerToken(r *http.Request) string {
	auth := r.Header.Get("Authorization")
	if auth == "" {
		return ""
	}

	parts := strings.Split(auth, " ")
	if len(parts) != 2 || parts[0] != "Bearer" {
		return ""
	}

	return parts[1]
}

func parseJSON(r *http.Request, v interface{}) error {
	// Read body
	body, err := io.ReadAll(r.Body)
	if err != nil {
		return err
	}
	defer r.Body.Close()

	// Try to parse as JSON
	if err := json.Unmarshal(body, v); err != nil {
		return err
	}

	// Validate that it was a JSON object (not array, string, etc.)
	var temp interface{}
	if err := json.Unmarshal(body, &temp); err != nil {
		return err
	}

	// Check if it's an object
	_, ok := temp.(map[string]interface{})
	if !ok {
		return fmt.Errorf("not a JSON object")
	}

	return nil
}

func hashPassword(password string) (string, error) {
	// Generate a random salt
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}

	// Use PBKDF2 with SHA256
	hashBytes := pbkdf2.Key([]byte(password), salt, 10000, 32, sha256.New)

	// Combine salt and hash for storage: salt + hash (both hex encoded)
	saltHex := hex.EncodeToString(salt)
	hashHex := hex.EncodeToString(hashBytes)

	return saltHex + ":" + hashHex, nil
}

func verifyPassword(password string, storedHash string) bool {
	parts := strings.Split(storedHash, ":")
	if len(parts) != 2 {
		return false
	}

	saltHex := parts[0]
	hashHex := parts[1]

	salt, err := hex.DecodeString(saltHex)
	if err != nil {
		return false
	}

	expectedHash, err := hex.DecodeString(hashHex)
	if err != nil {
		return false
	}

	// Recompute hash
	computedHash := pbkdf2.Key([]byte(password), salt, 10000, 32, sha256.New)

	// Compare
	if len(computedHash) != len(expectedHash) {
		return false
	}

	for i := 0; i < len(computedHash); i++ {
		if computedHash[i] != expectedHash[i] {
			return false
		}
	}

	return true
}

func sendError(w http.ResponseWriter, status int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(ErrorResponse{Error: message})
}
