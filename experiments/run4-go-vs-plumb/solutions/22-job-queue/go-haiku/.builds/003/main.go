package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type Job struct {
	ID       int
	Status   string // queued, running, done, cancelled
	Result   *string
	MS       int
	Input    string
	cancelCh chan struct{}
}

type JobQueue struct {
	mu              sync.Mutex
	nextID          int
	jobs            map[int]*Job
	queueOrder      []int            // IDs in queue order
	startedJobs     map[int]bool     // which jobs have started
	runningCount    int32            // atomic counter of running jobs
	maxWorkers      int
	shutdownStarted bool
	jobCompleted    chan int // signals when a job completes
}

func NewJobQueue(maxWorkers int) *JobQueue {
	return &JobQueue{
		nextID:       1,
		jobs:         make(map[int]*Job),
		queueOrder:   make([]int, 0),
		startedJobs:  make(map[int]bool),
		maxWorkers:   maxWorkers,
		jobCompleted: make(chan int, maxWorkers),
	}
}

func (jq *JobQueue) SubmitJob(ms int, input string) (*Job, error) {
	jq.mu.Lock()
	if jq.shutdownStarted {
		jq.mu.Unlock()
		return nil, fmt.Errorf("server is shutting down")
	}

	job := &Job{
		ID:       jq.nextID,
		Status:   "queued",
		Result:   nil,
		MS:       ms,
		Input:    input,
		cancelCh: make(chan struct{}),
	}
	jq.nextID++
	jq.jobs[job.ID] = job
	jq.queueOrder = append(jq.queueOrder, job.ID)
	jq.mu.Unlock()

	jq.tryStartJobs()

	return job, nil
}

func (jq *JobQueue) GetJob(id int) *Job {
	jq.mu.Lock()
	defer jq.mu.Unlock()
	return jq.jobs[id]
}

func (jq *JobQueue) CancelJob(id int) error {
	jq.mu.Lock()
	job, ok := jq.jobs[id]
	if !ok {
		jq.mu.Unlock()
		return fmt.Errorf("job not found")
	}

	if job.Status == "done" || job.Status == "cancelled" {
		jq.mu.Unlock()
		return fmt.Errorf("cannot cancel %s job", job.Status)
	}

	if job.Status == "running" {
		close(job.cancelCh)
	}
	job.Status = "cancelled"
	jq.mu.Unlock()

	// Try to start next job if a running job was cancelled
	if job.Status == "cancelled" {
		jq.tryStartJobs()
	}

	return nil
}

func (jq *JobQueue) GetStats() (queued, running, done, cancelled int) {
	jq.mu.Lock()
	defer jq.mu.Unlock()
	for _, job := range jq.jobs {
		switch job.Status {
		case "queued":
			queued++
		case "running":
			running++
		case "done":
			done++
		case "cancelled":
			cancelled++
		}
	}
	return
}

func (jq *JobQueue) tryStartJobs() {
	for {
		jq.mu.Lock()

		// Check if we have capacity
		if int(atomic.LoadInt32(&jq.runningCount)) >= jq.maxWorkers {
			jq.mu.Unlock()
			break
		}

		// Don't start new jobs during shutdown
		if jq.shutdownStarted {
			jq.mu.Unlock()
			break
		}

		// Find first queued job that hasn't started
		jobID := -1
		for _, id := range jq.queueOrder {
			if !jq.startedJobs[id] {
				job := jq.jobs[id]
				if job.Status == "queued" {
					jobID = id
					break
				}
			}
		}

		if jobID == -1 {
			jq.mu.Unlock()
			break
		}

		job := jq.jobs[jobID]
		job.Status = "running"
		jq.startedJobs[jobID] = true
		atomic.AddInt32(&jq.runningCount, 1)
		jq.mu.Unlock()

		// Run the job in a goroutine
		go func(j *Job) {
			defer func() {
				atomic.AddInt32(&jq.runningCount, -1)
				jq.jobCompleted <- j.ID
			}()

			select {
			case <-time.After(time.Duration(j.MS) * time.Millisecond):
				// Job completed normally
				jq.mu.Lock()
				if j.Status == "running" {
					result := reverseByCodePoint(j.Input)
					j.Result = &result
					j.Status = "done"
				}
				jq.mu.Unlock()
			case <-j.cancelCh:
				// Job was cancelled
				jq.mu.Lock()
				if j.Status == "running" {
					j.Status = "cancelled"
				}
				jq.mu.Unlock()
			}
		}(job)
	}
}

