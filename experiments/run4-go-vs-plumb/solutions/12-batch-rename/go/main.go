// batch-rename: rename many files in one directory by a pattern, all or nothing.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const usageText = "usage: app DIR --regex PATTERN --to TEMPLATE [--dry-run]\n" +
	"       app DIR --counter TEMPLATE [--start N] [--match PATTERN] [--dry-run]"

type usageError struct{ msg string }

func (e usageError) Error() string { return e.msg }

func usagef(format string, args ...any) error {
	return usageError{fmt.Sprintf(format, args...)}
}

// failure is an error that stops the run with exit code 1.
type failure struct{ msg string }

func (e failure) Error() string { return e.msg }

type options struct {
	dir     string
	regex   *string
	to      *string
	counter *string
	start   *string
	match   *string
	dryRun  bool
}

func parseArgs(args []string) (*options, error) {
	o := &options{}
	haveDir := false
	setters := map[string]**string{
		"--regex":   &o.regex,
		"--to":      &o.to,
		"--counter": &o.counter,
		"--start":   &o.start,
		"--match":   &o.match,
	}
	onlyPositional := false
	for i := 0; i < len(args); i++ {
		a := args[i]
		if onlyPositional || a == "" || a[0] != '-' || a == "-" {
			if haveDir {
				return nil, usagef("unexpected argument: %s", a)
			}
			o.dir, haveDir = a, true
			continue
		}
		if a == "--" {
			onlyPositional = true
			continue
		}
		if a == "--dry-run" {
			if o.dryRun {
				return nil, usagef("duplicate option: --dry-run")
			}
			o.dryRun = true
			continue
		}
		name, val, hasVal := a, "", false
		if eq := strings.IndexByte(a, '='); eq >= 0 && strings.HasPrefix(a, "--") {
			name, val, hasVal = a[:eq], a[eq+1:], true
		}
		slot, ok := setters[name]
		if !ok {
			return nil, usagef("unknown option: %s", a)
		}
		if !hasVal {
			if i+1 >= len(args) {
				return nil, usagef("missing value for %s", name)
			}
			i++
			val = args[i]
		}
		if *slot != nil {
			return nil, usagef("duplicate option: %s", name)
		}
		v := val
		*slot = &v
	}
	if !haveDir {
		return nil, usagef("missing DIR")
	}
	if (o.regex != nil) == (o.counter != nil) {
		return nil, usagef("exactly one of --regex and --counter must be given")
	}
	if o.regex != nil {
		if o.to == nil {
			return nil, usagef("--regex requires --to")
		}
		if o.start != nil || o.match != nil {
			return nil, usagef("--start and --match only go with --counter")
		}
	} else if o.to != nil {
		return nil, usagef("--to only goes with --regex")
	}
	return o, nil
}

// compile builds a matcher for the whole name.
func compile(pattern string) (*regexp.Regexp, error) {
	re, err := regexp.Compile(`(?s)^(?:` + pattern + `)$`)
	if err != nil {
		return nil, usagef("invalid pattern %q: %v", pattern, err)
	}
	return re, nil
}

// A templatePart is literal text, or a reference to a group when group >= 0.
type templatePart struct {
	lit   string
	group int
}

func parseTemplate(t string, ngroups int) ([]templatePart, error) {
	var parts []templatePart
	var lit strings.Builder
	flush := func() {
		if lit.Len() > 0 {
			parts = append(parts, templatePart{lit: lit.String(), group: -1})
			lit.Reset()
		}
	}
	for i := 0; i < len(t); i++ {
		c := t[i]
		switch c {
		case '{':
			if i+1 < len(t) && t[i+1] == '{' {
				lit.WriteByte('{')
				i++
				continue
			}
			j := i + 1
			for j < len(t) && t[j] >= '0' && t[j] <= '9' {
				j++
			}
			if j == i+1 || j >= len(t) || t[j] != '}' {
				return nil, usagef("invalid template: %q", t)
			}
			n, err := strconv.Atoi(t[i+1 : j])
			if err != nil || n > ngroups {
				return nil, usagef("invalid template: no group %s in pattern", t[i+1:j])
			}
			flush()
			parts = append(parts, templatePart{group: n})
			i = j
		case '}':
			if i+1 < len(t) && t[i+1] == '}' {
				lit.WriteByte('}')
				i++
				continue
			}
			return nil, usagef("invalid template: %q", t)
		default:
			lit.WriteByte(c)
		}
	}
	flush()
	return parts, nil
}

func expand(parts []templatePart, name string, m []int) string {
	var b strings.Builder
	for _, p := range parts {
		if p.group < 0 {
			b.WriteString(p.lit)
			continue
		}
		if s, e := m[2*p.group], m[2*p.group+1]; s >= 0 {
			b.WriteString(name[s:e])
		}
	}
	return b.String()
}

