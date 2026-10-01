package main

import (
	"database/sql"
	"fmt"
	"os"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

func main() {
	todoDB := os.Getenv("TODO_DB")
	if todoDB == "" {
		fmt.Fprintf(os.Stderr, "error: TODO_DB is not set\n")
		os.Exit(2)
	}

	db, err := sql.Open("sqlite3", todoDB)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	if err := initDB(db); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	args := os.Args[1:]
	if len(args) == 0 {
		usageError("no command specified")
	}

	command := args[0]

	switch command {
	case "add":
		handleAdd(db, args[1:])
	case "list":
		handleList(db, args[1:])
	case "done":
		handleDone(db, args[1:])
	case "rm":
		handleRm(db, args[1:])
	case "search":
		handleSearch(db, args[1:])
	default:
		usageError("unknown command: " + command)
	}
}

func initDB(db *sql.DB) error {
	schema := `
	CREATE TABLE IF NOT EXISTS todos (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		title TEXT NOT NULL,
		done INTEGER NOT NULL DEFAULT 0,
		due_date TEXT
	);
	CREATE TABLE IF NOT EXISTS tags (
		todo_id INTEGER NOT NULL,
		tag TEXT NOT NULL,
		PRIMARY KEY (todo_id, tag),
		FOREIGN KEY (todo_id) REFERENCES todos(id) ON DELETE CASCADE
	);
	`
	_, err := db.Exec(schema)
	return err
}

func handleAdd(db *sql.DB, args []string) {
	var title string
	var dueDate *string
	tags := make(map[string]bool)
	var i int

	for i = 0; i < len(args); i++ {
		arg := args[i]
		if arg == "--due" {
			if i+1 >= len(args) {
				usageError("--due requires an argument")
			}
			i++
			dueDate = &args[i]
		} else if arg == "--tag" {
			if i+1 >= len(args) {
				usageError("--tag requires an argument")
			}
			i++
			tag := args[i]
			if !isValidTag(tag) {
				usageError("invalid tag: " + tag)
			}
			tags[tag] = true
		} else if strings.HasPrefix(arg, "--") {
			usageError("unknown option: " + arg)
		} else {
			if title != "" {
				usageError("multiple titles provided")
			}
			title = arg
		}
	}

	if title == "" {
		usageError("title is required")
	}

	if strings.Contains(title, "\n") || strings.Contains(title, "\r") {
		usageError("title cannot contain newlines")
	}

	if dueDate != nil {
		if !isValidDate(*dueDate) {
			usageError("invalid date: " + *dueDate)
		}
	}

	var id int64
	err := db.QueryRow("SELECT MAX(id) FROM todos").Scan(&id)
	if err != nil && err != sql.ErrNoRows {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	id++

	_, err = db.Exec(
		"INSERT INTO todos (id, title, due_date) VALUES (?, ?, ?)",
		id, title, dueDate,
	)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	// Insert tags
	for tag := range tags {
		_, err := db.Exec(
			"INSERT INTO tags (todo_id, tag) VALUES (?, ?)",
			id, tag,
		)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}
	}

	fmt.Printf("added %d\n", id)
}

func handleList(db *sql.DB, args []string) {
	var filterTag *string
	var overdue bool
	var todayDate *string

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if arg == "--tag" {
			if i+1 >= len(args) {
				usageError("--tag requires an argument")
			}
			i++
			filterTag = &args[i]
		} else if arg == "--overdue" {
			overdue = true
		} else if arg == "--today" {
			if i+1 >= len(args) {
				usageError("--today requires an argument")
			}
			i++
			todayDate = &args[i]
		} else {
			usageError("unknown option: " + arg)
		}
	}

	if (overdue && todayDate == nil) || (!overdue && todayDate != nil) {
		usageError("--overdue and --today must be given together")
	}

	if todayDate != nil && !isValidDate(*todayDate) {
		usageError("invalid date: " + *todayDate)
	}

	rows, err := db.Query("SELECT id, title, done, due_date FROM todos ORDER BY id ASC")
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	defer rows.Close()

	for rows.Next() {
		var id int64
		var title string
		var done int
		var dueDate *string

		if err := rows.Scan(&id, &title, &done, &dueDate); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}

		// Check if should be filtered by overdue
		if overdue && done == 0 && dueDate != nil && *dueDate < *todayDate {
			// Include this one
		} else if overdue && (done != 0 || dueDate == nil || *dueDate >= *todayDate) {
			continue
		}

		// Check if should be filtered by tag
		if filterTag != nil {
			hasTags, err := hasTodo(db, id, *filterTag)
			if err != nil {
				fmt.Fprintf(os.Stderr, "error: %v\n", err)
				os.Exit(1)
			}
			if !hasTags {
				continue
			}
		}

		// Get tags for this todo
		tags, err := getTags(db, id)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}

		status := "[ ]"
		if done != 0 {
			status = "[x]"
		}

		output := fmt.Sprintf("%d %s %s", id, status, title)
		if dueDate != nil && *dueDate != "" {
			output += fmt.Sprintf(" (due %s)", *dueDate)
		}
		for _, tag := range tags {
			output += fmt.Sprintf(" #%s", tag)
		}
		fmt.Println(output)
	}

	if err := rows.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func handleDone(db *sql.DB, args []string) {
	var id string

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if strings.HasPrefix(arg, "--") {
			usageError("unknown option: " + arg)
		} else {
			if id != "" {
				usageError("multiple IDs provided")
			}
			id = arg
		}
	}

	if id == "" {
		usageError("ID is required")
	}

	var parsedID int64
	_, err := fmt.Sscanf(id, "%d", &parsedID)
	if err != nil || parsedID < 1 {
		usageError("invalid ID: " + id)
	}

	var exists bool
	err = db.QueryRow("SELECT 1 FROM todos WHERE id = ?", parsedID).Scan(&exists)
	if err == sql.ErrNoRows {
		fmt.Fprintf(os.Stderr, "error: no such todo: %d\n", parsedID)
		os.Exit(3)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	_, err = db.Exec("UPDATE todos SET done = 1 WHERE id = ?", parsedID)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("done %d\n", parsedID)
}

