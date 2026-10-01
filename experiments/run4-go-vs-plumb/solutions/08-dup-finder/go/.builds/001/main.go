// dup-finder: find duplicate files under a directory tree by SHA-256 of content.
package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"sort"
	"strconv"
	"strings"
)

const usageText = "usage: app [--min-size BYTES] DIR"

type options struct {
	minSize int64
	dir     string
}

func usageErr(w io.Writer, msg string) {
	if msg != "" {
		fmt.Fprintf(w, "usage: %s\n", msg)
	}
	fmt.Fprintln(w, usageText)
}

// parseMinSize accepts only ASCII digits; values too large for int64 are
// clamped (no file can be that large).
func parseMinSize(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 1<<63 - 1, true
	}
	return n, true
}

func parseArgs(args []string) (options, string) {
	opts := options{minSize: 1}
	var positional []string
	seenMin := false
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--":
			positional = append(positional, args[i+1:]...)
			i = len(args)
		case a == "--min-size" || strings.HasPrefix(a, "--min-size="):
			var v string
			if a == "--min-size" {
				if i+1 >= len(args) {
					return opts, "missing value for --min-size"
				}
				i++
				v = args[i]
			} else {
				v = strings.TrimPrefix(a, "--min-size=")
			}
			n, ok := parseMinSize(v)
			if !ok {
				return opts, "invalid --min-size value: " + v
			}
			opts.minSize = n
			seenMin = true
		case strings.HasPrefix(a, "-") && a != "-":
			return opts, "unknown option: " + a
		default:
			positional = append(positional, a)
		}
	}
	_ = seenMin
	if len(positional) != 1 {
		return opts, "expected exactly one DIR"
	}
	opts.dir = positional[0]
	return opts, ""
}

type candidate struct {
	rel  string // path relative to DIR
	full string
	size int64
}

type groupKey struct {
	sum  [sha256.Size]byte
	size int64
}

type group struct {
	key   groupKey
	paths []string
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

func run(args []string, stdout, stderr io.Writer) int {
	opts, msg := parseArgs(args)
	if msg != "" {
		usageErr(stderr, msg)
		return 64
	}
	st, err := os.Stat(opts.dir)
	if err != nil || !st.IsDir() {
		fmt.Fprintf(stderr, "error: not a directory: %s\n", opts.dir)
		return 2
	}

	failed := false
	fail := func(rel string) {
		failed = true
		fmt.Fprintf(stderr, "error: cannot read %s\n", rel)
	}

	// Walk, collecting regular files; bucket by size so only files that
	// share a size are ever hashed.
	bySize := map[int64][]candidate{}
	root := strings.TrimRight(opts.dir, "/")
	if root == "" {
		root = "/"
	}
	var walk func(full, rel string)
	walk = func(full, rel string) {
		entries, err := os.ReadDir(full)
		if err != nil {
			if rel == "" {
				fail(".")
			} else {
				fail(rel)
			}
			return
		}
		for _, e := range entries {
			name := e.Name()
			r := name
			if rel != "" {
				r = rel + "/" + name
			}
			f := full + "/" + name
			if full == "/" {
				f = "/" + name
			}
			mode := e.Type()
			switch {
			case mode&fs.ModeSymlink != 0:
				continue
			case mode.IsDir():
				walk(f, r)
			case mode.IsRegular():
				info, err := e.Info()
				if err != nil {
					fail(r)
					continue
				}
				if info.Size() < opts.minSize {
					continue
				}
				bySize[info.Size()] = append(bySize[info.Size()], candidate{r, f, info.Size()})
			}
		}
	}
	walk(root, "")

	groups := map[groupKey]*group{}
	for _, cands := range bySize {
		if len(cands) < 2 {
			continue
		}
		for _, c := range cands {
			sum, n, err := hashFile(c.full)
			if err != nil {
				fail(c.rel)
				continue
			}
			k := groupKey{sum, n}
			g := groups[k]
			if g == nil {
				g = &group{key: k}
				groups[k] = g
			}
			g.paths = append(g.paths, c.rel)
		}
	}

	var out []*group
	for _, g := range groups {
		if len(g.paths) >= 2 {
			sort.Strings(g.paths)
			out = append(out, g)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].key.size != out[j].key.size {
			return out[i].key.size > out[j].key.size
		}
		return hex.EncodeToString(out[i].key.sum[:]) < hex.EncodeToString(out[j].key.sum[:])
	})

	w := bufio.NewWriter(stdout)
	var files, wasted int64
	for _, g := range out {
		fmt.Fprintf(w, "sha256:%s size %d\n", hex.EncodeToString(g.key.sum[:]), g.key.size)
		for _, p := range g.paths {
			fmt.Fprintf(w, "  %s\n", p)
		}
		fmt.Fprintln(w)
		files += int64(len(g.paths))
		wasted += g.key.size * int64(len(g.paths)-1)
	}
	fmt.Fprintf(w, "groups %d files %d wasted %d\n", len(out), files, wasted)
	w.Flush()
	if failed {
		return 1
	}
	return 0
}

func hashFile(path string) ([sha256.Size]byte, int64, error) {
	var sum [sha256.Size]byte
	f, err := os.Open(path)
	if err != nil {
		return sum, 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return sum, 0, err
	}
	copy(sum[:], h.Sum(nil))
	return sum, n, nil
}
