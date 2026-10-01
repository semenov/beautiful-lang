package main

import (
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
)

// Value can be a string or an int64
type Value interface{}

// Store holds all key-value pairs
type Store struct {
	mu    sync.RWMutex
	items map[string]Value
}

var store = &Store{
	items: make(map[string]Value),
}

// Helper functions for JSON responses
func writeJSON(w http.ResponseWriter, statusCode int, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	json.NewEncoder(w).Encode(data)
}

func writeError(w http.ResponseWriter, statusCode int, message string) {
	writeJSON(w, statusCode, map[string]interface{}{"error": message})
}

func writeErrorWithValue(w http.ResponseWriter, statusCode int, message string, value Value) {
	writeJSON(w, statusCode, map[string]interface{}{"error": message, "value": value})
}

// parseKey extracts and URL-decodes the key from the path
func parseKey(path string, prefix string) (string, error) {
	if !strings.HasPrefix(path, prefix) {
		return "", fmt.Errorf("invalid path")
	}

	keyPart := strings.TrimPrefix(path, prefix)
	// Split off query string
	if idx := strings.Index(keyPart, "?"); idx >= 0 {
		keyPart = keyPart[:idx]
	}

	// URL decode
	key, err := url.QueryUnescape(keyPart)
	if err != nil {
		return "", err
	}

	// Must be non-empty
	if key == "" {
		return "", fmt.Errorf("empty key")
	}

	return key, nil
}

// isValidValue checks if a value is a valid JSON string or int64
func isValidValue(v interface{}) bool {
	switch v.(type) {
	case string:
		return true
	case float64:
		f := v.(float64)
		// Check if it's an integer in int64 range
		if f != math.Floor(f) {
			return false
		}
		if f < float64(math.MinInt64) || f > float64(math.MaxInt64) {
			return false
		}
		return true
	default:
		return false
	}
}

// toInt64 converts a value to int64 if it's a valid integer
func toInt64(v interface{}) (int64, error) {
	f, ok := v.(float64)
	if !ok {
		return 0, fmt.Errorf("not a number")
	}

	if f != math.Floor(f) {
		return 0, fmt.Errorf("not an integer")
	}

	if f < float64(math.MinInt64) || f > float64(math.MaxInt64) {
		return 0, fmt.Errorf("out of range")
	}

	return int64(f), nil
}

// valueToDisplay converts stored value to display format (int64 for integers)
func valueToDisplay(v interface{}) interface{} {
	switch val := v.(type) {
	case string:
		return val
	case float64:
		// Check if it's actually an integer
		if val == math.Floor(val) && val >= float64(math.MinInt64) && val <= float64(math.MaxInt64) {
			return int64(val)
		}
		return val
	default:
		return val
	}
}

// valuesEqual checks if two values are equal (handling int64 vs float64 comparison)
func valuesEqual(a, b interface{}) bool {
	// Handle nil cases
	if a == nil && b == nil {
		return true
	}
	if a == nil || b == nil {
		return false
	}

	// Convert both to their display values for comparison
	aDisplay := valueToDisplay(a)
	bDisplay := valueToDisplay(b)

	// Use JSON marshaling for deep equality
	aJSON, _ := json.Marshal(aDisplay)
	bJSON, _ := json.Marshal(bDisplay)
	return string(aJSON) == string(bJSON)
}

// handleGet handles GET /kv/<key>
func handleGet(w http.ResponseWriter, r *http.Request) {
	key, err := parseKey(r.URL.Path, "/kv/")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid key")
		return
	}

	store.mu.RLock()
	value, exists := store.items[key]
	store.mu.RUnlock()

	if !exists {
		writeError(w, http.StatusNotFound, "not found")
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"key":   key,
		"value": valueToDisplay(value),
	})
}

// handlePut handles PUT /kv/<key>
func handlePut(w http.ResponseWriter, r *http.Request) {
	key, err := parseKey(r.URL.Path, "/kv/")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid key")
		return
	}

	var body map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	value, exists := body["value"]
	if !exists {
		writeError(w, http.StatusBadRequest, "missing value")
		return
	}

	if !isValidValue(value) {
		writeError(w, http.StatusBadRequest, "invalid value")
		return
	}

	store.mu.Lock()
	store.items[key] = value
	store.mu.Unlock()

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"key":   key,
		"value": valueToDisplay(value),
	})
}

// handleDelete handles DELETE /kv/<key>
func handleDelete(w http.ResponseWriter, r *http.Request) {
	key, err := parseKey(r.URL.Path, "/kv/")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid key")
		return
	}

	store.mu.Lock()
	_, exists := store.items[key]
	if !exists {
		store.mu.Unlock()
		writeError(w, http.StatusNotFound, "not found")
		return
	}
	delete(store.items, key)
	store.mu.Unlock()

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusNoContent)
}

