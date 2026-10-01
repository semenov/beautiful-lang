package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strconv"
	"strings"
)

type Product struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Category   string `json:"category"`
	PriceCents int64  `json:"price_cents"`
	InStock    bool   `json:"in_stock"`
}

func fail(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "error: "+format+"\n", args...)
	os.Exit(1)
}

func validID(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '-', c == '_':
		default:
			return false
		}
	}
	return true
}

func decodeString(raw json.RawMessage, field string) (string, error) {
	var s string
	if len(raw) == 0 || raw[0] != '"' || json.Unmarshal(raw, &s) != nil {
		return "", fmt.Errorf("%s must be a string", field)
	}
	return s, nil
}

func decodePrice(raw json.RawMessage) (int64, error) {
	bad := errors.New("price_cents must be a whole number >= 0")
	if len(raw) == 0 || !(raw[0] == '-' || (raw[0] >= '0' && raw[0] <= '9')) {
		return 0, bad
	}
	s := string(raw)
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		if n < 0 {
			return 0, bad
		}
		return n, nil
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil || math.IsInf(f, 0) || f != math.Trunc(f) || f < 0 || f >= 9.2e18 {
		return 0, bad
	}
	return int64(f), nil
}

func parseProduct(raw json.RawMessage) (Product, error) {
	var p Product
	var obj map[string]json.RawMessage
	if len(raw) == 0 || raw[0] != '{' || json.Unmarshal(raw, &obj) != nil {
		return p, errors.New("product must be an object")
	}
	need := func(k string) (json.RawMessage, error) {
		v, ok := obj[k]
		if !ok {
			return nil, fmt.Errorf("missing field %s", k)
		}
		return v, nil
	}
	var err error
	var v json.RawMessage
	if v, err = need("id"); err != nil {
		return p, err
	}
	if p.ID, err = decodeString(v, "id"); err != nil {
		return p, err
	}
	if !validID(p.ID) {
		return p, fmt.Errorf("invalid id %q", p.ID)
	}
	if v, err = need("name"); err != nil {
		return p, err
	}
	if p.Name, err = decodeString(v, "name"); err != nil {
		return p, err
	}
	if v, err = need("category"); err != nil {
		return p, err
	}
	if p.Category, err = decodeString(v, "category"); err != nil {
		return p, err
	}
	if v, err = need("price_cents"); err != nil {
		return p, err
	}
	if p.PriceCents, err = decodePrice(v); err != nil {
		return p, err
	}
	if v, err = need("in_stock"); err != nil {
		return p, err
	}
	switch string(v) {
	case "true":
		p.InStock = true
	case "false":
	default:
		return p, errors.New("in_stock must be a boolean")
	}
	return p, nil
}

func loadCatalog(path string) ([]Product, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var raws []json.RawMessage
	if err := json.Unmarshal(data, &raws); err != nil {
		return nil, fmt.Errorf("invalid catalog JSON: %v", err)
	}
	seen := map[string]bool{}
	out := make([]Product, 0, len(raws))
	for i, r := range raws {
		p, err := parseProduct(r)
		if err != nil {
			return nil, fmt.Errorf("product %d: %v", i, err)
		}
		if seen[p.ID] {
			return nil, fmt.Errorf("product %d: duplicate id %q", i, p.ID)
		}
		seen[p.ID] = true
		out = append(out, p)
	}
	return out, nil
}

// Server holds the immutable catalog, pre-sorted once per sort order.
type Server struct {
	byID   map[string]*Product
	sorted map[string][]*Product
}

func less(sortName string) func(a, b *Product) bool {
	switch sortName {
	case "price_asc":
		return func(a, b *Product) bool {
			if a.PriceCents != b.PriceCents {
				return a.PriceCents < b.PriceCents
			}
			return a.ID < b.ID
		}
	case "price_desc":
		return func(a, b *Product) bool {
			if a.PriceCents != b.PriceCents {
				return a.PriceCents > b.PriceCents
			}
			return a.ID < b.ID
		}
	case "name":
		return func(a, b *Product) bool {
			if a.Name != b.Name {
				return a.Name < b.Name // byte order of UTF-8 == code point order
			}
			return a.ID < b.ID
		}
	}
	return func(a, b *Product) bool { return a.ID < b.ID }
}

func NewServer(products []Product) *Server {
	s := &Server{byID: map[string]*Product{}, sorted: map[string][]*Product{}}
	base := make([]*Product, len(products))
	for i := range products {
		base[i] = &products[i]
		s.byID[products[i].ID] = &products[i]
	}
	for _, name := range []string{"id", "price_asc", "price_desc", "name"} {
		l := append([]*Product(nil), base...)
		lt := less(name)
		sort.Slice(l, func(i, j int) bool { return lt(l[i], l[j]) })
		s.sorted[name] = l
	}
	return s
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.Encode(v)
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	w.Write(buf.Bytes())
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// parseCount parses decimal digits only; values too large to fit saturate.
func parseCount(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return math.MaxInt64, true
	}
	return n, true
}

