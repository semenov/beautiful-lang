package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
)

type cond struct {
	path  []string
	value string
}

func usage(msg string) {
	fmt.Fprintln(os.Stderr, "usage: app [--where PATH=VALUE]... [--select PATH,PATH...]")
	if msg != "" {
		fmt.Fprintln(os.Stderr, "  "+msg)
	}
	os.Exit(64)
}

func splitPath(p string) ([]string, bool) {
	keys := strings.Split(p, ".")
	for _, k := range keys {
		if k == "" {
			return nil, false
		}
	}
	return keys, true
}

// lookup follows path through nested objects.
func lookup(v *value, path []string) *value {
	for _, k := range path {
		if v.kind != kObject {
			return nil
		}
		var next *value
		for _, m := range v.members {
			if m.key == k {
				next = m.val
				break
			}
		}
		if next == nil {
			return nil
		}
		v = next
	}
	return v
}

func (c cond) holds(rec *value) bool {
	v := lookup(rec, c.path)
	if v == nil {
		return false
	}
	switch v.kind {
	case kString, kNumber, kLiteral:
		return v.text == c.value
	}
	return false
}

func main() {
	var wheres []cond
	var selectRaw []string
	var selectPaths [][]string
	hasSelect := false

	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		a := args[i]
		name, val, hasVal := a, "", false
		if strings.HasPrefix(a, "--") {
			if eq := strings.IndexByte(a, '='); eq >= 0 {
				name, val, hasVal = a[:eq], a[eq+1:], true
			}
		}
		if name != "--where" && name != "--select" {
			usage("unknown option: " + a)
		}
		if !hasVal {
			if i+1 >= len(args) {
				usage("missing value for " + name)
			}
			i++
			val = args[i]
		}
		if name == "--where" {
			eq := strings.IndexByte(val, '=')
			if eq < 0 {
				usage("--where needs PATH=VALUE")
			}
			path, ok := splitPath(val[:eq])
			if !ok {
				usage("empty key in path: " + val[:eq])
			}
			wheres = append(wheres, cond{path, val[eq+1:]})
		} else {
			hasSelect = true
			selectRaw = selectRaw[:0]
			selectPaths = selectPaths[:0]
			for _, p := range strings.Split(val, ",") {
				path, ok := splitPath(p)
				if !ok {
					usage("empty key in path: " + p)
				}
				selectRaw = append(selectRaw, p)
				selectPaths = append(selectPaths, path)
			}
		}
	}

	in := bufio.NewReaderSize(os.Stdin, 1<<20)
	out := bufio.NewWriterSize(os.Stdout, 1<<16)
	exit := 0
	var sb strings.Builder

	for n := 1; ; n++ {
		line, err := in.ReadString('\n')
		if err != nil && err != io.EOF {
			fmt.Fprintln(os.Stderr, "error: reading input:", err)
			exit = 1
			break
		}
		if len(line) == 0 && err == io.EOF {
			break
		}
		line = strings.TrimSuffix(line, "\n")
		if strings.Trim(line, " \t\r") != "" {
			rec, perr := parseDocument(line)
			switch {
			case perr != nil:
				out.Flush()
				fmt.Fprintf(os.Stderr, "error: line %d: invalid JSON\n", n)
				exit = 1
			case rec.kind != kObject:
				out.Flush()
				fmt.Fprintf(os.Stderr, "error: line %d: not an object\n", n)
				exit = 1
			default:
				ok := true
				for _, c := range wheres {
					if !c.holds(rec) {
						ok = false
						break
					}
				}
				if ok {
					sb.Reset()
					if hasSelect {
						sb.WriteByte('{')
						for j, p := range selectPaths {
							if j > 0 {
								sb.WriteByte(',')
							}
							writeString(&sb, selectRaw[j])
							sb.WriteByte(':')
							if v := lookup(rec, p); v != nil {
								writeValue(&sb, v)
							} else {
								sb.WriteString("null")
							}
						}
						sb.WriteByte('}')
					} else {
						writeValue(&sb, rec)
					}
					sb.WriteByte('\n')
					out.WriteString(sb.String())
				}
			}
		}
		if err == io.EOF {
			break
		}
	}
	out.Flush()
	os.Exit(exit)
}
