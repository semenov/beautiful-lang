// A small HTTP/1.1 load generator: N keep-alive connections, each sending
// requests back to back for a fixed time. Prints requests per second.
//
//	go run load.go -c 64 -d 5s http://127.0.0.1:8080/hello
package main

import (
	"bufio"
	"flag"
	"fmt"
	"net"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	conns := flag.Int("c", 64, "connections")
	dur := flag.Duration("d", 5*time.Second, "duration")
	flag.Parse()
	u, err := url.Parse(flag.Arg(0))
	if err != nil || u.Host == "" {
		fmt.Fprintln(os.Stderr, "usage: load -c 64 -d 5s http://host:port/path")
		os.Exit(2)
	}
	req := []byte("GET " + u.RequestURI() + " HTTP/1.1\r\nHost: " + u.Host + "\r\nUser-Agent: load\r\n\r\n")
	var done, failed int64
	stop := time.Now().Add(*dur)
	var wg sync.WaitGroup
	for i := 0; i < *conns; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			c, err := net.Dial("tcp", u.Host)
			if err != nil {
				atomic.AddInt64(&failed, 1)
				return
			}
			defer c.Close()
			r := bufio.NewReaderSize(c, 65536)
			for time.Now().Before(stop) {
				if _, err := c.Write(req); err != nil {
					atomic.AddInt64(&failed, 1)
					return
				}
				length := -1
				status := ""
				for {
					line, err := r.ReadString('\n')
					if err != nil {
						atomic.AddInt64(&failed, 1)
						return
					}
					line = strings.TrimRight(line, "\r\n")
					if status == "" {
						status = line
						continue
					}
					if line == "" {
						break
					}
					if k, v, ok := strings.Cut(line, ":"); ok && strings.EqualFold(k, "content-length") {
						length, _ = strconv.Atoi(strings.TrimSpace(v))
					}
				}
				if length < 0 || !strings.Contains(status, " 200") {
					atomic.AddInt64(&failed, 1)
					return
				}
				if _, err := r.Discard(length); err != nil {
					atomic.AddInt64(&failed, 1)
					return
				}
				atomic.AddInt64(&done, 1)
			}
		}()
	}
	wg.Wait()
	fmt.Printf("%.0f req/s (%d requests, %d failed)\n", float64(done)/dur.Seconds(), done, failed)
}