func handleRm(db *sql.DB, args []string) {
	var id string

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if strings.HasPrefix(arg, "--") {
			usageError("unknown option: " + arg)
		} else {
			if id != "" {
				usageError("multiple IDs provided")
			}
			id = arg
		}
	}

	if id == "" {
		usageError("ID is required")
	}

	var parsedID int64
	_, err := fmt.Sscanf(id, "%d", &parsedID)
	if err != nil || parsedID < 1 {
		usageError("invalid ID: " + id)
	}

	var exists bool
	err = db.QueryRow("SELECT 1 FROM todos WHERE id = ?", parsedID).Scan(&exists)
	if err == sql.ErrNoRows {
		fmt.Fprintf(os.Stderr, "error: no such todo: %d\n", parsedID)
		os.Exit(3)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	_, err = db.Exec("DELETE FROM todos WHERE id = ?", parsedID)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("removed %d\n", parsedID)
}

func handleSearch(db *sql.DB, args []string) {
	var text string

	for i := 0; i < len(args); i++ {
		arg := args[i]
		if strings.HasPrefix(arg, "--") {
			usageError("unknown option: " + arg)
		} else {
			if text != "" {
				usageError("multiple search texts provided")
			}
			text = arg
		}
	}

	if text == "" {
		usageError("search text is required")
	}

	rows, err := db.Query(
		"SELECT id, title, done, due_date FROM todos WHERE title LIKE ? ORDER BY id ASC",
		"%"+text+"%",
	)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	defer rows.Close()

	for rows.Next() {
		var id int64
		var title string
		var done int
		var dueDate *string

		if err := rows.Scan(&id, &title, &done, &dueDate); err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}

		// Get tags for this todo
		tags, err := getTags(db, id)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: %v\n", err)
			os.Exit(1)
		}

		status := "[ ]"
		if done != 0 {
			status = "[x]"
		}

		output := fmt.Sprintf("%d %s %s", id, status, title)
		if dueDate != nil && *dueDate != "" {
			output += fmt.Sprintf(" (due %s)", *dueDate)
		}
		for _, tag := range tags {
			output += fmt.Sprintf(" #%s", tag)
		}
		fmt.Println(output)
	}

	if err := rows.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func isValidTag(tag string) bool {
	if tag == "" {
		return false
	}
	for _, r := range tag {
		if !((r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' || r == '-') {
			return false
		}
	}
	return true
}

func isValidDate(dateStr string) bool {
	t, err := time.Parse("2006-01-02", dateStr)
	if err != nil {
		return false
	}
	// Verify it's a real date by parsing it back
	formatted := t.Format("2006-01-02")
	return formatted == dateStr
}

func getTags(db *sql.DB, todoID int64) ([]string, error) {
	rows, err := db.Query("SELECT tag FROM tags WHERE todo_id = ? ORDER BY tag ASC", todoID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var tags []string
	for rows.Next() {
		var tag string
		if err := rows.Scan(&tag); err != nil {
			return nil, err
		}
		tags = append(tags, tag)
	}

	return tags, rows.Err()
}

func hasTodo(db *sql.DB, todoID int64, tag string) (bool, error) {
	var exists bool
	err := db.QueryRow("SELECT 1 FROM tags WHERE todo_id = ? AND tag = ?", todoID, tag).Scan(&exists)
	if err == sql.ErrNoRows {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return true, nil
}

func usageError(msg string) {
	fmt.Fprintf(os.Stderr, "usage: %s\n", msg)
	os.Exit(64)
}
