package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

type Result struct {
	URL        string
	StatusCode int
	Bytes      int64
	Error      string // empty if no error
	ErrorKind  string // invalid-url, timeout, connect, redirects, other
}

func main() {
	concurrency := flag.Int("concurrency", 4, "max concurrent fetches")
	timeout := flag.Duration("timeout", 10*time.Second, "timeout per URL")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: app [--concurrency N] [--timeout SECONDS] [FILE]\n")
	}
	flag.CommandLine.Parse(os.Args[1:])

	args := flag.Args()
	if len(args) > 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--concurrency N] [--timeout SECONDS] [FILE]\n")
		os.Exit(64)
	}

	// Validate concurrency
	if *concurrency < 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--concurrency N] [--timeout SECONDS] [FILE]\n")
		os.Exit(64)
	}

	// Validate timeout
	if *timeout <= 0 {
		fmt.Fprintf(os.Stderr, "usage: app [--concurrency N] [--timeout SECONDS] [FILE]\n")
		os.Exit(64)
	}

	// Read URLs
	urls, err := readURLs(args)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %s\n", err)
		os.Exit(2)
	}

	// Fetch URLs concurrently
	results := fetchURLs(urls, *concurrency, *timeout)

	// Output results
	exitCode := outputResults(results)
	os.Exit(exitCode)
}

func readURLs(args []string) ([]string, error) {
	var input io.Reader = os.Stdin
	if len(args) == 1 {
		f, err := os.Open(args[0])
		if err != nil {
			return nil, fmt.Errorf("cannot read %s", args[0])
		}
		defer f.Close()
		input = f
	}

	var urls []string
	scanner := bufio.NewScanner(input)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		urls = append(urls, line)
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("cannot read input: %w", err)
	}
	return urls, nil
}

func fetchURLs(urls []string, concurrency int, timeout time.Duration) []Result {
	results := make([]Result, len(urls))
	work := make(chan int, concurrency)
	done := make(chan struct{})

	// Start workers
	for i := 0; i < concurrency; i++ {
		go func() {
			for idx := range work {
				results[idx] = fetchURL(urls[idx], timeout)
			}
			done <- struct{}{}
		}()
	}

	// Send work
	go func() {
		for i := range urls {
			work <- i
		}
		close(work)
	}()

	// Wait for workers
	for i := 0; i < concurrency; i++ {
		<-done
	}

	return results
}

func fetchURL(urlStr string, timeout time.Duration) Result {
	result := Result{URL: urlStr}

	// Validate URL
	parsedURL, err := url.Parse(urlStr)
	if err != nil || (parsedURL.Scheme != "http" && parsedURL.Scheme != "https") || parsedURL.Host == "" {
		result.Error = "invalid-url"
		result.ErrorKind = "invalid-url"
		return result
	}

	// Create HTTP client with timeout
	client := &http.Client{
		Timeout: timeout,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= 10 {
				return http.ErrUseLastResponse // Stop following redirects
			}
			return nil
		},
	}

	// Make request
	start := time.Now()
	resp, err := client.Get(urlStr)
	if err != nil {
		kind := classifyError(err, time.Since(start), timeout)
		result.Error = kind
		result.ErrorKind = kind
		return result
	}
	defer resp.Body.Close()

	// Check for too many redirects
	if resp.StatusCode == http.StatusMovedPermanently || resp.StatusCode == http.StatusFound ||
		resp.StatusCode == http.StatusSeeOther || resp.StatusCode == http.StatusTemporaryRedirect ||
		resp.StatusCode == http.StatusPermanentRedirect {
		// If we got here with a redirect status, it means we exhausted the redirect limit
		// Actually, CheckRedirect returning http.ErrUseLastResponse means we won't follow more
		// This case shouldn't happen with our setup
	}

	// Read body with timeout
	done := make(chan error, 1)
	var body []byte
	go func() {
		var readErr error
		body, readErr = io.ReadAll(resp.Body)
		done <- readErr
	}()

	// Wait with timeout
	select {
	case <-time.After(timeout - time.Since(start)):
		result.Error = "timeout"
		result.ErrorKind = "timeout"
		return result
	case err := <-done:
		if err != nil {
			result.Error = "other"
			result.ErrorKind = "other"
			return result
		}
	}

	result.StatusCode = resp.StatusCode
	result.Bytes = int64(len(body))
	return result
}

func classifyError(err error, elapsed time.Duration, timeout time.Duration) string {
	if err == http.ErrUseLastResponse {
		return "redirects"
	}

	if strings.Contains(err.Error(), "timeout") || strings.Contains(err.Error(), "context deadline") {
		return "timeout"
	}

	// Check for connection errors
	if _, ok := err.(net.Error); ok {
		if netErr, ok := err.(net.Error); ok {
			if netErr.Timeout() {
				return "timeout"
			}
		}
		return "connect"
	}

	return "other"
}

func outputResults(results []Result) int {
	okCount := 0
	badStatusCount := 0
	failedCount := 0

	for _, r := range results {
		if r.Error != "" {
			fmt.Printf("%s error %s\n", r.URL, r.ErrorKind)
			failedCount++
		} else {
			fmt.Printf("%s %d %d\n", r.URL, r.StatusCode, r.Bytes)
			if r.StatusCode >= 200 && r.StatusCode < 300 {
				okCount++
			} else {
				badStatusCount++
			}
		}
	}

	total := len(results)
	fmt.Printf("total %d ok %d bad-status %d failed %d\n", total, okCount, badStatusCount, failedCount)

	if failedCount > 0 || badStatusCount > 0 {
		return 1
	}
	return 0
}
