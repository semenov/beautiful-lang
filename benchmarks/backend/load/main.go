// Load for the notes service: sets up users and notes, then runs a mix of
// requests over keep-alive connections for a while and prints requests per
// second and latency percentiles.
//
//	go run . -url http://127.0.0.1:8300 -c 64 -d 10s
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"os"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

type account struct {
	token string
	notes []int64
}

func must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func call(c *http.Client, method, url, token string, body any, out any) int {
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, url, rd)
	must(err)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	res, err := c.Do(req)
	if err != nil {
		return 0
	}
	data, _ := io.ReadAll(res.Body)
	res.Body.Close()
	if out != nil {
		json.Unmarshal(data, out)
	}
	return res.StatusCode
}

func main() {
	base := flag.String("url", "http://127.0.0.1:8300", "server")
	conns := flag.Int("c", 64, "connections")
	dur := flag.Duration("d", 10*time.Second, "duration")
	users := flag.Int("users", 100, "users")
	perUser := flag.Int("notes", 20, "notes per user to start with")
	flag.Parse()
	tr := &http.Transport{MaxIdleConns: *conns * 2, MaxIdleConnsPerHost: *conns * 2, MaxConnsPerHost: *conns}
	c := &http.Client{Transport: tr, Timeout: 10 * time.Second}

	run := strconv.FormatInt(time.Now().UnixNano(), 36)
	accounts := make([]*account, *users)
	for i := range accounts {
		email := fmt.Sprintf("user%d-%s@example.com", i, run)
		if st := call(c, "POST", *base+"/users", "", map[string]string{"name": fmt.Sprintf("User %d", i), "email": email}, nil); st != 201 {
			must(fmt.Errorf("create user: status %d", st))
		}
		var tok struct {
			Token string `json:"token"`
		}
		if st := call(c, "POST", *base+"/login", "", map[string]string{"email": email}, &tok); st != 200 {
			must(fmt.Errorf("login: status %d", st))
		}
		a := &account{token: tok.Token}
		for k := 0; k < *perUser; k++ {
			var n struct {
				ID int64 `json:"id"`
			}
			if st := call(c, "POST", *base+"/notes", a.token, map[string]string{"title": fmt.Sprintf("Note %d", k), "body": "Some text of a note, a few words long, like people write."}, &n); st != 201 {
				must(fmt.Errorf("create note: status %d", st))
			}
			a.notes = append(a.notes, n.ID)
		}
		accounts[i] = a
	}

	var done, failed int64
	var mu sync.Mutex
	var lat []time.Duration
	stop := time.Now().Add(*dur)
	var wg sync.WaitGroup
	for w := 0; w < *conns; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			rng := rand.New(rand.NewSource(int64(w)))
			var mine []time.Duration
			for time.Now().Before(stop) {
				a := accounts[rng.Intn(len(accounts))]
				t0 := time.Now()
				var st int
				switch p := rng.Intn(10); {
				case p < 7:
					st = call(c, "GET", *base+"/notes?limit=20", a.token, nil, nil)
				case p < 9:
					id := a.notes[rng.Intn(len(a.notes))]
					st = call(c, "GET", *base+"/notes/"+strconv.FormatInt(id, 10), a.token, nil, nil)
				default:
					st = call(c, "POST", *base+"/notes", a.token, map[string]string{"title": "New note", "body": "Written during the load test."}, nil)
				}
				mine = append(mine, time.Since(t0))
				if st < 200 || st >= 300 {
					atomic.AddInt64(&failed, 1)
				}
				atomic.AddInt64(&done, 1)
			}
			mu.Lock()
			lat = append(lat, mine...)
			mu.Unlock()
		}(w)
	}
	wg.Wait()
	sort.Slice(lat, func(i, j int) bool { return lat[i] < lat[j] })
	pct := func(p float64) time.Duration { return lat[int(float64(len(lat)-1)*p)] }
	fmt.Printf("%.0f req/s, p50 %v, p99 %v, %d failed of %d\n", float64(done)/dur.Seconds(), pct(0.5).Round(time.Microsecond), pct(0.99).Round(time.Microsecond), failed, done)
}
