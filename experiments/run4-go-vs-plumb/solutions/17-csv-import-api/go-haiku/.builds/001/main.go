package main

import (
	"bufio"
	"bytes"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"unicode/utf8"

	_ "github.com/mattn/go-sqlite3"
)

var (
	db *sql.DB
	mu sync.Mutex
)

type User struct {
	Email string `json:"email"`
	Name  string `json:"name"`
	Age   int    `json:"age"`
}

type ErrorDetail struct {
	Row     int    `json:"row"`
	Field   string `json:"field"`
	Message string `json:"message"`
}

type ImportResponse struct {
	Imported int            `json:"imported"`
	Errors   []ErrorDetail  `json:"errors,omitempty"`
}

type UsersResponse struct {
	Users []User `json:"users"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	dbPath := os.Getenv("DB_PATH")
	if dbPath == "" {
		dbPath = "./users.db"
	}

	var err error
	db, err = sql.Open("sqlite3", dbPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to open database: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	// Create users table if it doesn't exist
	createTableSQL := `
	CREATE TABLE IF NOT EXISTS users (
		email TEXT PRIMARY KEY,
		name TEXT NOT NULL,
		age INTEGER NOT NULL
	);
	`
	if _, err := db.Exec(createTableSQL); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to create table: %v\n", err)
		os.Exit(1)
	}

	http.HandleFunc("/import", handleImport)
	http.HandleFunc("/users", handleUsers)
	http.HandleFunc("/", handleNotFound)

	addr := net.JoinHostPort("127.0.0.1", port)
	fmt.Printf("Listening on %s\n", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Fprintf(os.Stderr, "Server error: %v\n", err)
		os.Exit(1)
	}
}

func handleImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}
	defer r.Body.Close()

	if len(body) == 0 {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	if !utf8.Valid(body) {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	records, err := parseCSV(string(body))
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	if len(records) == 0 {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	// Parse header
	header := records[0]
	if len(header) != 3 {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	emailIdx, nameIdx, ageIdx := -1, -1, -1
	for i, col := range header {
		switch col {
		case "email":
			emailIdx = i
		case "name":
			nameIdx = i
		case "age":
			ageIdx = i
		}
	}

	if emailIdx == -1 || nameIdx == -1 || ageIdx == -1 {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid csv"})
		return
	}

	// Validate data rows
	var errors []ErrorDetail
	seenEmails := make(map[string]int) // email -> row number (1-indexed)

	for rowNum := 1; rowNum < len(records); rowNum++ {
		row := records[rowNum]

		// Check field count
		if len(row) != 3 {
			errors = append(errors, ErrorDetail{
				Row:     rowNum,
				Field:   "row",
				Message: "wrong number of fields",
			})
			continue
		}

		email := row[emailIdx]
		name := row[nameIdx]
		age := row[ageIdx]

		// Validate email
		emailErr := validateEmail(email)
		if emailErr != "" {
			errors = append(errors, ErrorDetail{
				Row:     rowNum,
				Field:   "email",
				Message: emailErr,
			})
		} else {
			emailLower := strings.ToLower(email)
			// Check if email was seen in this file
			if prevRow, exists := seenEmails[emailLower]; exists {
				errors = append(errors, ErrorDetail{
					Row:     rowNum,
					Field:   "email",
					Message: "duplicate email in file",
				})
			} else {
				// Check if email already exists in database
				var exists bool
				err := db.QueryRow("SELECT EXISTS(SELECT 1 FROM users WHERE LOWER(email) = ?)", emailLower).Scan(&exists)
				if err != nil {
					w.WriteHeader(http.StatusInternalServerError)
					return
				}
				if exists {
					errors = append(errors, ErrorDetail{
						Row:     rowNum,
						Field:   "email",
						Message: "email already exists",
					})
				} else {
					seenEmails[emailLower] = rowNum
				}
			}
		}

		// Validate name
		nameErr := validateName(name)
		if nameErr != "" {
			errors = append(errors, ErrorDetail{
				Row:     rowNum,
				Field:   "name",
				Message: nameErr,
			})
		}

		// Validate age
		ageErr := validateAge(age)
		if ageErr != "" {
			errors = append(errors, ErrorDetail{
				Row:     rowNum,
				Field:   "age",
				Message: ageErr,
			})
		}
	}

	if len(errors) > 0 {
		w.WriteHeader(http.StatusUnprocessableEntity)
		json.NewEncoder(w).Encode(ImportResponse{
			Imported: 0,
			Errors:   errors,
		})
		return
	}

	// No errors, insert all rows in a transaction
	mu.Lock()
	defer mu.Unlock()

	tx, err := db.Begin()
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	insertedCount := 0
	for rowNum := 1; rowNum < len(records); rowNum++ {
		row := records[rowNum]
		email := strings.ToLower(row[emailIdx])
		name := row[nameIdx]
		age := parseAge(row[ageIdx])

		_, err := tx.Exec("INSERT INTO users (email, name, age) VALUES (?, ?, ?)", email, name, age)
		if err != nil {
			tx.Rollback()
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		insertedCount++
	}

	if err := tx.Commit(); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(ImportResponse{Imported: insertedCount})
}

func handleUsers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	rows, err := db.Query("SELECT email, name, age FROM users ORDER BY email ASC")
	if err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	var users []User
	for rows.Next() {
		var email, name string
		var age int
		if err := rows.Scan(&email, &name, &age); err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		users = append(users, User{Email: email, Name: name, Age: age})
	}

	if users == nil {
		users = []User{}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(UsersResponse{Users: users})
}

func handleNotFound(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusNotFound)
	json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
}

// parseCSV parses CSV content with proper handling of quoted fields
func parseCSV(content string) ([][]string, error) {
	var records [][]string
	reader := bufio.NewReader(strings.NewReader(content))

	for {
		record, err := readCSVRecord(reader)
		if err != nil {
			if err == io.EOF {
				break
			}
			return nil, err
		}
		if record != nil {
			records = append(records, record)
		}
	}

	return records, nil
}

// readCSVRecord reads one CSV record (handling quoted fields)
func readCSVRecord(reader *bufio.Reader) ([]string, error) {
	var record []string
	var field bytes.Buffer
	inQuotes := false

	for {
		r, size, err := reader.ReadRune()
		if err != nil && err != io.EOF {
			return nil, err
		}

		if err == io.EOF {
			if inQuotes {
				return nil, fmt.Errorf("unterminated quoted field")
			}
			// End of file
			if field.Len() > 0 || len(record) > 0 {
				record = append(record, field.String())
			}
			if len(record) == 0 && len(records) == 0 {
				// Empty file
				return nil, io.EOF
			}
			if len(record) > 0 {
				return record, nil
			}
			return nil, io.EOF
		}

		if inQuotes {
			if r == '"' {
				// Look ahead for next character
				nextRune, _, err := reader.ReadRune()
				if err != nil {
					if err == io.EOF {
						// End of file right after closing quote
						record = append(record, field.String())
						return record, nil
					}
					return nil, err
				}

				if nextRune == '"' {
					// Escaped quote
					field.WriteRune('"')
				} else {
					// End of quoted field
					inQuotes = false
					// Put back the nextRune
					reader.UnreadRune()
				}
			} else {
				field.WriteRune(r)
			}
		} else {
			if r == '"' {
				if field.Len() > 0 {
					// Quote in middle of unquoted field
					return nil, fmt.Errorf("quote in unquoted field")
				}
				inQuotes = true
			} else if r == ',' {
				record = append(record, field.String())
				field.Reset()
			} else if r == '\n' {
				record = append(record, field.String())
				return record, nil
			} else if r == '\r' {
				// Check if followed by \n
				nextRune, _, err := reader.ReadRune()
				if err != nil {
					if err == io.EOF {
						record = append(record, field.String())
						return record, nil
					}
					return nil, err
				}
				if nextRune != '\n' {
					reader.UnreadRune()
				}
				record = append(record, field.String())
				return record, nil
			} else {
				field.WriteRune(r)
			}
		}
	}
}

func validateEmail(email string) string {
	if len(email) > 254 {
		return "invalid email"
	}

	parts := strings.Split(email, "@")
	if len(parts) != 2 {
		return "invalid email"
	}

	localPart := parts[0]
	domainPart := parts[1]

	if localPart == "" {
		return "invalid email"
	}

	if domainPart == "" {
		return "invalid email"
	}

	// Check for spaces or whitespace
	if strings.ContainsAny(email, " \t\n\r") {
		return "invalid email"
	}

	// Domain must have a dot that is neither first nor last
	dotIdx := strings.Index(domainPart, ".")
	if dotIdx == -1 || dotIdx == 0 || dotIdx == len(domainPart)-1 {
		return "invalid email"
	}

	return ""
}

func validateName(name string) string {
	runeCount := utf8.RuneCountInString(name)
	if runeCount < 1 || runeCount > 100 {
		return "invalid name"
	}
	return ""
}

func validateAge(age string) string {
	// Only decimal digits
	for _, r := range age {
		if r < '0' || r > '9' {
			return "invalid age"
		}
	}

	if age == "" || age == "0" {
		return "invalid age"
	}

	// Parse as integer
	var ageInt int
	_, err := fmt.Sscanf(age, "%d", &ageInt)
	if err != nil {
		return "invalid age"
	}

	if ageInt < 13 || ageInt > 120 {
		return "invalid age"
	}

	return ""
}

func parseAge(age string) int {
	var ageInt int
	fmt.Sscanf(age, "%d", &ageInt)
	return ageInt
}
