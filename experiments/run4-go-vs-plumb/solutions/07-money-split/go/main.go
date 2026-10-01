package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

type part struct {
	name   string
	shares int64
}

type expense struct {
	payer string
	cents int64
	split []part
}

func isBlank(c byte) bool { return c == ' ' || c == '\t' }

func validName(s string) bool {
	return s != "" && !strings.ContainsAny(s, " \t,:#")
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

// parseAmount parses AMOUNT into cents.
func parseAmount(s string) (int64, error) {
	whole, frac := s, ""
	if i := strings.IndexByte(s, '.'); i >= 0 {
		whole, frac = s[:i], s[i+1:]
		if len(frac) < 1 || len(frac) > 2 || !allDigits(frac) {
			return 0, fmt.Errorf("invalid amount %q", s)
		}
	}
	if !allDigits(whole) {
		return 0, fmt.Errorf("invalid amount %q", s)
	}
	w := strings.TrimLeft(whole, "0")
	if len(w) > 10 {
		return 0, fmt.Errorf("amount out of range %q", s)
	}
	var cents int64
	for i := 0; i < len(w); i++ {
		cents = cents*10 + int64(w[i]-'0')
	}
	cents *= 100
	if len(frac) == 1 {
		frac += "0"
	}
	if frac != "" {
		cents += int64(frac[0]-'0')*10 + int64(frac[1]-'0')
	}
	if cents <= 0 {
		return 0, fmt.Errorf("amount must be more than 0 %q", s)
	}
	if cents > 100000000000 {
		return 0, fmt.Errorf("amount out of range %q", s)
	}
	return cents, nil
}

func parseSplit(s string) ([]part, error) {
	seen := map[string]bool{}
	var parts []part
	for _, item := range strings.Split(s, ",") {
		name, sh := item, ""
		hasShares := false
		if i := strings.IndexByte(item, ':'); i >= 0 {
			name, sh = item[:i], item[i+1:]
			hasShares = true
		}
		if !validName(name) {
			return nil, fmt.Errorf("invalid participant name in %q", item)
		}
		var n int64 = 1
		if hasShares {
			if !allDigits(sh) {
				return nil, fmt.Errorf("invalid shares in %q", item)
			}
			t := strings.TrimLeft(sh, "0")
			if len(t) > 4 {
				return nil, fmt.Errorf("shares out of range in %q", item)
			}
			n = 0
			for i := 0; i < len(t); i++ {
				n = n*10 + int64(t[i]-'0')
			}
			if n < 1 || n > 1000 {
				return nil, fmt.Errorf("shares out of range in %q", item)
			}
		}
		if seen[name] {
			return nil, fmt.Errorf("duplicate participant %q", name)
		}
		seen[name] = true
		parts = append(parts, part{name, n})
	}
	return parts, nil
}

func parseLine(line string) (*expense, error) {
	fields := strings.FieldsFunc(line, func(r rune) bool { return r == ' ' || r == '\t' })
	if len(fields) == 0 || strings.HasPrefix(fields[0], "#") {
		return nil, nil
	}
	if len(fields) != 3 {
		return nil, fmt.Errorf("expected 3 fields (PAYER AMOUNT SPLIT), got %d", len(fields))
	}
	if !validName(fields[0]) {
		return nil, fmt.Errorf("invalid payer name %q", fields[0])
	}
	cents, err := parseAmount(fields[1])
	if err != nil {
		return nil, err
	}
	split, err := parseSplit(fields[2])
	if err != nil {
		return nil, err
	}
	return &expense{fields[0], cents, split}, nil
}

func parse(data []byte) ([]*expense, error) {
	var out []*expense
	for i, raw := range bytes.Split(data, []byte("\n")) {
		line := strings.TrimSuffix(string(raw), "\r")
		e, err := parseLine(line)
		if err != nil {
			return nil, fmt.Errorf("line %d: %v", i+1, err)
		}
		if e != nil {
			out = append(out, e)
		}
	}
	return out, nil
}

func balances(exps []*expense) map[string]int64 {
	bal := map[string]int64{}
	for _, e := range exps {
		bal[e.payer] += e.cents
		var total int64
		for _, p := range e.split {
			total += p.shares
		}
		shares := make([]int64, len(e.split))
		var sum int64
		for i, p := range e.split {
			shares[i] = e.cents * p.shares / total
			sum += shares[i]
		}
		for i := int64(0); i < e.cents-sum; i++ {
			shares[i]++
		}
		for i, p := range e.split {
			bal[p.name] -= shares[i]
		}
	}
	return bal
}

func fmtCents(c int64, sign bool) string {
	s := ""
	switch {
	case c < 0:
		s, c = "-", -c
	case c > 0 && sign:
		s = "+"
	}
	return fmt.Sprintf("%s%d.%02d", s, c/100, c%100)
}

func render(bal map[string]int64) string {
	names := make([]string, 0, len(bal))
	for n := range bal {
		names = append(names, n)
	}
	sort.Strings(names) // byte order of UTF-8 == code point order
	var b strings.Builder
	b.WriteString("balances:\n")
	for _, n := range names {
		fmt.Fprintf(&b, "  %s %s\n", n, fmtCents(bal[n], true))
	}
	b.WriteString("transfers:\n")
	for {
		var debtor, creditor string
		for _, n := range names {
			v := bal[n]
			if v < 0 && (debtor == "" || v < bal[debtor]) {
				debtor = n
			}
			if v > 0 && (creditor == "" || v > bal[creditor]) {
				creditor = n
			}
		}
		if debtor == "" || creditor == "" {
			break
		}
		amt := bal[creditor]
		if -bal[debtor] < amt {
			amt = -bal[debtor]
		}
		bal[debtor] += amt
		bal[creditor] -= amt
		fmt.Fprintf(&b, "  %s -> %s %s\n", debtor, creditor, fmtCents(amt, false))
	}
	return b.String()
}

func main() {
	args := os.Args[1:]
	if len(args) > 1 || (len(args) == 1 && strings.HasPrefix(args[0], "-")) {
		fmt.Fprintln(os.Stderr, "usage: app [FILE]")
		os.Exit(64)
	}
	var data []byte
	var err error
	if len(args) == 1 {
		data, err = os.ReadFile(args[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", args[0])
			os.Exit(2)
		}
	} else {
		data, err = io.ReadAll(os.Stdin)
		if err != nil {
			fmt.Fprintln(os.Stderr, "error: cannot read standard input")
			os.Exit(2)
		}
	}
	exps, err := parse(data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
	os.Stdout.WriteString(render(balances(exps)))
}
