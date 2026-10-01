// log-stats summarizes web server access logs per endpoint.
package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
)

const usageText = "usage: app [--min-count N] [FILE...]"

type stats struct {
	errors    int64
	latencies []uint64
}

type log struct {
	endpoints map[string]*stats
	total     int64
	malformed int64
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdin, os.Stdout, os.Stderr))
}

func run(args []string, stdin io.Reader, stdout, stderr io.Writer) int {
	minCount, files, err := parseArgs(args)
	if err != nil {
		fmt.Fprintf(stderr, "%s\nusage: %v\n", usageText, err)
		return 64
	}

	lg := &log{endpoints: map[string]*stats{}}
	if len(files) == 0 {
		if err := lg.read(stdin); err != nil {
			fmt.Fprintln(stderr, "error: cannot read standard input")
			return 2
		}
	}
	for _, name := range files {
		f, err := os.Open(name)
		if err == nil {
			err = lg.read(f)
			f.Close()
		}
		if err != nil {
			fmt.Fprintf(stderr, "error: cannot read %s\n", name)
			return 2
		}
	}

	w := bufio.NewWriter(stdout)
	lg.report(w, minCount)
	if err := w.Flush(); err != nil {
		return 1
	}
	return 0
}

// parseArgs returns the --min-count value and the file operands.
func parseArgs(args []string) (minCount uint64, files []string, err error) {
	minCount = 1
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--":
			files = append(files, args[i+1:]...)
			return
		case a == "--min-count":
			if i+1 >= len(args) {
				return 0, nil, fmt.Errorf("--min-count needs a value")
			}
			i++
			if minCount, err = parseMin(args[i]); err != nil {
				return 0, nil, err
			}
		case len(a) > len("--min-count=") && a[:len("--min-count=")] == "--min-count=":
			if minCount, err = parseMin(a[len("--min-count="):]); err != nil {
				return 0, nil, err
			}
		case a == "--min-count=":
			return 0, nil, fmt.Errorf("--min-count needs a value")
		case len(a) > 1 && a[0] == '-':
			return 0, nil, fmt.Errorf("unknown option %s", a)
		default:
			files = append(files, a)
		}
	}
	return
}

// parseMin parses a whole number >= 1; values too large to matter saturate.
func parseMin(s string) (uint64, error) {
	if s == "" {
		return 0, fmt.Errorf("--min-count needs a whole number >= 1")
	}
	var n uint64
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, fmt.Errorf("--min-count needs a whole number >= 1, got %q", s)
		}
		if n < 1<<60 {
			n = n*10 + uint64(c-'0')
		}
	}
	if n < 1 {
		return 0, fmt.Errorf("--min-count needs a whole number >= 1, got %q", s)
	}
	return n, nil
}

func (lg *log) read(r io.Reader) error {
	br := bufio.NewReaderSize(r, 1<<16)
	for {
		line, err := br.ReadBytes('\n')
		if len(line) > 0 {
			line = bytes.TrimSuffix(line, []byte("\n"))
			line = bytes.TrimSuffix(line, []byte("\r"))
			lg.add(line)
		}
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
	}
}

func (lg *log) add(line []byte) {
	if len(line) == 0 {
		return
	}
	endpoint, status, latency, ok := parseLine(line)
	if !ok {
		lg.malformed++
		return
	}
	lg.total++
	st := lg.endpoints[endpoint]
	if st == nil {
		st = &stats{}
		lg.endpoints[endpoint] = st
	}
	if status >= 500 {
		st.errors++
	}
	st.latencies = append(st.latencies, latency)
}

// scanner walks a line left to right.
type scanner struct {
	s   []byte
	pos int
}

// token consumes one or more non-space bytes for which ok holds.
func (sc *scanner) token(ok func(byte) bool) []byte {
	start := sc.pos
	for sc.pos < len(sc.s) && sc.s[sc.pos] != ' ' && ok(sc.s[sc.pos]) {
		sc.pos++
	}
	if sc.pos == start {
		return nil
	}
	return sc.s[start:sc.pos]
}

