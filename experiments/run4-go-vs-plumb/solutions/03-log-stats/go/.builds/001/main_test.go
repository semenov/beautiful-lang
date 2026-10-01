package main

import (
	"bytes"
	"strings"
	"testing"
)

func runStr(in string, args ...string) (string, string, int) {
	var o, e bytes.Buffer
	c := run(args, strings.NewReader(in), &o, &e)
	return o.String(), e.String(), c
}

func TestExample(t *testing.T) {
	in := `1.2.3.4 - - [10/Oct/2024:13:55:36 +0000] "GET /api/users?id=3 HTTP/1.1" 200 512 40
1.2.3.4 - - [10/Oct/2024:13:55:37 +0000] "GET /api/users HTTP/1.1" 503 - 120
1.2.3.4 - - [10/Oct/2024:13:55:38 +0000] "POST /api/login HTTP/1.1" 200 64 15
garbage
`
	want := "GET /api/users count=2 error_rate=50.00% p50=40 p95=120 max=120\n" +
		"POST /api/login count=1 error_rate=0.00% p50=15 p95=15 max=15\n" +
		"total=3 malformed=1\n"
	if out, _, c := runStr(in); out != want || c != 0 {
		t.Fatalf("got %q %d", out, c)
	}
	out, _, _ := runStr(in, "--min-count", "2")
	if !strings.HasSuffix(out, "total=3 malformed=1\n") || strings.Contains(out, "POST") {
		t.Fatalf("min-count: %q", out)
	}
}

func TestErrorRate(t *testing.T) {
	for _, c := range []struct{ e, n uint64; w string }{
		{1, 800, "0.13%"}, {0, 5, "0.00%"}, {5, 5, "100.00%"}, {1, 3, "33.33%"}, {2, 3, "66.67%"}, {1, 16, "6.25%"}, {1, 2000, "0.05%"}, {1, 8000, "0.01%"}, {1, 12000, "0.01%"}, {1, 20001, "0.00%"},
	} {
		if g := errorRate(c.e, c.n); g != c.w {
			t.Errorf("%d/%d: %s want %s", c.e, c.n, g, c.w)
		}
	}
}

func TestPercentile(t *testing.T) {
	var v []uint64
	for i := uint64(1); i <= 20; i++ {
		v = append(v, i)
	}
	if percentile(v, 95) != 19 || percentile(v, 50) != 10 || percentile(v[:1], 95) != 1 {
		t.Fatal("percentile")
	}
}

func TestMalformed(t *testing.T) {
	good := `a - - [t t] "GET /x HTTP/1.1" 200 - 007`
	bad := []string{
		`a - - [t] "get /x HTTP/1.1" 200 1 1`,
		`a - - [t] "GET /x HTTP/1.1" 600 1 1`,
		`a - - [t] "GET /x HTTP/1.1" 099 1 1`,
		`a - - [t] "GET /x HTTP/1.1" 200 1 1 `,
		` a - - [t] "GET /x HTTP/1.1" 200 1 1`,
		`a - - [t]  "GET /x HTTP/1.1" 200 1 1`,
		`a - - [t] "GET /x HTTP/1.1" 200 1x 1`,
		`a - - [t] "GET /x HTTP/1.1" 200 1 -`,
		`a - - [t] "GET /x HTTP/ 1" 200 1 1`,
		`a - - [t] "GET /x" 200 1 1`,
		`a - - [t] "GET /x HTTP/1.1" 200 1`,
	}
	out, _, c := runStr(good + "\r\n\r\n\n" + strings.Join(bad, "\n") + "\n")
	want := "GET /x count=1 error_rate=0.00% p50=7 p95=7 max=7\ntotal=1 malformed=11\n"
	if out != want || c != 0 {
		t.Fatalf("got %q", out)
	}
}

func TestArgs(t *testing.T) {
	for _, a := range [][]string{{"--min-count"}, {"--min-count", "0"}, {"--min-count", "x"}, {"--min-count", "-1"}, {"--min-count", "1.5"}, {"--bogus"}} {
		if o, e, c := runStr("", a...); c != 64 || o != "" || !strings.HasPrefix(e, "usage:") {
			t.Errorf("%v: %q %q %d", a, o, e, c)
		}
	}
	if o, e, c := runStr("", "/nonexistent/f"); c != 2 || o != "" || e != "error: cannot read /nonexistent/f\n" {
		t.Errorf("%q %q %d", o, e, c)
	}
}