// handleIncr handles POST /incr/<key>?by=N
func handleIncr(w http.ResponseWriter, r *http.Request) {
	key, err := parseKey(r.URL.Path, "/incr/")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid key")
		return
	}

	// Parse 'by' parameter
	byStr := r.URL.Query().Get("by")
	by := int64(1) // default
	if byStr != "" {
		var err error
		by, err = strconv.ParseInt(byStr, 10, 64)
		if err != nil {
			writeError(w, http.StatusBadRequest, "invalid by parameter")
			return
		}
	}

	store.mu.Lock()
	defer store.mu.Unlock()

	current, exists := store.items[key]
	var currentInt int64

	if !exists {
		currentInt = 0
	} else {
		// Check if current value is an integer
		curF, ok := current.(float64)
		if !ok {
			// Current value is a string, not an integer
			writeErrorWithValue(w, http.StatusConflict, "value is not an integer", valueToDisplay(current))
			return
		}
		// Convert to int64
		if curF != math.Floor(curF) {
			writeErrorWithValue(w, http.StatusConflict, "value is not an integer", valueToDisplay(current))
			return
		}
		currentInt = int64(curF)
	}

	// Check for overflow/underflow
	if by > 0 && currentInt > math.MaxInt64-by {
		writeErrorWithValue(w, http.StatusConflict, "overflow", valueToDisplay(current))
		return
	}
	if by < 0 && currentInt < math.MinInt64-by {
		writeErrorWithValue(w, http.StatusConflict, "underflow", valueToDisplay(current))
		return
	}

	newValue := currentInt + by
	store.items[key] = float64(newValue)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"key":   key,
		"value": newValue,
	})
}

// handleCas handles POST /cas/<key> with {"expected": E, "value": V}
func handleCas(w http.ResponseWriter, r *http.Request) {
	key, err := parseKey(r.URL.Path, "/cas/")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid key")
		return
	}

	var body map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON")
		return
	}

	expectedVal, expectedExists := body["expected"]
	newVal, newExists := body["value"]

	if !newExists {
		writeError(w, http.StatusBadRequest, "missing value")
		return
	}

	// expected can be null (means key must not exist)
	if expectedExists && expectedVal != nil && !isValidValue(expectedVal) {
		writeError(w, http.StatusBadRequest, "invalid expected value")
		return
	}

	if !isValidValue(newVal) {
		writeError(w, http.StatusBadRequest, "invalid value")
		return
	}

	store.mu.Lock()
	defer store.mu.Unlock()

	current, exists := store.items[key]

	// Check if expected matches current
	if !expectedExists || expectedVal == nil {
		// expected is null, must not exist
		if exists {
			writeErrorWithValue(w, http.StatusConflict, "key exists", valueToDisplay(current))
			return
		}
	} else {
		// expected has a value
		if !exists {
			// Key doesn't exist but expected is not null
			writeErrorWithValue(w, http.StatusConflict, "key does not exist", nil)
			return
		}

		// Compare values
		if !valuesEqual(current, expectedVal) {
			writeErrorWithValue(w, http.StatusConflict, "compare failed", valueToDisplay(current))
			return
		}
	}

	// Update the value
	store.items[key] = newVal

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"key":   key,
		"value": valueToDisplay(newVal),
	})
}

// handleSnapshot handles GET /snapshot
func handleSnapshot(w http.ResponseWriter, r *http.Request) {
	store.mu.RLock()
	defer store.mu.RUnlock()

	items := make(map[string]interface{})
	for k, v := range store.items {
		items[k] = valueToDisplay(v)
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"count": len(store.items),
		"items": items,
	})
}

// Router
func handleRequest(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path

	// Snapshot endpoint
	if path == "/snapshot" {
		if r.Method == http.MethodGet {
			handleSnapshot(w, r)
		} else {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
		return
	}

	// /kv/ endpoints
	if strings.HasPrefix(path, "/kv/") {
		switch r.Method {
		case http.MethodGet:
			handleGet(w, r)
		case http.MethodPut:
			handlePut(w, r)
		case http.MethodDelete:
			handleDelete(w, r)
		default:
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
		return
	}

	// /incr/ endpoints
	if strings.HasPrefix(path, "/incr/") {
		if r.Method == http.MethodPost {
			handleIncr(w, r)
		} else {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
		return
	}

	// /cas/ endpoints
	if strings.HasPrefix(path, "/cas/") {
		if r.Method == http.MethodPost {
			handleCas(w, r)
		} else {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		}
		return
	}

	// Unknown path
	writeError(w, http.StatusNotFound, "not found")
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	http.HandleFunc("/", handleRequest)

	addr := net.JoinHostPort("127.0.0.1", port)
	log.Printf("Starting server on %s", addr)

	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("Server error: %v", err)
	}
}
