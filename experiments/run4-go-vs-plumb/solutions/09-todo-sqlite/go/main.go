package main

import (
	"bufio"
	"database/sql"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

const usageText = `usage: app add TITLE [--due YYYY-MM-DD] [--tag TAG]...
       app list [--tag TAG] [--overdue --today YYYY-MM-DD]
       app done ID
       app rm ID
       app search TEXT`

type usageError struct{ msg string }

func (e usageError) Error() string { return e.msg }

type notFoundError struct{ id string }

func (e notFoundError) Error() string { return "no such todo: " + e.id }

func usagef(format string, a ...any) error {
	return usageError{fmt.Sprintf(format, a...)}
}

// parsed holds the result of splitting a command's arguments into
// positionals and options.
type parsed struct {
	pos   []string
	opts  map[string][]string
	flags map[string]bool
}

// parseArgs separates positionals from options. valueOpts take a value;
// flagOpts do not. Options may appear anywhere; "--" ends option parsing.
func parseArgs(args []string, valueOpts, flagOpts []string) (*parsed, error) {
	p := &parsed{opts: map[string][]string{}, flags: map[string]bool{}}
	isValue := map[string]bool{}
	for _, o := range valueOpts {
		isValue[o] = true
	}
	isFlag := map[string]bool{}
	for _, o := range flagOpts {
		isFlag[o] = true
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			p.pos = append(p.pos, args[i+1:]...)
			break
		}
		if !strings.HasPrefix(a, "-") || a == "-" {
			p.pos = append(p.pos, a)
			continue
		}
		name, val, hasVal := a, "", false
		if strings.HasPrefix(a, "--") {
			if eq := strings.IndexByte(a, '='); eq >= 0 {
				name, val, hasVal = a[:eq], a[eq+1:], true
			}
		}
		switch {
		case isValue[name]:
			if !hasVal {
				if i+1 >= len(args) {
					return nil, usagef("usage: option %s needs a value", name)
				}
				i++
				val = args[i]
			}
			p.opts[name] = append(p.opts[name], val)
		case isFlag[name] && !hasVal:
			if p.flags[name] {
				return nil, usagef("usage: option %s given twice", name)
			}
			p.flags[name] = true
		default:
			return nil, usagef("usage: unknown option %s", a)
		}
	}
	return p, nil
}

func validDate(s string) bool {
	if len(s) != 10 {
		return false
	}
	for i, c := range []byte(s) {
		if i == 4 || i == 7 {
			if c != '-' {
				return false
			}
		} else if c < '0' || c > '9' {
			return false
		}
	}
	_, err := time.Parse("2006-01-02", s)
	return err == nil
}

func validTag(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range []byte(s) {
		switch {
		case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9', c == '_', c == '-':
		default:
			return false
		}
	}
	return true
}

// parseID checks that s is a whole number >= 1. It returns the canonical
// decimal form (no leading zeros), which may exceed int64.
func parseID(s string) (string, error) {
	if s == "" {
		return "", usagef("usage: invalid ID %q", s)
	}
	for _, c := range []byte(s) {
		if c < '0' || c > '9' {
			return "", usagef("usage: invalid ID %q", s)
		}
	}
	t := strings.TrimLeft(s, "0")
	if t == "" {
		return "", usagef("usage: invalid ID %q", s)
	}
	return t, nil
}

func (p *parsed) single(name string) (string, bool, error) {
	v := p.opts[name]
	switch len(v) {
	case 0:
		return "", false, nil
	case 1:
		return v[0], true, nil
	}
	return "", false, usagef("usage: option %s given twice", name)
}

type todo struct {
	id    int64
	title string
	done  bool
	due   sql.NullString
	tags  []string
}