type cursorData struct {
	F string `json:"f"` // fingerprint of the filters and sort
	ID string `json:"id"`
}

func fingerprint(cat string, hasCat bool, min, max string, stock, sortName string) string {
	b, _ := json.Marshal([]any{hasCat, cat, min, max, stock, sortName})
	return string(b)
}

func (s *Server) handleProducts(w http.ResponseWriter, r *http.Request) {
	// Lenient parse: first occurrence of each parameter wins; undecodable pairs are skipped.
	q := map[string]string{}
	for _, pair := range strings.Split(r.URL.RawQuery, "&") {
		if pair == "" {
			continue
		}
		k, v, _ := strings.Cut(pair, "=")
		k, err1 := url.QueryUnescape(k)
		v, err2 := url.QueryUnescape(v)
		if err1 != nil || err2 != nil {
			continue
		}
		if _, ok := q[k]; !ok {
			q[k] = v
		}
	}
	bad := func(name string) { writeErr(w, 400, "invalid parameter: "+name) }

	category, hasCat := q["category"]

	var minP, maxP int64
	var hasMin, hasMax bool
	if v, ok := q["min_price"]; ok {
		n, good := parseCount(v)
		if !good {
			bad("min_price")
			return
		}
		minP, hasMin = n, true
	}
	if v, ok := q["max_price"]; ok {
		n, good := parseCount(v)
		if !good {
			bad("max_price")
			return
		}
		maxP, hasMax = n, true
	}
	if hasMin && hasMax && minP > maxP {
		bad("max_price")
		return
	}
	stockVal, hasStock := q["in_stock"]
	var stock bool
	if hasStock {
		switch stockVal {
		case "true":
			stock = true
		case "false":
		default:
			bad("in_stock")
			return
		}
	}
	sortName := "id"
	if v, ok := q["sort"]; ok {
		switch v {
		case "id", "price_asc", "price_desc", "name":
			sortName = v
		default:
			bad("sort")
			return
		}
	}
	limit := int64(20)
	if v, ok := q["limit"]; ok {
		n, good := parseCount(v)
		if !good || n < 1 || n > 100 {
			bad("limit")
			return
		}
		limit = n
	}
	fp := fingerprint(category, hasCat, q["min_price"], q["max_price"], stockVal, sortName)
	// Normalize numeric strings so "05" and "5" give the same fingerprint.
	if hasMin {
		fp = fingerprint(category, hasCat, strconv.FormatInt(minP, 10), q["max_price"], stockVal, sortName)
	}
	if hasMax {
		mn := q["min_price"]
		if hasMin {
			mn = strconv.FormatInt(minP, 10)
		}
		fp = fingerprint(category, hasCat, mn, strconv.FormatInt(maxP, 10), stockVal, sortName)
	}

	list := s.sorted[sortName]
	start := 0
	if cv, ok := q["cursor"]; ok {
		raw, err := base64.RawURLEncoding.DecodeString(cv)
		var cd cursorData
		if err != nil || json.Unmarshal(raw, &cd) != nil || cd.F != fp {
			bad("cursor")
			return
		}
		last, found := s.byID[cd.ID]
		if !found {
			bad("cursor")
			return
		}
		lt := less(sortName)
		// First position whose element sorts strictly after the last one returned.
		start = sort.Search(len(list), func(i int) bool { return lt(last, list[i]) })
	}

	items := make([]*Product, 0, limit)
	more := false
	for _, p := range list[start:] {
		if hasCat && p.Category != category {
			continue
		}
		if hasMin && p.PriceCents < minP {
			continue
		}
		if hasMax && p.PriceCents > maxP {
			continue
		}
		if hasStock && p.InStock != stock {
			continue
		}
		if int64(len(items)) == limit {
			more = true
			break
		}
		items = append(items, p)
	}
	var next any
	if more {
		raw, _ := json.Marshal(cursorData{F: fp, ID: items[len(items)-1].ID})
		next = base64.RawURLEncoding.EncodeToString(raw)
	}
	writeJSON(w, 200, map[string]any{"items": items, "next_cursor": next})
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	var id string
	switch {
	case path == "/products":
	case strings.HasPrefix(path, "/products/") && len(path) > len("/products/"):
		id = path[len("/products/"):]
	default:
		writeErr(w, 404, "not found")
		return
	}
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", "GET")
		writeErr(w, 405, "method not allowed")
		return
	}
	if id == "" {
		s.handleProducts(w, r)
		return
	}
	if p, ok := s.byID[id]; ok {
		writeJSON(w, 200, p)
		return
	}
	writeErr(w, 404, "not found")
}

func main() {
	path := os.Getenv("CATALOG_PATH")
	if path == "" {
		fail("CATALOG_PATH is not set")
	}
	products, err := loadCatalog(path)
	if err != nil {
		fail("%v", err)
	}
	port := os.Getenv("PORT")
	if n, err := strconv.Atoi(port); err != nil || n < 0 || n > 65535 {
		fail("invalid PORT %q", port)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		fail("%v", err)
	}
	if err := http.Serve(ln, NewServer(products)); err != nil {
		fail("%v", err)
	}
}
