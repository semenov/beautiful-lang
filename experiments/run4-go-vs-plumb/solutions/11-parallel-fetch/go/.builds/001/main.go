// parallel-fetch: fetch a list of URLs concurrently and report the results.
package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	maxRedirects   = 10
	usageText      = "usage: app [--concurrency N] [--timeout SECONDS] [FILE]"
	exitUsage      = 64
	exitUnreadable = 2
)

type options struct {
	concurrency int
	timeout     time.Duration
	file        string
	hasFile     bool
}

type result struct {
	status  int   // valid when errKind == ""
	bytes   int64 // valid when errKind == ""
	errKind string
}

func main() {
	opts, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", usageText)
		fmt.Fprintf(os.Stderr, "  (%v)\n", err)
		os.Exit(exitUsage)
	}

	var data []byte
	if opts.hasFile {
		data, err = os.ReadFile(opts.file)
	} else {
		data, err = io.ReadAll(os.Stdin)
	}
	if err != nil {
		name := opts.file
		if !opts.hasFile {
			name = "standard input"
		}
		fmt.Fprintf(os.Stderr, "error: cannot read %s\n", name)
		os.Exit(exitUnreadable)
	}

	urls := parseURLs(string(data))
	results := fetchAll(urls, opts)
	os.Exit(report(os.Stdout, urls, results))
}

func parseArgs(args []string) (options, error) {
	opts := options{concurrency: 4, timeout: 10 * time.Second}
	// value returns the value of option name at args[i], either inline
	// (--name=V) or the following argument.
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--" {
			for _, rest := range args[i+1:] {
				if err := setFile(&opts, rest); err != nil {
					return opts, err
				}
			}
			break
		}
		if strings.HasPrefix(a, "--") {
			name, val, hasVal := strings.Cut(a, "=")
			if name != "--concurrency" && name != "--timeout" {
				return opts, fmt.Errorf("unknown option %s", name)
			}
			if !hasVal {
				if i+1 >= len(args) {
					return opts, fmt.Errorf("missing value for %s", name)
				}
				i++
				val = args[i]
			}
			if name == "--concurrency" {
				n, err := strconv.Atoi(val)
				if err != nil || n < 1 {
					return opts, fmt.Errorf("invalid value for --concurrency: %q", val)
				}
				opts.concurrency = n
			} else {
				f, err := strconv.ParseFloat(val, 64)
				if err != nil || math.IsNaN(f) || math.IsInf(f, 0) || f <= 0 {
					return opts, fmt.Errorf("invalid value for --timeout: %q", val)
				}
				opts.timeout = secondsToDuration(f)
			}
			continue
		}
		if len(a) > 1 && a[0] == '-' {
			return opts, fmt.Errorf("unknown option %s", a)
		}
		if err := setFile(&opts, a); err != nil {
			return opts, err
		}
	}
	return opts, nil
}

func setFile(o *options, f string) error {
	if o.hasFile {
		return errors.New("more than one FILE")
	}
	o.file, o.hasFile = f, true
	return nil
}

func secondsToDuration(s float64) time.Duration {
	ns := s * float64(time.Second)
	if ns >= float64(math.MaxInt64) {
		return time.Duration(math.MaxInt64)
	}
	d := time.Duration(ns)
	if d < 1 {
		d = 1
	}
	return d
}

// parseURLs returns one URL per meaningful line: trimmed, no blanks, no comments.
func parseURLs(text string) []string {
	var urls []string
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		urls = append(urls, line)
	}
	return urls
}

// fetchAll fetches every URL with at most opts.concurrency in flight,
// keeping the workers busy while URLs remain. Results are in input order.
func fetchAll(urls []string, opts options) []result {
	results := make([]result, len(urls))
	client := &http.Client{
		Transport: &http.Transport{
			Proxy:              nil,  // never consult proxy environment variables
			DisableCompression: true, // count the body bytes as sent
			DialContext:        (&net.Dialer{}).DialContext,
		},
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse // redirects are followed by hand
		},
	}

	workers := opts.concurrency
	if workers > len(urls) {
		workers = len(urls)
	}
	jobs := make(chan int)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				results[i] = fetch(client, urls[i], opts.timeout)
			}
		}()
	}
	for i := range urls {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	return results
}

func parseHTTPURL(raw string) (*url.URL, bool) {
	u, err := url.Parse(raw)
	if err != nil {
		return nil, false
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil, false
	}
	if u.Hostname() == "" {
		return nil, false
	}
	return u, true
}

func isRedirect(code int) bool {
	switch code {
	case 301, 302, 303, 307, 308:
		return true
	}
	return false
}

// fetch performs one GET, following redirects, under a single deadline that
// covers connection, redirects and reading the whole final body.
func fetch(client *http.Client, raw string, timeout time.Duration) result {
	u, ok := parseHTTPURL(raw)
	if !ok {
		return result{errKind: "invalid-url"}
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	fail := func(err error) result { return result{errKind: classify(ctx, err)} }

	for redirects := 0; ; redirects++ {
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
		if err != nil {
			return fail(err)
		}
		resp, err := client.Do(req)
		if err != nil {
			return fail(err)
		}
		if isRedirect(resp.StatusCode) {
			if loc := resp.Header.Get("Location"); loc != "" {
				resp.Body.Close()
				if redirects >= maxRedirects {
					return result{errKind: "redirects"}
				}
				next, err := resp.Request.URL.Parse(loc)
				if err != nil {
					return result{errKind: "other"}
				}
				if next.Scheme != "http" && next.Scheme != "https" || next.Hostname() == "" {
					return result{errKind: "other"}
				}
				u = next
				continue
			}
		}
		n, err := io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		if err != nil {
			return fail(err)
		}
		return result{status: resp.StatusCode, bytes: n}
	}
}

func classify(ctx context.Context, err error) string {
	if errors.Is(ctx.Err(), context.DeadlineExceeded) || errors.Is(err, context.DeadlineExceeded) {
		return "timeout"
	}
	var ne net.Error
	if errors.As(err, &ne) && ne.Timeout() {
		return "timeout"
	}
	var oe *net.OpError
	if errors.As(err, &oe) && oe.Op == "dial" {
		return "connect"
	}
	return "other"
}

// report prints the per-URL lines and the summary; it returns the exit code.
func report(w io.Writer, urls []string, results []result) int {
	var ok, bad, failed int
	var sb strings.Builder
	for i, u := range urls {
		r := results[i]
		switch {
		case r.errKind != "":
			failed++
			fmt.Fprintf(&sb, "%s error %s\n", u, r.errKind)
		case r.status >= 200 && r.status < 300:
			ok++
			fmt.Fprintf(&sb, "%s %d %d\n", u, r.status, r.bytes)
		default:
			bad++
			fmt.Fprintf(&sb, "%s %d %d\n", u, r.status, r.bytes)
		}
	}
	fmt.Fprintf(&sb, "total %d ok %d bad-status %d failed %d\n", ok+bad+failed, ok, bad, failed)
	io.WriteString(w, sb.String())
	if bad+failed == 0 {
		return 0
	}
	return 1
}
