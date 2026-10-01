package main

import (
	"bytes"
	"strings"
	"testing"
)

func do(in string, args ...string) (string, string, int) {
	var o, e bytes.Buffer
	c := run(args, strings.NewReader(in), &o, &e)
	return o.String(), e.String(), c
}

func TestCases(t *testing.T) {
	tests := []struct {
		name, in, out, err string
		code               int
	}{
		{"example", "region,product,amount\nNorth,\"Widget, large\",10.50\nSouth,Gadget,3.25\nNorth,Gizmo,0.10\n", "region,total\nNorth,10.60\nSouth,3.25\n", "", 0},
		{"neg", "region,amount\na,1\na,-1.5\nb,-0.5\nc,1\nc,-1\n", "region,total\na,-0.50\nb,-0.50\nc,0.00\n", "", 0},
		{"bad", "region,amount\na,1.234\n\na,x,y\n\"b\"x,1\na,2\n\"q,1\n", "region,total\na,2.00\n", "line 2: invalid amount\nline 4: expected 2 fields, got 3\nline 5: unexpected character after closing quote\nline 7: unterminated quote\n", 1},
		{"multiline", "region,amount\n\"a\nb\",x\nc,1\r\n", "region,total\nc,1.00\n", "line 2: invalid amount\n", 1},
		{"crlf", "region,amount\r\n\r\n,1\r\n", "region,total\n,1.00\n", "", 0},
		{"spaces", "region,amount\na, 1 \na,+1\na,.5\na,5.\n", "region,total\na,1.00\n", "line 3: invalid amount\nline 4: invalid amount\nline 5: invalid amount\n", 1},
		{"quoteout", "region,amount\n\"a,\"\"b\",1", "region,total\n\"a,\"\"b\",1.00\n", "", 0},
		{"empty", "", "", "error: empty input\n", 2},
		{"missing", "region,amount\n", "", "error: missing column foo\n", 2},
	}
	for _, tc := range tests {
		by := "region"
		if tc.name == "missing" {
			by = "foo"
		}
		o, e, c := do(tc.in, "--by", by)
		if o != tc.out || e != tc.err || c != tc.code {
			t.Errorf("%s: got %q %q %d; want %q %q %d", tc.name, o, e, c, tc.out, tc.err, tc.code)
		}
	}
	if _, e, c := do("", "x"); c != 64 || !strings.HasPrefix(e, "usage:") {
		t.Error("usage")
	}
	if _, _, c := do("", "--by"); c != 64 {
		t.Error("by no value")
	}
	if _, _, c := do("", "--by", "a", "--x"); c != 64 {
		t.Error("unknown")
	}
}
