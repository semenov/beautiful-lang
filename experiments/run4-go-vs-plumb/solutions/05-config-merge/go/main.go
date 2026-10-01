package main

import (
	"bufio"
	"fmt"
	"os"
	"sort"
	"strings"
)

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app FILE...")
	os.Exit(64)
}

func fail(code int, format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(code)
}

// merge merges b into a (both objects) and returns a.
func merge(a, b *value) *value {
	for k, bv := range b.obj {
		if av, ok := a.obj[k]; ok && av.kind == kObject && bv.kind == kObject {
			merge(av, bv)
		} else {
			a.obj[k] = bv
		}
	}
	return a
}

func lowerASCII(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + 32
		}
	}
	return string(b)
}

func applyEnv(root *value) {
	var names []string
	for _, kv := range os.Environ() {
		name, _, _ := strings.Cut(kv, "=")
		if strings.HasPrefix(name, "APP_") {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	for _, name := range names {
		path := strings.Split(name[len("APP_"):], "__")
		ok := true
		for i := range path {
			if path[i] == "" {
				ok = false
			}
			path[i] = lowerASCII(path[i])
		}
		if !ok {
			continue
		}
		raw := os.Getenv(name)
		v, err := parseJSON(raw)
		if err != nil {
			v = &value{kind: kString, s: raw}
		}
		cur := root
		for _, k := range path[:len(path)-1] {
			next, ok := cur.obj[k]
			if !ok || next.kind != kObject {
				next = &value{kind: kObject, obj: map[string]*value{}}
				cur.obj[k] = next
			}
			cur = next
		}
		cur.obj[path[len(path)-1]] = v
	}
}

func lookup(root *value, path ...string) *value {
	cur := root
	for _, k := range path {
		if cur == nil || cur.kind != kObject {
			return nil
		}
		cur = cur.obj[k]
	}
	return cur
}

func nonEmptyString(v *value) bool { return v != nil && v.kind == kString && v.s != "" }

func validPort(v *value) bool {
	if v == nil || v.kind != kNumber || len(v.s) > 5 {
		return false
	}
	n := 0
	for i := 0; i < len(v.s); i++ {
		if !isDigit(v.s[i]) {
			return false
		}
		n = n*10 + int(v.s[i]-'0')
	}
	return n >= 1 && n <= 65535
}

func validate(root *value) []string {
	var bad []string
	if !nonEmptyString(lookup(root, "service", "name")) {
		bad = append(bad, "service.name")
	}
	if !nonEmptyString(lookup(root, "db", "host")) {
		bad = append(bad, "db.host")
	}
	if !validPort(lookup(root, "db", "port")) {
		bad = append(bad, "db.port")
	}
	return bad
}

func writeString(w *bufio.Writer, s string) {
	const hexd = "0123456789abcdef"
	w.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"':
			w.WriteString(`\"`)
		case c == '\\':
			w.WriteString(`\\`)
		case c == '\b':
			w.WriteString(`\b`)
		case c == '\t':
			w.WriteString(`\t`)
		case c == '\n':
			w.WriteString(`\n`)
		case c == '\f':
			w.WriteString(`\f`)
		case c == '\r':
			w.WriteString(`\r`)
		case c < 0x20:
			w.WriteString(`\u00`)
			w.WriteByte(hexd[c>>4])
			w.WriteByte(hexd[c&15])
		default:
			w.WriteByte(c)
		}
	}
	w.WriteByte('"')
}

func writeValue(w *bufio.Writer, v *value, indent int) {
	pad := func(n int) {
		for i := 0; i < n; i++ {
			w.WriteString("  ")
		}
	}
	switch v.kind {
	case kNull:
		w.WriteString("null")
	case kBool:
		if v.b {
			w.WriteString("true")
		} else {
			w.WriteString("false")
		}
	case kNumber:
		w.WriteString(v.s)
	case kString:
		writeString(w, v.s)
	case kArray:
		if len(v.arr) == 0 {
			w.WriteString("[]")
			return
		}
		w.WriteString("[\n")
		for i, e := range v.arr {
			pad(indent + 1)
			writeValue(w, e, indent+1)
			if i < len(v.arr)-1 {
				w.WriteByte(',')
			}
			w.WriteByte('\n')
		}
		pad(indent)
		w.WriteByte(']')
	case kObject:
		if len(v.obj) == 0 {
			w.WriteString("{}")
			return
		}
		keys := make([]string, 0, len(v.obj))
		for k := range v.obj {
			keys = append(keys, k)
		}
		sort.Strings(keys) // UTF-8 byte order is code point order
		w.WriteString("{\n")
		for i, k := range keys {
			pad(indent + 1)
			writeString(w, k)
			w.WriteString(": ")
			writeValue(w, v.obj[k], indent+1)
			if i < len(keys)-1 {
				w.WriteByte(',')
			}
			w.WriteByte('\n')
		}
		pad(indent)
		w.WriteByte('}')
	}
}

func main() {
	files := os.Args[1:]
	if len(files) == 0 {
		usage()
	}
	for _, f := range files {
		if strings.HasPrefix(f, "-") {
			usage()
		}
	}

	var root *value
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			fail(2, "error: cannot read %s", f)
		}
		v, err := parseJSON(string(data))
		if err != nil {
			fail(2, "error: invalid JSON in %s", f)
		}
		if v.kind != kObject {
			fail(2, "error: %s is not an object", f)
		}
		if root == nil {
			root = v
		} else {
			merge(root, v)
		}
	}

	applyEnv(root)

	if bad := validate(root); len(bad) > 0 {
		for _, k := range bad {
			fmt.Fprintf(os.Stderr, "error: invalid %s\n", k)
		}
		os.Exit(1)
	}

	w := bufio.NewWriter(os.Stdout)
	writeValue(w, root, 0)
	w.WriteByte('\n')
	w.Flush()
}
