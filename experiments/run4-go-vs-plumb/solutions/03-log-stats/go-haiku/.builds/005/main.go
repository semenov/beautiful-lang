package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type EndpointStats struct {
	Endpoint  string
	Count     int
	ErrorRate float64
	P50       int
	P95       int
	Max       int
	Latencies []int
}

func main() {
	fs := flag.NewFlagSet("", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	minCount := fs.Int("min-count", 1, "minimum request count")
	err := fs.Parse(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "usage: app [--min-count N] [FILE...]\n")
		os.Exit(64)
	}

	if *minCount < 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--min-count N] [FILE...]\n")
		os.Exit(64)
	}

	files := fs.Args()

	var readers []io.Reader
	if len(files) == 0 {
		readers = []io.Reader{os.Stdin}
	} else {
		for _, file := range files {
			f, err := os.Open(file)
			if err != nil {
				fmt.Fprintf(os.Stderr, "error: cannot read %s\n", file)
				os.Exit(2)
			}
			defer f.Close()
			readers = append(readers, f)
		}
	}

	// Read and parse all logs
	stats := make(map[string]*EndpointStats)
	totalValid := 0
	totalMalformed := 0

	logLineRegex := regexp.MustCompile(`^(\S+) (\S+) (\S+) \[([^\]]*)\] "([A-Z]+) (\S+) (HTTP/[\d.]+)" (\d{3}) (-|\d+) (\d+)$`)

	for _, reader := range readers {
		scanner := bufio.NewScanner(reader)
		for scanner.Scan() {
			line := scanner.Text()
			if line == "" {
				continue
			}

			matches := logLineRegex.FindStringSubmatch(line)
			if matches == nil {
				totalMalformed++
				continue
			}

			method := matches[5]
			target := matches[6]
			statusStr := matches[8]
			latencyStr := matches[10]

			status, _ := strconv.Atoi(statusStr)
			latency, _ := strconv.Atoi(latencyStr)

			// Extract endpoint (up to query params)
			endpoint := method + " " + strings.Split(target, "?")[0]

			if stats[endpoint] == nil {
				stats[endpoint] = &EndpointStats{
					Endpoint:  endpoint,
					Latencies: []int{},
				}
			}

			s := stats[endpoint]
			s.Count++
			if status >= 500 && status <= 599 {
				s.ErrorRate++
			}
			s.Latencies = append(s.Latencies, latency)
			if latency > s.Max {
				s.Max = latency
			}

			totalValid++
		}
	}

	// Calculate percentiles and error rates
	for _, s := range stats {
		sort.Ints(s.Latencies)

		// Calculate error rate
		s.ErrorRate = (s.ErrorRate / float64(s.Count)) * 100

		// Calculate percentiles
		s.P50 = getNearestRankPercentile(s.Latencies, 50)
		s.P95 = getNearestRankPercentile(s.Latencies, 95)
	}

	// Sort endpoints: by count (descending), then by endpoint (ascending)
	var endpoints []*EndpointStats
	for _, s := range stats {
		if s.Count >= *minCount {
			endpoints = append(endpoints, s)
		}
	}
	sort.Slice(endpoints, func(i, j int) bool {
		if endpoints[i].Count != endpoints[j].Count {
			return endpoints[i].Count > endpoints[j].Count
		}
		return endpoints[i].Endpoint < endpoints[j].Endpoint
	})

	// Output
	for _, s := range endpoints {
		errorRateStr := formatErrorRate(s.ErrorRate)
		fmt.Printf("%s count=%d error_rate=%s%% p50=%d p95=%d max=%d\n",
			s.Endpoint, s.Count, errorRateStr, s.P50, s.P95, s.Max)
	}
	fmt.Printf("total=%d malformed=%d\n", totalValid, totalMalformed)
}

func getNearestRankPercentile(latencies []int, percentile float64) int {
	if len(latencies) == 0 {
		return 0
	}

	n := float64(len(latencies))
	pos := math.Ceil((percentile / 100) * n)
	if pos < 1 {
		pos = 1
	}
	if pos > n {
		pos = n
	}

	return latencies[int(pos)-1]
}

func formatErrorRate(errorRate float64) string {
	// Round half-up to 2 decimal places
	rounded := math.Floor(errorRate*100+0.5) / 100
	return fmt.Sprintf("%.2f", rounded)
}
