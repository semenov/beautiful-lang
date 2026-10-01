package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

type Event struct {
	ID       int64
	Source   string
	Payload  json.RawMessage
	Body     []byte
	Status   string
	Attempts int
}

type EventRequest struct {
	Source  string          `json:"source"`
	Payload json.RawMessage `json:"payload"`
}

type EventResponse struct {
	ID int64 `json:"id"`
}

type StatusResponse struct {
	ID       int64  `json:"id"`
	Source   string `json:"source"`
	Status   string `json:"status"`
	Attempts int    `json:"attempts"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

var (
	events   = make(map[int64]*Event)
	eventsMu sync.Mutex
	eventID  int64 = 0

	targetURL         string
	secret            string
	maxAttempts       int
	backoffMS         int
	attemptTimeoutMS  int

	// Per-source delivery queues
	sourceQueues = make(map[string]chan int64)
	sourceQueueMu sync.Mutex
)

func main() {
	// Parse environment variables
	port := os.Getenv("PORT")
	if port == "" {
		log.Fatal("PORT not set")
	}

	targetURL = os.Getenv("TARGET_URL")
	if targetURL == "" {
		log.Fatal("TARGET_URL not set")
	}

	secret = os.Getenv("SECRET")
	if secret == "" {
		log.Fatal("SECRET not set")
	}

	var err error
	maxAttempts = 5
	if ma := os.Getenv("MAX_ATTEMPTS"); ma != "" {
		maxAttempts, err = strconv.Atoi(ma)
		if err != nil || maxAttempts < 1 {
			log.Fatal("Invalid MAX_ATTEMPTS")
		}
	}

	backoffMS = 1000
	if bm := os.Getenv("BACKOFF_MS"); bm != "" {
		backoffMS, err = strconv.Atoi(bm)
		if err != nil || backoffMS < 1 {
			log.Fatal("Invalid BACKOFF_MS")
		}
	}

	attemptTimeoutMS = 5000
	if atm := os.Getenv("ATTEMPT_TIMEOUT_MS"); atm != "" {
		attemptTimeoutMS, err = strconv.Atoi(atm)
		if err != nil || attemptTimeoutMS < 1 {
			log.Fatal("Invalid ATTEMPT_TIMEOUT_MS")
		}
	}

	// Set up HTTP handlers
	http.HandleFunc("/events", handleEvents)
	http.HandleFunc("/events/", handleEventStatus)

	// Listen on 127.0.0.1:PORT
	addr := "127.0.0.1:" + port
	log.Printf("Starting webhook relay on %s", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatal(err)
	}
}

func handleEvents(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
		return
	}

	// Read the entire body
	body, err := io.ReadAll(r.Body)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid request"})
		return
	}
	defer r.Body.Close()

	// Parse JSON
	var req EventRequest
	if err := json.Unmarshal(body, &req); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid JSON"})
		return
	}

	// Validate source
	if req.Source == "" {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "source is required and must not be empty"})
		return
	}

	// Validate payload is present
	if req.Payload == nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "payload is required"})
		return
	}

	// Create event
	eventsMu.Lock()
	eventID++
	id := eventID
	event := &Event{
		ID:       id,
		Source:   req.Source,
		Payload:  req.Payload,
		Body:     body,
		Status:   "pending",
		Attempts: 0,
	}
	events[id] = event
	eventsMu.Unlock()

	// Queue for delivery
	sourceQueueMu.Lock()
	queue, exists := sourceQueues[req.Source]
	if !exists {
		queue = make(chan int64, 100)
		sourceQueues[req.Source] = queue
		go sourceWorker(req.Source, queue)
	}
	sourceQueueMu.Unlock()

	queue <- id

	// Respond with 202
	w.WriteHeader(http.StatusAccepted)
	json.NewEncoder(w).Encode(EventResponse{ID: id})
}

func handleEventStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
		return
	}

	// Parse event ID from path
	idStr := r.URL.Path[len("/events/"):]
	if idStr == "" {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
		return
	}

	id, err := strconv.ParseInt(idStr, 10, 64)
	if err != nil {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
		return
	}

	eventsMu.Lock()
	event, exists := events[id]
	eventsMu.Unlock()

	if !exists {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
		return
	}

	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(StatusResponse{
		ID:       event.ID,
		Source:   event.Source,
		Status:   event.Status,
		Attempts: event.Attempts,
	})
}

func sourceWorker(source string, queue chan int64) {
	for eventID := range queue {
		deliverEvent(eventID)
	}
}

func deliverEvent(eventID int64) {
	eventsMu.Lock()
	event := events[eventID]
	eventsMu.Unlock()

	// Retry loop
	for attempt := 0; attempt < maxAttempts; attempt++ {
		if attempt > 0 {
			// Exponential backoff: BACKOFF_MS × 2^(attempt-1)
			waitMS := backoffMS * (1 << (attempt - 1))
			time.Sleep(time.Duration(waitMS) * time.Millisecond)
		}

		// Increment attempts
		eventsMu.Lock()
		event.Attempts++
		eventsMu.Unlock()

		// Try to deliver
		if tryDeliver(event) {
			eventsMu.Lock()
			event.Status = "delivered"
			eventsMu.Unlock()
			return
		}
	}

	// All attempts failed
	eventsMu.Lock()
	event.Status = "failed"
	eventsMu.Unlock()
}

func tryDeliver(event *Event) bool {
	// Create HTTP request
	req, err := http.NewRequest(http.MethodPost, targetURL, bytes.NewReader(event.Body))
	if err != nil {
		return false
	}

	// Set headers
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Event-Id", strconv.FormatInt(event.ID, 10))

	// Calculate signature
	h := hmac.New(sha256.New, []byte(secret))
	h.Write(event.Body)
	signature := "sha256=" + hex.EncodeToString(h.Sum(nil))
	req.Header.Set("X-Signature", signature)

	// Create HTTP client with timeout
	client := &http.Client{
		Timeout: time.Duration(attemptTimeoutMS) * time.Millisecond,
	}

	// Make request
	resp, err := client.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()

	// Check if 2xx
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}
