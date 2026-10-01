// csv-report totals the "amount" column of a CSV file, grouped by another column.
package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

const usageText = "usage: app --by COLUMN [FILE]"

func main() {
	os.Exit(run(os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}

func run(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	by, hasBy := "", false
	var files []string
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--by":
			if i+1 >= len(args) {
				fmt.Fprintln(stderr, usageText)
				return 64
			}
			i++
			by, hasBy = args[i], true
		case len(a) > 1 && a[0] == '-':
			fmt.Fprintln(stderr, usageText)
			return 64
		default:
			files = append(files, a)
		}
	}
	if !hasBy || len(files) > 1 {
		fmt.Fprintln(stderr, usageText)
		return 64
	}

	var data []byte
	var err error
	if len(files) == 1 {
		data, err = os.ReadFile(files[0])
		if err != nil {
			fmt.Fprintf(stderr, "error: cannot read %s\n", files[0])
			return 2
		}
	} else {
		data, err = io.ReadAll(stdin)
		if err != nil {
			fmt.Fprintln(stderr, "error: cannot read standard input")
			return 2
		}
	}

	p := &parser{data: data, line: 1}

	header, ok := p.next()
	if !ok || header.err != "" {
		fmt.Fprintln(stderr, "error: empty input")
		return 2
	}
	amountCol, byCol := indexOf(header.fields, "amount"), indexOf(header.fields, by)
	if amountCol < 0 {
		fmt.Fprintln(stderr, "error: missing column amount")
		return 2
	}
	if byCol < 0 {
		fmt.Fprintf(stderr, "error: missing column %s\n", by)
		return 2
	}

	errw := bufio.NewWriter(stderr)
	totals := map[string]int64{}
	bad := false
	for {
		rec, ok := p.next()
		if !ok {
			break
		}
		reason := rec.err
		if reason == "" && len(rec.fields) != len(header.fields) {
			reason = fmt.Sprintf("expected %d fields, got %d", len(header.fields), len(rec.fields))
		}
		var cents int64
		if reason == "" {
			var valid bool
			cents, valid = parseAmount(rec.fields[amountCol])
			if !valid {
				reason = "invalid amount"
			}
		}
		if reason != "" {
			bad = true
			fmt.Fprintf(errw, "line %d: %s\n", rec.line, reason)
			continue
		}
		totals[rec.fields[byCol]] += cents
	}
	errw.Flush()

	keys := make([]string, 0, len(totals))
	for k := range totals {
		keys = append(keys, k)
	}
	sort.Strings(keys) // byte order of UTF-8 is code point order

	out := bufio.NewWriter(stdout)
	out.WriteString(quoteField(by) + ",total\n")
	for _, k := range keys {
		out.WriteString(quoteField(k) + "," + formatCents(totals[k]) + "\n")
	}
	out.Flush()
	if bad {
		return 1
	}
	return 0
}

func indexOf(fields []string, name string) int {
	for i, f := range fields {
		if f == name {
			return i
		}
	}
	return -1
}

// parseAmount parses an amount into cents.
func parseAmount(s string) (int64, bool) {
	s = strings.Trim(s, " ")
	neg := false
	if strings.HasPrefix(s, "-") {
		neg = true
		s = s[1:]
	}
	whole, frac, hasDot := strings.Cut(s, ".")
	if !allDigits(whole) || (hasDot && (len(frac) < 1 || len(frac) > 2 || !allDigits(frac))) {
		return 0, false
	}
	var v int64
	for _, c := range whole {
		v = v*10 + int64(c-'0')
	}
	v *= 100
	if hasDot {
		f := int64(frac[0]-'0') * 10
		if len(frac) == 2 {
			f += int64(frac[1] - '0')
		}
		v += f
	}
	if neg {
		v = -v
	}
	return v, true
}

func allDigits(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func formatCents(c int64) string {
	sign := ""
	if c < 0 {
		sign = "-"
		c = -c
	}
	return fmt.Sprintf("%s%d.%02d", sign, c/100, c%100)
}

func quoteField(s string) string {
	if !strings.ContainsAny(s, ",\"\r\n") {
		return s
	}
	return `"` + strings.ReplaceAll(s, `"`, `""`) + `"`
}

// record is one parsed CSV record. A non-empty err marks it bad.
type record struct {
	fields []string
	line   int
	err    string
}

type parser struct {
	data []byte
	pos  int
	line int // line number at pos
}

// next returns the next non-empty record, or false at end of input.
func (p *parser) next() (record, bool) {
	d := p.data
	// Skip empty lines.
	for p.pos < len(d) {
		if d[p.pos] == '\n' {
			p.pos++
			p.line++
		} else if d[p.pos] == '\r' && p.pos+1 < len(d) && d[p.pos+1] == '\n' {
			p.pos += 2
			p.line++
		} else {
			break
		}
	}
	if p.pos >= len(d) {
		return record{}, false
	}

	rec := record{line: p.line}
	for {
		// Parse one field starting at p.pos.
		var sb strings.Builder
		if p.pos < len(d) && d[p.pos] == '"' {
			p.pos++
			closed := false
			for p.pos < len(d) {
				c := d[p.pos]
				if c == '"' {
					if p.pos+1 < len(d) && d[p.pos+1] == '"' {
						sb.WriteByte('"')
						p.pos += 2
						continue
					}
					p.pos++
					closed = true
					break
				}
				if c == '\n' {
					p.line++
				}
				sb.WriteByte(c)
				p.pos++
			}
			if !closed {
				rec.err = "unterminated quote"
				p.pos = len(d)
				return rec, true
			}
			rec.fields = append(rec.fields, sb.String())
			// Must be followed by comma, line break, or end of input.
			switch {
			case p.pos >= len(d):
				return rec, true
			case d[p.pos] == ',':
				p.pos++
				continue
			case d[p.pos] == '\n':
				p.pos++
				p.line++
				return rec, true
			case d[p.pos] == '\r' && p.pos+1 < len(d) && d[p.pos+1] == '\n':
				p.pos += 2
				p.line++
				return rec, true
			}
			rec.err = "unexpected character after closing quote"
			for p.pos < len(d) && d[p.pos] != '\n' {
				p.pos++
			}
			if p.pos < len(d) {
				p.pos++
				p.line++
			}
			return rec, true
		}
		// Unquoted field.
		start := p.pos
		for p.pos < len(d) && d[p.pos] != ',' && d[p.pos] != '\n' &&
			!(d[p.pos] == '\r' && p.pos+1 < len(d) && d[p.pos+1] == '\n') {
			p.pos++
		}
		rec.fields = append(rec.fields, string(d[start:p.pos]))
		switch {
		case p.pos >= len(d):
			return rec, true
		case d[p.pos] == ',':
			p.pos++
		case d[p.pos] == '\n':
			p.pos++
			p.line++
			return rec, true
		default:
			p.pos += 2
			p.line++
			return rec, true
		}
	}
}