// parseCounter splits a counter template into prefix, width of the run, suffix.
func parseCounter(t string) (prefix string, width int, suffix string, err error) {
	first := strings.IndexByte(t, '#')
	if first < 0 {
		return "", 0, "", usagef("invalid template: %q has no #", t)
	}
	last := first
	for last < len(t) && t[last] == '#' {
		last++
	}
	if strings.IndexByte(t[last:], '#') >= 0 {
		return "", 0, "", usagef("invalid template: %q has more than one run of #", t)
	}
	return t[:first], last - first, t[last:], nil
}

type rename struct{ old, new string }

func run(args []string) (out string, err error) {
	o, err := parseArgs(args)
	if err != nil {
		return "", err
	}

	var (
		re      *regexp.Regexp
		parts   []templatePart
		prefix  string
		suffix  string
		width   int
		start   uint64 = 1
		counter        = o.counter != nil
	)
	if counter {
		if prefix, width, suffix, err = parseCounter(*o.counter); err != nil {
			return "", err
		}
		if o.start != nil {
			s := *o.start
			if s == "" || strings.Trim(s, "0123456789") != "" {
				return "", usagef("bad --start: %q", s)
			}
			if start, err = strconv.ParseUint(s, 10, 63); err != nil {
				return "", usagef("bad --start: %q", s)
			}
		}
		if o.match != nil {
			if re, err = compile(*o.match); err != nil {
				return "", err
			}
		}
	} else {
		if re, err = compile(*o.regex); err != nil {
			return "", err
		}
		if parts, err = parseTemplate(*o.to, re.NumSubexp()); err != nil {
			return "", err
		}
	}

	dir := o.dir
	if st, serr := os.Stat(dir); serr != nil || !st.IsDir() {
		return "", notDir{dir}
	}
	entries, rerr := os.ReadDir(dir)
	if rerr != nil {
		return "", notDir{dir}
	}
	existing := make(map[string]bool, len(entries))
	var cands []string
	for _, e := range entries {
		existing[e.Name()] = true
		if e.Type().IsRegular() {
			cands = append(cands, e.Name())
		}
	}
	sort.Strings(cands)

	var batch []rename
	k := uint64(0)
	for _, name := range cands {
		var nn string
		if counter {
			if re != nil && !re.MatchString(name) {
				continue
			}
			num := strconv.FormatUint(start+k, 10)
			k++
			if len(num) < width {
				num = strings.Repeat("0", width-len(num)) + num
			}
			nn = prefix + num + suffix
		} else {
			m := re.FindStringSubmatchIndex(name)
			if m == nil {
				continue
			}
			nn = expand(parts, name, m)
		}
		if nn != name {
			batch = append(batch, rename{name, nn})
		}
	}

	for _, r := range batch {
		if r.new == "" || r.new == "." || r.new == ".." ||
			strings.ContainsAny(r.new, "/\x00") {
			return "", failure{"invalid name: " + r.new}
		}
	}
	seen := make(map[string]bool, len(batch))
	for _, r := range batch {
		if seen[r.new] {
			return "", failure{"conflict: " + r.new}
		}
		seen[r.new] = true
	}
	for _, r := range batch {
		if existing[r.new] {
			return "", failure{"exists: " + r.new}
		}
	}

	if !o.dryRun {
		for i, r := range batch {
			if rerr := os.Rename(filepath.Join(dir, r.old), filepath.Join(dir, r.new)); rerr != nil {
				for j := i - 1; j >= 0; j-- { // roll back
					os.Rename(filepath.Join(dir, batch[j].new), filepath.Join(dir, batch[j].old))
				}
				return "", failure{fmt.Sprintf("cannot rename %s: %v", r.old, rerr)}
			}
		}
	}

	var b strings.Builder
	for _, r := range batch {
		b.WriteString(r.old + " -> " + r.new + "\n")
	}
	verb := "renamed"
	if o.dryRun {
		verb = "would rename"
	}
	fmt.Fprintf(&b, "%s %d\n", verb, len(batch))
	return b.String(), nil
}

type notDir struct{ dir string }

func (e notDir) Error() string { return "not a directory: " + e.dir }

func main() {
	out, err := run(os.Args[1:])
	switch e := err.(type) {
	case nil:
		os.Stdout.WriteString(out)
	case usageError:
		fmt.Fprintf(os.Stderr, "usage: %s\n%s\n", e.msg, usageText)
		os.Exit(64)
	case notDir:
		fmt.Fprintf(os.Stderr, "error: %s\n", e.Error())
		os.Exit(2)
	default:
		fmt.Fprintf(os.Stderr, "error: %s\n", err.Error())
		os.Exit(1)
	}
}