func reverseByCodePoint(s string) string {
	runes := []rune(s)
	for i, j := 0, len(runes)-1; i < j; i, j = i+1, j-1 {
		runes[i], runes[j] = runes[j], runes[i]
	}
	return string(runes)
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	workersStr := os.Getenv("WORKERS")
	workers := 2
	if workersStr != "" {
		w, err := strconv.Atoi(workersStr)
		if err == nil && w > 0 {
			workers = w
		}
	}

	graceMsStr := os.Getenv("SHUTDOWN_GRACE_MS")
	graceMs := 5000
	if graceMsStr != "" {
		g, err := strconv.Atoi(graceMsStr)
		if err == nil && g >= 0 {
			graceMs = g
		}
	}

	jq := NewJobQueue(workers)

	// Listen in background for completed jobs and start next ones
	go func() {
		for range jq.jobCompleted {
			jq.tryStartJobs()
		}
	}()

	// HTTP handler for POST /jobs
	http.HandleFunc("/jobs", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			var reqBody struct {
				MS    interface{} `json:"ms"`
				Input interface{} `json:"input"`
			}
			err := json.NewDecoder(r.Body).Decode(&reqBody)
			if err != nil {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": "invalid JSON"})
				return
			}

			// Validate ms is present and a number
			ms, ok := reqBody.MS.(float64)
			if !ok {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": "invalid ms"})
				return
			}

			// Validate ms is an integer (no fractional part)
			if ms != float64(int(ms)) {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": "invalid ms"})
				return
			}

			// Validate ms is in range [0, 60000]
			if ms < 0 || ms > 60000 {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": "invalid ms"})
				return
			}

			// Validate input is present and a string
			input, ok := reqBody.Input.(string)
			if !ok {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": "invalid input"})
				return
			}

			job, err := jq.SubmitJob(int(ms), input)
			if err != nil {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
				return
			}

			w.WriteHeader(http.StatusAccepted)
			json.NewEncoder(w).Encode(map[string]int{"id": job.ID})
		} else {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
		}
	})

	// HTTP handler for GET /jobs/<id> and POST /jobs/<id>/cancel
	http.HandleFunc("/jobs/", func(w http.ResponseWriter, r *http.Request) {
		parts := strings.Split(r.URL.Path, "/")
		if len(parts) < 3 {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
			return
		}

		idStr := parts[2]
		id, err := strconv.Atoi(idStr)
		if err != nil || id <= 0 {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
			return
		}

		job := jq.GetJob(id)
		if job == nil {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
			return
		}

		if len(parts) == 3 && r.Method == http.MethodGet {
			// GET /jobs/<id>
			w.WriteHeader(http.StatusOK)
			jq.mu.Lock()
			respBody := map[string]interface{}{
				"id":     job.ID,
				"status": job.Status,
				"result": job.Result,
			}
			jq.mu.Unlock()
			json.NewEncoder(w).Encode(respBody)
		} else if len(parts) == 4 && parts[3] == "cancel" && r.Method == http.MethodPost {
			// POST /jobs/<id>/cancel
			if err := jq.CancelJob(id); err != nil {
				w.WriteHeader(http.StatusConflict)
				json.NewEncoder(w).Encode(map[string]string{"error": err.Error()})
				return
			}
			w.WriteHeader(http.StatusOK)
			jq.mu.Lock()
			respBody := map[string]interface{}{
				"id":     job.ID,
				"status": job.Status,
				"result": job.Result,
			}
			jq.mu.Unlock()
			json.NewEncoder(w).Encode(respBody)
		} else {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
		}
	})

	// HTTP handler for GET /stats
	http.HandleFunc("/stats", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			q, run, d, c := jq.GetStats()
			w.WriteHeader(http.StatusOK)
			json.NewEncoder(w).Encode(map[string]int{
				"queued":    q,
				"running":   run,
				"done":      d,
				"cancelled": c,
			})
		} else {
			w.WriteHeader(http.StatusNotFound)
			json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
		}
	})

	// Catch-all for other paths
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
	})

	// Start server with listener
	listener, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		log.Fatalf("Failed to listen: %v", err)
	}

	// Handle SIGTERM for graceful shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGTERM)

	go func() {
		<-sigChan

		// Mark shutdown started so no new jobs are accepted
		jq.mu.Lock()
		jq.shutdownStarted = true
		jq.mu.Unlock()

		// Close listener to stop accepting new connections
		listener.Close()

		// Record the state at shutdown
		_, runningAtSignal, doneAtSignal, _ := jq.GetStats()
		queuedAtSignal := 0
		jq.mu.Lock()
		for _, job := range jq.jobs {
			if job.Status == "queued" {
				queuedAtSignal++
			}
		}
		jq.mu.Unlock()

		// Wait for running jobs to complete with grace period
		startTime := time.Now()
		graceDuration := time.Duration(graceMs) * time.Millisecond

		for {
			_, running, done, _ := jq.GetStats()
			if running == 0 {
				// No jobs running, safe to shutdown
				completed := done - doneAtSignal
				abandoned := queuedAtSignal + (runningAtSignal - completed)
				fmt.Printf("shutdown: completed %d, abandoned %d\n", completed, abandoned)
				os.Exit(0)
			}

			elapsed := time.Since(startTime)
			if elapsed >= graceDuration {
				// Grace period expired
				_, stillRunning, done, _ := jq.GetStats()
				completed := done - doneAtSignal
				abandoned := queuedAtSignal + stillRunning
				fmt.Printf("shutdown: completed %d, abandoned %d\n", completed, abandoned)
				os.Exit(1)
			}

			time.Sleep(10 * time.Millisecond)
		}
	}()

	// Serve HTTP requests
	http.Serve(listener, nil)
}
