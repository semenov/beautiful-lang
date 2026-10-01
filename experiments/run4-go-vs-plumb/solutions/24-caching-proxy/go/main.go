package main

import (
	"container/list"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// result is one answer to a key: what upstream said, or a proxy-made error.
type result struct {
	status      int
	contentType string
	hasCT       bool
	body        []byte
}

type entry struct {
	key     string
	res     *result
	expires time.Time
}

type call struct {
	done chan struct{}
	res  *result
}

type proxy struct {
	scheme, host, prefix string
	ttl                  time.Duration
	max                  int
	client               *http.Client
	timeout              time.Duration

	mu       sync.Mutex
	lru      *list.List // front = most recently used; values are *entry
	items    map[string]*list.Element
	inflight map[string]*call
}

func jsonErr(status int, body string) *result {
	return &result{status: status, contentType: "application/json", hasCT: true, body: []byte(body)}
}

// fetch asks the upstream. The path and query are sent exactly as received.
func (p *proxy) fetch(requestURI string) (res *result, cacheable bool) {
	path, query, hasQuery := strings.Cut(requestURI, "?")
	u := &url.URL{Scheme: p.scheme, Host: p.host, Opaque: p.prefix + path}
	if hasQuery {
		u.RawQuery = query
		u.ForceQuery = true
	}
	ctx, cancel := context.WithTimeout(context.Background(), p.timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", "http://x/", nil)
	if err != nil {
		return jsonErr(502, `{"error": "upstream unavailable"}`), false
	}
	req.URL = u
	resp, err := p.client.Do(req)
	if err == nil {
		defer resp.Body.Close()
		var body []byte
		body, err = io.ReadAll(resp.Body)
		if err == nil {
			switch {
			case resp.StatusCode >= 500:
				return jsonErr(502, fmt.Sprintf(`{"error": "upstream error", "status": %d}`, resp.StatusCode)), false
			default:
				_, hasCT := resp.Header["Content-Type"]
				return &result{status: resp.StatusCode, contentType: resp.Header.Get("Content-Type"), hasCT: hasCT, body: body},
					resp.StatusCode == 200
			}
		}
	}
	var ne net.Error
	if errors.Is(err, context.DeadlineExceeded) || ctx.Err() != nil || (errors.As(err, &ne) && ne.Timeout()) {
		return jsonErr(504, `{"error": "upstream timeout"}`), false
	}
	return jsonErr(502, `{"error": "upstream unavailable"}`), false
}

// get returns the answer for key and whether this request caused the upstream request.
func (p *proxy) get(key string) (*result, bool) {
	p.mu.Lock()
	if el, ok := p.items[key]; ok {
		e := el.Value.(*entry)
		if time.Now().Before(e.expires) {
			p.lru.MoveToFront(el)
			p.mu.Unlock()
			return e.res, false
		}
		p.lru.Remove(el)
		delete(p.items, key)
	}
	if c, ok := p.inflight[key]; ok {
		p.mu.Unlock()
		<-c.done
		return c.res, false
	}
	c := &call{done: make(chan struct{})}
	p.inflight[key] = c
	p.mu.Unlock()

	res, cacheable := p.fetch(key)

	p.mu.Lock()
	if cacheable {
		for p.lru.Len() >= p.max {
			old := p.lru.Back()
			p.lru.Remove(old)
			delete(p.items, old.Value.(*entry).key)
		}
		p.items[key] = p.lru.PushFront(&entry{key: key, res: res, expires: time.Now().Add(p.ttl)})
	}
	delete(p.inflight, key)
	c.res = res
	close(c.done)
	p.mu.Unlock()
	return res, true
}

func (p *proxy) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		write(w, jsonErr(405, `{"error": "method not allowed"}`), "")
		return
	}
	uri := r.RequestURI
	if !strings.HasPrefix(uri, "/") {
		// absolute-form request target: keep only path and query
		if u, err := url.ParseRequestURI(uri); err == nil {
			uri = u.RequestURI()
		}
	}
	res, miss := p.get(uri)
	xc := "HIT"
	if miss {
		xc = "MISS"
	}
	write(w, res, xc)
}

func write(w http.ResponseWriter, res *result, xcache string) {
	h := w.Header()
	if res.hasCT {
		h.Set("Content-Type", res.contentType)
	} else {
		h["Content-Type"] = nil // no sniffing
	}
	if xcache != "" {
		h.Set("X-Cache", xcache)
	}
	h.Set("Content-Length", strconv.Itoa(len(res.body)))
	w.WriteHeader(res.status)
	w.Write(res.body)
}

func envInt(name string, def int) int {
	s, ok := os.LookupEnv(name)
	if !ok || s == "" {
		return def
	}
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 {
		fmt.Fprintf(os.Stderr, "%s must be a whole number >= 1\n", name)
		os.Exit(2)
	}
	return n
}

func main() {
	port := envInt("PORT", 0)
	if os.Getenv("PORT") == "" {
		fmt.Fprintln(os.Stderr, "PORT is required")
		os.Exit(2)
	}
	up := os.Getenv("UPSTREAM")
	if up == "" {
		fmt.Fprintln(os.Stderr, "UPSTREAM is required")
		os.Exit(2)
	}
	u, err := url.Parse(up)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		fmt.Fprintln(os.Stderr, "UPSTREAM must be http://host:port[/prefix]")
		os.Exit(2)
	}
	tr := &http.Transport{DisableCompression: true, MaxIdleConnsPerHost: 64}
	p := &proxy{
		scheme: u.Scheme, host: u.Host, prefix: strings.TrimRight(u.EscapedPath(), "/"),
		ttl:     time.Duration(envInt("CACHE_TTL_MS", 60000)) * time.Millisecond,
		max:     envInt("CACHE_MAX_ENTRIES", 1000),
		timeout: time.Duration(envInt("UPSTREAM_TIMEOUT_MS", 5000)) * time.Millisecond,
		client: &http.Client{Transport: tr,
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }},
		lru: list.New(), items: map[string]*list.Element{}, inflight: map[string]*call{},
	}
	srv := &http.Server{Addr: "127.0.0.1:" + strconv.Itoa(port), Handler: p}
	if err := srv.ListenAndServe(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