func openDB(path string) (*sql.DB, error) {
	db, err := sql.Open("sqlite3", "file:"+escapePath(path)+"?_busy_timeout=10000&_foreign_keys=on")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	_, err = db.Exec(`
CREATE TABLE IF NOT EXISTS todos (
	id    INTEGER PRIMARY KEY AUTOINCREMENT,
	title TEXT NOT NULL,
	done  INTEGER NOT NULL DEFAULT 0,
	due   TEXT
);
CREATE TABLE IF NOT EXISTS tags (
	todo_id INTEGER NOT NULL REFERENCES todos(id) ON DELETE CASCADE,
	tag     TEXT NOT NULL,
	PRIMARY KEY (todo_id, tag)
);`)
	if err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

// escapePath makes a filesystem path safe inside a sqlite file: URI.
func escapePath(p string) string {
	var b strings.Builder
	for _, c := range []byte(p) {
		switch c {
		case '?', '#', '%':
			fmt.Fprintf(&b, "%%%02X", c)
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	err := execute(args)
	if err == nil {
		return 0
	}
	fmt.Fprintln(os.Stderr, errMessage(err))
	switch err.(type) {
	case usageError:
		return 64
	case notFoundError:
		return 3
	case envError:
		return 2
	}
	return 1
}

type envError struct{}

func (envError) Error() string { return "TODO_DB is not set" }

func errMessage(err error) string {
	switch e := err.(type) {
	case usageError:
		if strings.HasPrefix(e.msg, "usage:") {
			return e.msg + "\n" + usageText
		}
		return "usage: " + e.msg
	case envError, notFoundError:
		return "error: " + err.Error()
	}
	return "error: " + err.Error()
}

func execute(args []string) error {
	if len(args) == 0 {
		return usagef("usage: missing command")
	}
	var run func(db *sql.DB, out *bufio.Writer) error
	var err error
	switch args[0] {
	case "add":
		run, err = prepAdd(args[1:])
	case "list":
		run, err = prepList(args[1:])
	case "done":
		run, err = prepDoneRm(args[1:], true)
	case "rm":
		run, err = prepDoneRm(args[1:], false)
	case "search":
		run, err = prepSearch(args[1:])
	default:
		return usagef("usage: unknown command %q", args[0])
	}
	if err != nil {
		return err
	}
	path := os.Getenv("TODO_DB")
	if path == "" {
		return envError{}
	}
	db, err := openDB(path)
	if err != nil {
		return err
	}
	defer db.Close()
	// Buffer stdout so that nothing is printed if the command fails.
	var sb strings.Builder
	out := bufio.NewWriter(&sb)
	if err := run(db, out); err != nil {
		return err
	}
	out.Flush()
	_, werr := os.Stdout.WriteString(sb.String())
	return werr
}

func prepAdd(args []string) (func(*sql.DB, *bufio.Writer) error, error) {
	p, err := parseArgs(args, []string{"--due", "--tag"}, nil)
	if err != nil {
		return nil, err
	}
	if len(p.pos) != 1 {
		return nil, usagef("usage: add needs exactly one TITLE")
	}
	title := p.pos[0]
	if title == "" || strings.ContainsAny(title, "\r\n") {
		return nil, usagef("usage: TITLE must be non-empty and contain no newline")
	}
	due, hasDue, err := p.single("--due")
	if err != nil {
		return nil, err
	}
	if hasDue && !validDate(due) {
		return nil, usagef("usage: invalid date %q (want YYYY-MM-DD)", due)
	}
	seen := map[string]bool{}
	var tags []string
	for _, t := range p.opts["--tag"] {
		if !validTag(t) {
			return nil, usagef("usage: invalid tag %q", t)
		}
		if !seen[t] {
			seen[t] = true
			tags = append(tags, t)
		}
	}
	return func(db *sql.DB, out *bufio.Writer) error {
		tx, err := db.Begin()
		if err != nil {
			return err
		}
		defer tx.Rollback()
		var dueArg any
		if hasDue {
			dueArg = due
		}
		res, err := tx.Exec(`INSERT INTO todos (title, due) VALUES (?, ?)`, title, dueArg)
		if err != nil {
			return err
		}
		id, err := res.LastInsertId()
		if err != nil {
			return err
		}
		for _, t := range tags {
			if _, err := tx.Exec(`INSERT INTO tags (todo_id, tag) VALUES (?, ?)`, id, t); err != nil {
				return err
			}
		}
		if err := tx.Commit(); err != nil {
			return err
		}
		fmt.Fprintf(out, "added %d\n", id)
		return nil
	}, nil
}

func prepList(args []string) (func(*sql.DB, *bufio.Writer) error, error) {
	p, err := parseArgs(args, []string{"--tag", "--today"}, []string{"--overdue"})
	if err != nil {
		return nil, err
	}
	if len(p.pos) != 0 {
		return nil, usagef("usage: list takes no arguments")
	}
	tag, hasTag, err := p.single("--tag")
	if err != nil {
		return nil, err
	}
	if hasTag && !validTag(tag) {
		return nil, usagef("usage: invalid tag %q", tag)
	}
	today, hasToday, err := p.single("--today")
	if err != nil {
		return nil, err
	}
	overdue := p.flags["--overdue"]
	if overdue != hasToday {
		return nil, usagef("usage: --overdue and --today must be given together")
	}
	if hasToday && !validDate(today) {
		return nil, usagef("usage: invalid date %q (want YYYY-MM-DD)", today)
	}
	return func(db *sql.DB, out *bufio.Writer) error {
		q := `SELECT id, title, done, due FROM todos WHERE 1=1`
		var qa []any
		if hasTag {
			q += ` AND id IN (SELECT todo_id FROM tags WHERE tag = ?)`
			qa = append(qa, tag)
		}
		if overdue {
			q += ` AND done = 0 AND due IS NOT NULL AND due < ?`
			qa = append(qa, today)
		}
		return printTodos(db, out, q, qa...)
	}, nil
}

func prepSearch(args []string) (func(*sql.DB, *bufio.Writer) error, error) {
	p, err := parseArgs(args, nil, nil)
	if err != nil {
		return nil, err
	}
	if len(p.pos) != 1 || p.pos[0] == "" {
		return nil, usagef("usage: search needs one non-empty TEXT")
	}
	text := p.pos[0]
	return func(db *sql.DB, out *bufio.Writer) error {
		return printTodos(db, out, `SELECT id, title, done, due FROM todos WHERE instr(title, ?) > 0`, text)
	}, nil
}

func printTodos(db *sql.DB, out *bufio.Writer, where string, args ...any) error {
	rows, err := db.Query(where+` ORDER BY id`, args...)
	if err != nil {
		return err
	}
	var todos []*todo
	for rows.Next() {
		t := &todo{}
		var done int
		if err := rows.Scan(&t.id, &t.title, &done, &t.due); err != nil {
			rows.Close()
			return err
		}
		t.done = done != 0
		todos = append(todos, t)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	rows.Close()
	for _, t := range todos {
		trows, err := db.Query(`SELECT tag FROM tags WHERE todo_id = ?`, t.id)
		if err != nil {
			return err
		}
		for trows.Next() {
			var s string
			if err := trows.Scan(&s); err != nil {
				trows.Close()
				return err
			}
			t.tags = append(t.tags, s)
		}
		trows.Close()
		sort.Strings(t.tags)
		mark := " "
		if t.done {
			mark = "x"
		}
		fmt.Fprintf(out, "%d [%s] %s", t.id, mark, t.title)
		if t.due.Valid {
			fmt.Fprintf(out, " (due %s)", t.due.String)
		}
		for _, tg := range t.tags {
			fmt.Fprintf(out, " #%s", tg)
		}
		out.WriteByte('\n')
	}
	return nil
}

func prepDoneRm(args []string, isDone bool) (func(*sql.DB, *bufio.Writer) error, error) {
	p, err := parseArgs(args, nil, nil)
	if err != nil {
		return nil, err
	}
	if len(p.pos) != 1 {
		return nil, usagef("usage: expected exactly one ID")
	}
	idStr, err := parseID(p.pos[0])
	if err != nil {
		return nil, err
	}
	id, perr := strconv.ParseInt(idStr, 10, 64)
	return func(db *sql.DB, out *bufio.Writer) error {
		if perr != nil { // beyond int64: cannot exist
			return notFoundError{idStr}
		}
		var res sql.Result
		var err error
		verb := "removed"
		if isDone {
			verb = "done"
			res, err = db.Exec(`UPDATE todos SET done = 1 WHERE id = ?`, id)
		} else {
			res, err = db.Exec(`DELETE FROM todos WHERE id = ?`, id)
		}
		if err != nil {
			return err
		}
		n, err := res.RowsAffected()
		if err != nil {
			return err
		}
		if n == 0 {
			return notFoundError{idStr}
		}
		fmt.Fprintf(out, "%s %d\n", verb, id)
		return nil
	}, nil
}
