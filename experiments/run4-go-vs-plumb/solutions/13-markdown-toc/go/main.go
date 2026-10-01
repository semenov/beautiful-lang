package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"unicode"
)

type heading struct {
	level int
	text  string
}

func usage(msg string) {
	if msg != "" {
		fmt.Fprintln(os.Stderr, "usage: "+msg)
	} else {
		fmt.Fprintln(os.Stderr, "usage: app [--min-level N] [--max-level M] [FILE]")
	}
	os.Exit(64)
}

func parseLevel(s string) int {
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > 6 {
		usage("level must be an integer from 1 to 6: app [--min-level N] [--max-level M] [FILE]")
	}
	return n
}

func main() {
	minL, maxL := 1, 6
	file := ""
	haveFile := false
	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--min-level" || a == "--max-level":
			if i+1 >= len(args) {
				usage("missing value for " + a + ": app [--min-level N] [--max-level M] [FILE]")
			}
			i++
			v := parseLevel(args[i])
			if a == "--min-level" {
				minL = v
			} else {
				maxL = v
			}
		case strings.HasPrefix(a, "--min-level="):
			minL = parseLevel(strings.TrimPrefix(a, "--min-level="))
		case strings.HasPrefix(a, "--max-level="):
			maxL = parseLevel(strings.TrimPrefix(a, "--max-level="))
		case strings.HasPrefix(a, "-") && a != "-":
			usage("unknown option " + a + ": app [--min-level N] [--max-level M] [FILE]")
		default:
			if haveFile {
				usage("more than one FILE: app [--min-level N] [--max-level M] [FILE]")
			}
			file, haveFile = a, true
		}
	}
	if minL > maxL {
		usage("--min-level must not exceed --max-level: app [--min-level N] [--max-level M] [FILE]")
	}

	var data []byte
	var err error
	if haveFile {
		data, err = os.ReadFile(file)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", file)
			os.Exit(2)
		}
	} else {
		data, err = io.ReadAll(os.Stdin)
		if err != nil {
			fmt.Fprintln(os.Stderr, "error: cannot read standard input")
			os.Exit(2)
		}
	}

	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	seen := map[string]int{}
	for _, h := range extract(string(data)) {
		base := slug(h.text)
		s := base
		if n := seen[base]; n > 0 {
			s = base + "-" + strconv.Itoa(n)
		}
		seen[base]++
		if h.level < minL || h.level > maxL {
			continue
		}
		fmt.Fprintf(w, "%s- [%s](#%s)\n", strings.Repeat("  ", h.level-minL), h.text, s)
	}
}

// leadingSpaces returns the number of leading spaces in s.
func leadingSpaces(s string) int {
	n := 0
	for n < len(s) && s[n] == ' ' {
		n++
	}
	return n
}

// fenceOpen reports the fence character and run length if line opens a fence.
func fenceOpen(line string) (byte, int) {
	ind := leadingSpaces(line)
	if ind > 3 || ind >= len(line) {
		return 0, 0
	}
	c := line[ind]
	if c != '`' && c != '~' {
		return 0, 0
	}
	n := 0
	for ind+n < len(line) && line[ind+n] == c {
		n++
	}
	if n < 3 {
		return 0, 0
	}
	return c, n
}

func fenceCloses(line string, c byte, n int) bool {
	ind := leadingSpaces(line)
	if ind > 3 {
		return false
	}
	run := 0
	for ind+run < len(line) && line[ind+run] == c {
		run++
	}
	if run < n {
		return false
	}
	return strings.Trim(line[ind+run:], " \t") == ""
}

// parseHeading returns the level and text of an ATX heading line.
func parseHeading(line string) (int, string, bool) {
	ind := leadingSpaces(line)
	if ind > 3 {
		return 0, "", false
	}
	rest := line[ind:]
	l := 0
	for l < len(rest) && rest[l] == '#' {
		l++
	}
	if l < 1 || l > 6 {
		return 0, "", false
	}
	rest = rest[l:]
	if rest != "" && rest[0] != ' ' && rest[0] != '\t' {
		return 0, "", false
	}
	text := strings.Trim(rest, " \t")
	t := strings.TrimRight(text, "#")
	if len(t) < len(text) {
		if t == "" {
			text = ""
		} else if last := t[len(t)-1]; last == ' ' || last == '\t' {
			text = strings.TrimRight(t, " \t")
		}
	}
	return l, text, true
}

func extract(src string) []heading {
	var out []heading
	var fc byte
	fn := 0
	for _, line := range strings.Split(src, "\n") {
		line = strings.TrimSuffix(line, "\r")
		if fc != 0 {
			if fenceCloses(line, fc, fn) {
				fc = 0
			}
			continue
		}
		if c, n := fenceOpen(line); c != 0 {
			fc, fn = c, n
			continue
		}
		if l, t, ok := parseHeading(line); ok && t != "" {
			out = append(out, heading{l, t})
		}
	}
	return out
}

func slug(text string) string {
	var b strings.Builder
	for _, r := range text {
		r = unicode.ToLower(r)
		switch {
		case r == ' ':
			b.WriteByte('-')
		case r == '-' || r == '_' || unicode.IsLetter(r) || unicode.IsMark(r) || unicode.Is(unicode.Nd, r):
			b.WriteRune(r)
		}
	}
	return b.String()
}
