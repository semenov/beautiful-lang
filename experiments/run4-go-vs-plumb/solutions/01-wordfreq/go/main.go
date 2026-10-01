// wordfreq prints the most common words in text.
package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

const usage = "usage: app [--top N] [--min-length L] [FILE...]"

type options struct {
	top       int
	minLength int
	files     []string
}

func usageError(msg string) {
	fmt.Fprintln(os.Stderr, usage)
	if msg != "" {
		fmt.Fprintln(os.Stderr, "  "+msg)
	}
	os.Exit(64)
}

// parseCount parses a whole number (digits only) that is at least min.
// Values too large for an int are clamped, which behaves the same as
// "unlimited" for --top and "longer than any word" for --min-length.
func parseCount(s string, min int) (int, bool) {
	if s == "" {
		return 0, false
	}
	n := 0
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
		if n > (int(^uint(0)>>1)-9)/10 {
			n = int(^uint(0) >> 1)
			continue
		}
		n = n*10 + int(c-'0')
	}
	if n < min {
		return 0, false
	}
	return n, true
}

func parseArgs(args []string) options {
	opts := options{top: 10, minLength: 1}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			opts.files = append(opts.files, args[i+1:]...)
			break
		}
		if !strings.HasPrefix(a, "--") {
			if strings.HasPrefix(a, "-") && a != "-" {
				usageError("unknown option " + a)
			}
			opts.files = append(opts.files, a)
			continue
		}
		name, value, hasValue := strings.Cut(a, "=")
		if name != "--top" && name != "--min-length" {
			usageError("unknown option " + a)
		}
		if !hasValue {
			if i+1 >= len(args) {
				usageError("missing value for " + name)
			}
			i++
			value = args[i]
		}
		if name == "--top" {
			n, ok := parseCount(value, 0)
			if !ok {
				usageError("--top needs a whole number >= 0, got " + value)
			}
			opts.top = n
		} else {
			n, ok := parseCount(value, 1)
			if !ok {
				usageError("--min-length needs a whole number >= 1, got " + value)
			}
			opts.minLength = n
		}
	}
	return opts
}

// readAll returns the concatenated contents of the files, or stdin if none.
// On failure it returns the name of the file that could not be read.
func readAll(files []string) ([]byte, string, error) {
	if len(files) == 0 {
		data, err := io.ReadAll(os.Stdin)
		return data, "", err
	}
	var all []byte
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			return nil, f, err
		}
		all = append(all, data...)
	}
	return all, "", nil
}

func isWordRune(r rune) bool {
	return r == '\'' || unicode.IsLetter(r)
}

// countWords splits text into words and counts them (lowercased), ignoring
// words shorter than minLength code points. It returns the counts and the
// number of words counted.
func countWords(text []byte, minLength int) (map[string]int, int) {
	counts := make(map[string]int)
	total := 0
	var word []rune
	flush := func() {
		start, end := 0, len(word)
		for start < end && word[start] == '\'' {
			start++
		}
		for end > start && word[end-1] == '\'' {
			end--
		}
		if w := word[start:end]; len(w) >= minLength && len(w) > 0 {
			counts[string(w)]++
			total++
		}
		word = word[:0]
	}
	for len(text) > 0 {
		r, size := utf8.DecodeRune(text)
		text = text[size:]
		if r == utf8.RuneError && size == 1 {
			flush() // invalid byte: separator
			continue
		}
		if isWordRune(r) {
			word = append(word, unicode.ToLower(r))
		} else {
			flush()
		}
	}
	flush()
	return counts, total
}

type entry struct {
	word  string
	count int
}

func ranked(counts map[string]int) []entry {
	entries := make([]entry, 0, len(counts))
	for w, c := range counts {
		entries = append(entries, entry{w, c})
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].count != entries[j].count {
			return entries[i].count > entries[j].count
		}
		return entries[i].word < entries[j].word // byte order == code point order in UTF-8
	})
	return entries
}

func main() {
	opts := parseArgs(os.Args[1:])
	data, failed, err := readAll(opts.files)
	if err != nil {
		if failed == "" {
			fmt.Fprintln(os.Stderr, "error: cannot read standard input")
		} else {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", failed)
		}
		os.Exit(2)
	}
	counts, total := countWords(data, opts.minLength)
	entries := ranked(counts)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for i, e := range entries {
		if i >= opts.top {
			break
		}
		fmt.Fprintf(out, "%s %d\n", e.word, e.count)
	}
	fmt.Fprintf(out, "total %d unique %d\n", total, len(counts))
}
