package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"sync"
)

type FileInfo struct {
	Name   string `json:"name"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type ListResponse struct {
	Files []FileInfo `json:"files"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

var (
	port      string
	storeDir  string
	maxBytes  int64
	mu        sync.RWMutex // Protects concurrent access to files
)

func init() {
	port = os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	storeDir = os.Getenv("STORE_DIR")
	if storeDir == "" {
		storeDir = "/tmp/file-store"
	}

	maxBytesStr := os.Getenv("MAX_BYTES")
	if maxBytesStr == "" {
		maxBytes = 1048576
	} else {
		var err error
		maxBytes, err = strconv.ParseInt(maxBytesStr, 10, 64)
		if err != nil {
			log.Fatalf("Invalid MAX_BYTES: %v", err)
		}
	}

	// Create store directory if it doesn't exist
	if err := os.MkdirAll(storeDir, 0755); err != nil {
		log.Fatalf("Failed to create store directory: %v", err)
	}
}

func isValidName(name string) bool {
	// Name must be 1-100 characters
	if len(name) < 1 || len(name) > 100 {
		return false
	}

	// Must not start with .
	if name[0] == '.' {
		return false
	}

	// Only ASCII letters, digits, ., _, -
	// Use regex for simplicity
	pattern := regexp.MustCompile(`^[a-zA-Z0-9._-]+$`)
	if !pattern.MatchString(name) {
		return false
	}

	return true
}

func hashFile(data []byte) string {
	hash := sha256.Sum256(data)
	return hex.EncodeToString(hash[:])
}

func getFilePath(name string) string {
	return filepath.Join(storeDir, name)
}

func writeJSON(w http.ResponseWriter, statusCode int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(statusCode)
	json.NewEncoder(w).Encode(v)
}

func handleFilesEndpoint(w http.ResponseWriter, r *http.Request) {
	// GET /files - list all files
	if r.Method == http.MethodGet && r.URL.Path == "/files" {
		mu.RLock()
		defer mu.RUnlock()

		entries, err := os.ReadDir(storeDir)
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "Failed to read directory"})
			return
		}

		files := make([]FileInfo, 0)
		for _, entry := range entries {
			// Skip hidden files (starting with .)
			if entry.Name()[0] == '.' {
				continue
			}

			if entry.IsDir() {
				continue
			}

			filePath := getFilePath(entry.Name())
			data, err := os.ReadFile(filePath)
			if err != nil {
				continue // Skip files we can't read
			}

			files = append(files, FileInfo{
				Name:   entry.Name(),
				Size:   int64(len(data)),
				SHA256: hashFile(data),
			})
		}

		// Sort by name (ascending by byte value)
		sort.Slice(files, func(i, j int) bool {
			return files[i].Name < files[j].Name
		})

		writeJSON(w, http.StatusOK, ListResponse{Files: files})
		return
	}

	// DELETE /files is not allowed
	if r.Method == http.MethodDelete && r.URL.Path == "/files" {
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
		return
	}

	// Other methods on /files are not allowed
	writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
}

func handleFileEndpoint(w http.ResponseWriter, r *http.Request) {
	// Extract name from URL path (everything after /files/)
	name := r.URL.Path[7:] // Skip "/files/"

	// URL decode the name
	decodedName, err := url.QueryUnescape(name)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, ErrorResponse{Error: "invalid name"})
		return
	}

	// Validate name
	if !isValidName(decodedName) {
		writeJSON(w, http.StatusBadRequest, ErrorResponse{Error: "invalid name"})
		return
	}

	filePath := getFilePath(decodedName)

	switch r.Method {
	case http.MethodPut:
		handlePut(w, r, filePath, decodedName)
	case http.MethodGet:
		handleGet(w, r, filePath)
	case http.MethodDelete:
		handleDelete(w, r, filePath)
	default:
		writeJSON(w, http.StatusMethodNotAllowed, ErrorResponse{Error: "method not allowed"})
	}
}

func handlePut(w http.ResponseWriter, r *http.Request, filePath, name string) {
	// Read request body (limited by MAX_BYTES)
	if r.ContentLength > maxBytes {
		writeJSON(w, http.StatusRequestEntityTooLarge, ErrorResponse{Error: "too large"})
		return
	}

	data := make([]byte, 0, r.ContentLength)
	buf := make([]byte, 32*1024)
	totalSize := int64(0)

	for {
		n, err := r.Body.Read(buf)
		if n > 0 {
			if totalSize+int64(n) > maxBytes {
				writeJSON(w, http.StatusRequestEntityTooLarge, ErrorResponse{Error: "too large"})
				return
			}
			data = append(data, buf[:n]...)
			totalSize += int64(n)
		}
		if err != nil {
			break
		}
	}

	mu.Lock()
	defer mu.Unlock()

	// Check if file exists
	_, err := os.Stat(filePath)
	isNew := os.IsNotExist(err)

	// Write to temporary file first
	tempPath := filepath.Join(storeDir, ".tmp_"+name)
	err = os.WriteFile(tempPath, data, 0644)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "Failed to write file"})
		return
	}

	// Atomic rename
	err = os.Rename(tempPath, filePath)
	if err != nil {
		os.Remove(tempPath)
		writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "Failed to write file"})
		return
	}

	statusCode := http.StatusOK
	if isNew {
		statusCode = http.StatusCreated
	}

	response := FileInfo{
		Name:   name,
		Size:   int64(len(data)),
		SHA256: hashFile(data),
	}

	writeJSON(w, statusCode, response)
}

func handleGet(w http.ResponseWriter, r *http.Request, filePath string) {
	mu.RLock()
	defer mu.RUnlock()

	data, err := os.ReadFile(filePath)
	if err != nil {
		if os.IsNotExist(err) {
			writeJSON(w, http.StatusNotFound, ErrorResponse{Error: "not found"})
		} else {
			writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "Failed to read file"})
		}
		return
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	w.WriteHeader(http.StatusOK)
	w.Write(data)
}

func handleDelete(w http.ResponseWriter, r *http.Request, filePath string) {
	mu.Lock()
	defer mu.Unlock()

	// Check if file exists first
	_, err := os.Stat(filePath)
	if os.IsNotExist(err) {
		writeJSON(w, http.StatusNotFound, ErrorResponse{Error: "not found"})
		return
	}

	err = os.Remove(filePath)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, ErrorResponse{Error: "Failed to delete file"})
		return
	}

	w.WriteHeader(http.StatusNoContent)
}

func routeHandler(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path

	if path == "/files" {
		handleFilesEndpoint(w, r)
		return
	}

	if len(path) > 7 && path[:7] == "/files/" {
		handleFileEndpoint(w, r)
		return
	}

	writeJSON(w, http.StatusNotFound, ErrorResponse{Error: "not found"})
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", routeHandler)

	addr := "127.0.0.1:" + port
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", addr, err)
	}
	defer listener.Close()

	log.Printf("Listening on %s", addr)
	log.Fatal(http.Serve(listener, mux))
}
