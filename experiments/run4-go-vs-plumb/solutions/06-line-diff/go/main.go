// line-diff: print the differences between two text files as a unified diff.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"strconv"
)

const usage = "usage: app [-U N] OLD NEW"

type opKind byte

const (
	keep   opKind = ' '
	remove opKind = '-'
	add    opKind = '+'
)

type op struct {
	kind opKind
	line string
}

func main() {
	os.Exit(run(os.Args[1:]))
}

func fail(msg string) int {
	fmt.Fprintln(os.Stderr, msg)
	return 2
}

func run(args []string) int {
	context := 3
	for len(args) > 0 && len(args[0]) > 1 && args[0][0] == '-' {
		if args[0] != "-U" || len(args) < 2 {
			return fail(usage)
		}
		n, ok := parseCount(args[1])
		if !ok {
			return fail(usage)
		}
		context = n
		args = args[2:]
	}
	if len(args) != 2 {
		return fail(usage)
	}
	oldName, newName := args[0], args[1]

	oldData, err := os.ReadFile(oldName)
	if err != nil {
		return fail("error: cannot read " + oldName)
	}
	newData, err := os.ReadFile(newName)
	if err != nil {
		return fail("error: cannot read " + newName)
	}

	ops := diff(splitLines(oldData), splitLines(newData))
	changed := false
	for _, o := range ops {
		if o.kind != keep {
			changed = true
			break
		}
	}
	if !changed {
		return 0
	}

	w := bufio.NewWriter(os.Stdout)
	fmt.Fprintf(w, "--- %s\n+++ %s\n", oldName, newName)
	writeHunks(w, ops, context)
	if err := w.Flush(); err != nil {
		return 2
	}
	return 1
}

// parseCount accepts a whole number >= 0 written as decimal digits only.
// Values too large for an int are clamped, which behaves the same.
func parseCount(s string) (int, bool) {
	if s == "" {
		return 0, false
	}
	for _, c := range []byte(s) {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	n, err := strconv.Atoi(s)
	if err != nil {
		return 1 << 40, true
	}
	return n, true
}

// splitLines splits after each LF; each line keeps its LF.
func splitLines(data []byte) []string {
	var lines []string
	for len(data) > 0 {
		i := bytes.IndexByte(data, '\n')
		if i < 0 {
			lines = append(lines, string(data))
			break
		}
		lines = append(lines, string(data[:i+1]))
		data = data[i+1:]
	}
	return lines
}

// diff walks the files exactly as the spec describes, using the suffix LCS
// table L(i, j).
func diff(a, b []string) []op {
	n, m := len(a), len(b)
	w := m + 1
	l := make([]int32, (n+1)*w)
	for i := n - 1; i >= 0; i-- {
		for j := m - 1; j >= 0; j-- {
			if a[i] == b[j] {
				l[i*w+j] = l[(i+1)*w+j+1] + 1
			} else if x, y := l[(i+1)*w+j], l[i*w+j+1]; x >= y {
				l[i*w+j] = x
			} else {
				l[i*w+j] = y
			}
		}
	}
	var ops []op
	i, j := 0, 0
	for i < n || j < m {
		switch {
		case i < n && j < m && a[i] == b[j]:
			ops = append(ops, op{keep, a[i]})
			i++
			j++
		case j == m || (i < n && l[(i+1)*w+j] >= l[i*w+j+1]):
			ops = append(ops, op{remove, a[i]})
			i++
		default:
			ops = append(ops, op{add, b[j]})
			j++
		}
	}
	return ops
}

func writeHunks(w *bufio.Writer, ops []op, context int) {
	// Positions of changes.
	var idx []int
	for k, o := range ops {
		if o.kind != keep {
			idx = append(idx, k)
		}
	}
	// Running counts of old/new lines before each op.
	oldBefore := make([]int, len(ops)+1)
	newBefore := make([]int, len(ops)+1)
	for k, o := range ops {
		oldBefore[k+1] = oldBefore[k]
		newBefore[k+1] = newBefore[k]
		if o.kind != add {
			oldBefore[k+1]++
		}
		if o.kind != remove {
			newBefore[k+1]++
		}
	}

	for g := 0; g < len(idx); {
		first, last := idx[g], idx[g]
		g++
		for g < len(idx) && idx[g]-last-1 <= 2*context {
			last = idx[g]
			g++
		}
		start := first - min(context, first)
		end := last + 1 + min(context, len(ops)-last-1)

		c1 := oldBefore[end] - oldBefore[start]
		c2 := newBefore[end] - newBefore[start]
		s1, s2 := oldBefore[start], newBefore[start]
		if c1 > 0 {
			s1++
		}
		if c2 > 0 {
			s2++
		}
		fmt.Fprintf(w, "@@ -%s +%s @@\n", rng(s1, c1), rng(s2, c2))
		for _, o := range ops[start:end] {
			w.WriteByte(byte(o.kind))
			w.WriteString(o.line)
			if len(o.line) == 0 || o.line[len(o.line)-1] != '\n' {
				w.WriteString("\n\\ No newline at end of file\n")
			}
		}
	}
}

func rng(start, count int) string {
	if count == 1 {
		return strconv.Itoa(start)
	}
	return strconv.Itoa(start) + "," + strconv.Itoa(count)
}