func (sc *scanner) lit(lit string) bool {
	if len(sc.s)-sc.pos >= len(lit) && string(sc.s[sc.pos:sc.pos+len(lit)]) == lit {
		sc.pos += len(lit)
		return true
	}
	return false
}

func (sc *scanner) done() bool { return sc.pos == len(sc.s) }

func any(byte) bool       { return true }
func isDigit(c byte) bool { return c >= '0' && c <= '9' }
func isUpper(c byte) bool { return c >= 'A' && c <= 'Z' }

func parseLine(line []byte) (endpoint string, status int, latency uint64, ok bool) {
	sc := &scanner{s: line}
	// CLIENT IDENT USER
	for i := 0; i < 3; i++ {
		if sc.token(any) == nil || !sc.lit(" ") {
			return
		}
	}
	// [TIME]
	if !sc.lit("[") {
		return
	}
	end := bytes.IndexByte(sc.s[sc.pos:], ']')
	if end < 0 {
		return
	}
	sc.pos += end + 1
	if !sc.lit(" \"") {
		return
	}
	// "METHOD TARGET PROTOCOL"
	method := sc.token(isUpper)
	if method == nil || !sc.lit(" ") {
		return
	}
	target := sc.token(func(c byte) bool { return c != '"' })
	if target == nil || !sc.lit(" HTTP/") {
		return
	}
	if sc.token(func(c byte) bool { return isDigit(c) || c == '.' }) == nil || !sc.lit("\" ") {
		return
	}
	// STATUS
	st := sc.token(isDigit)
	if len(st) != 3 || !sc.lit(" ") {
		return
	}
	status = int(st[0]-'0')*100 + int(st[1]-'0')*10 + int(st[2]-'0')
	if status < 100 || status > 599 {
		return
	}
	// BYTES
	b := sc.token(func(c byte) bool { return isDigit(c) || c == '-' })
	if b == nil || !sc.lit(" ") {
		return
	}
	if len(b) == 1 && b[0] == '-' {
		// ok
	} else {
		for _, c := range b {
			if !isDigit(c) {
				return
			}
		}
	}
	// LATENCY
	lat := sc.token(isDigit)
	if lat == nil || !sc.done() {
		return
	}
	for _, c := range lat {
		latency = latency*10 + uint64(c-'0')
	}

	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	endpoint = string(method) + " " + string(target)
	return endpoint, status, latency, true
}

type row struct {
	name string
	st   *stats
}

func (lg *log) report(w *bufio.Writer, minCount uint64) {
	var rows []row
	for name, st := range lg.endpoints {
		if uint64(len(st.latencies)) >= minCount {
			rows = append(rows, row{name, st})
		}
	}
	sort.Slice(rows, func(i, j int) bool {
		ci, cj := len(rows[i].st.latencies), len(rows[j].st.latencies)
		if ci != cj {
			return ci > cj
		}
		return rows[i].name < rows[j].name
	})
	for _, r := range rows {
		lat := r.st.latencies
		sort.Slice(lat, func(i, j int) bool { return lat[i] < lat[j] })
		n := uint64(len(lat))
		fmt.Fprintf(w, "%s count=%d error_rate=%s p50=%d p95=%d max=%d\n",
			r.name, n, errorRate(uint64(r.st.errors), n),
			percentile(lat, 50), percentile(lat, 95), lat[n-1])
	}
	fmt.Fprintf(w, "total=%d malformed=%d\n", lg.total, lg.malformed)
}

// percentile is the nearest-rank percentile of sorted values.
func percentile(sorted []uint64, p uint64) uint64 {
	n := uint64(len(sorted))
	rank := (p*n + 99) / 100
	if rank < 1 {
		rank = 1
	}
	return sorted[rank-1]
}

// errorRate formats 100*errors/count with two decimals, halves rounded up.
func errorRate(errors, count uint64) string {
	// hundredths = floor(10000*errors/count + 1/2)
	h := (20000*errors + count) / (2 * count)
	return strconv.FormatUint(h/100, 10) + "." + fmt.Sprintf("%02d", h%100) + "%"
}
