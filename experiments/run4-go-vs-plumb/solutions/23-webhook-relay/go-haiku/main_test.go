package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestEventAcceptance(t *testing.T) {
	// Setup env
	os.Setenv("PORT", "9999")
	os.Setenv("TARGET_URL", "http://target")
	os.Setenv("SECRET", "test-secret")
	os.Setenv("MAX_ATTEMPTS", "3")
	os.Setenv("BACKOFF_MS", "100")
	os.Setenv("ATTEMPT_TIMEOUT_MS", "5000")

	// Reset global state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = "http://target"
	secret = "test-secret"
	maxAttempts = 3
	backoffMS = 100
	attemptTimeoutMS = 5000

	tests := []struct {
		name       string
		body       string
		wantStatus int
		wantError  bool
	}{
		{
			name:       "valid event",
			body:       `{"source":"test","payload":{"data":"value"}}`,
			wantStatus: http.StatusAccepted,
			wantError:  false,
		},
		{
			name:       "missing source",
			body:       `{"payload":{"data":"value"}}`,
			wantStatus: http.StatusBadRequest,
			wantError:  true,
		},
		{
			name:       "empty source",
			body:       `{"source":"","payload":{"data":"value"}}`,
			wantStatus: http.StatusBadRequest,
			wantError:  true,
		},
		{
			name:       "missing payload",
			body:       `{"source":"test"}`,
			wantStatus: http.StatusBadRequest,
			wantError:  true,
		},
		{
			name:       "invalid JSON",
			body:       `{invalid json}`,
			wantStatus: http.StatusBadRequest,
			wantError:  true,
		},
		{
			name:       "extra fields ignored",
			body:       `{"source":"test","payload":{},"extra":"ignored"}`,
			wantStatus: http.StatusAccepted,
			wantError:  false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodPost, "/events", strings.NewReader(tt.body))
			w := httptest.NewRecorder()
			handleEvents(w, req)

			if w.Code != tt.wantStatus {
				t.Errorf("status = %d, want %d", w.Code, tt.wantStatus)
			}

			if tt.wantError {
				var errResp ErrorResponse
				json.Unmarshal(w.Body.Bytes(), &errResp)
				if errResp.Error == "" {
					t.Error("expected error response")
				}
			} else {
				var resp EventResponse
				json.Unmarshal(w.Body.Bytes(), &resp)
				if resp.ID == 0 {
					t.Error("expected valid ID")
				}
			}
		})
	}
}

func TestEventIDSequence(t *testing.T) {
	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = "http://target"
	secret = "test"

	var ids []int64
	for i := 0; i < 5; i++ {
		req := httptest.NewRequest(http.MethodPost, "/events",
			strings.NewReader(fmt.Sprintf(`{"source":"test","payload":%d}`, i)))
		w := httptest.NewRecorder()
		handleEvents(w, req)

		var resp EventResponse
		json.Unmarshal(w.Body.Bytes(), &resp)
		ids = append(ids, resp.ID)
	}

	// IDs should be sequential
	for i, id := range ids {
		if id != int64(i+1) {
			t.Errorf("ID[%d] = %d, want %d", i, id, i+1)
		}
	}
}

func TestEventStatus(t *testing.T) {
	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0

	// Create an event
	events[1] = &Event{
		ID:       1,
		Source:   "test-source",
		Status:   "pending",
		Attempts: 0,
	}

	req := httptest.NewRequest(http.MethodGet, "/events/1", nil)
	w := httptest.NewRecorder()
	handleEventStatus(w, req)

	if w.Code != http.StatusOK {
		t.Errorf("status = %d, want %d", w.Code, http.StatusOK)
	}

	var resp StatusResponse
	json.Unmarshal(w.Body.Bytes(), &resp)

	if resp.ID != 1 || resp.Source != "test-source" || resp.Status != "pending" {
		t.Errorf("unexpected response: %+v", resp)
	}
}

func TestEventStatusNotFound(t *testing.T) {
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)

	req := httptest.NewRequest(http.MethodGet, "/events/999", nil)
	w := httptest.NewRecorder()
	handleEventStatus(w, req)

	if w.Code != http.StatusNotFound {
		t.Errorf("status = %d, want %d", w.Code, http.StatusNotFound)
	}
}

func TestSignatureCalculation(t *testing.T) {
	secret = "mysecret"
	body := []byte(`{"source":"test","payload":{"id":1}}`)

	// Calculate signature
	h := hmac.New(sha256.New, []byte(secret))
	h.Write(body)
	sig := hex.EncodeToString(h.Sum(nil))

	// Verify signature length is 64 hex characters
	if len(sig) != 64 {
		t.Errorf("signature length = %d, want 64", len(sig))
	}

	// Verify it's lowercase hex
	for _, c := range sig {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			t.Errorf("signature contains non-hex character: %c", c)
		}
	}
}

func TestDeliveryWithMockTarget(t *testing.T) {
	// Create a mock target server
	receivedRequests := make([]struct {
		body      []byte
		eventID   string
		signature string
		contentType string
	}, 0)

	mockServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		receivedRequests = append(receivedRequests, struct {
			body      []byte
			eventID   string
			signature string
			contentType string
		}{
			body:      body,
			eventID:   r.Header.Get("X-Event-Id"),
			signature: r.Header.Get("X-Signature"),
			contentType: r.Header.Get("Content-Type"),
		})
		w.WriteHeader(http.StatusOK)
	}))
	defer mockServer.Close()

	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = mockServer.URL
	secret = "test-secret"
	maxAttempts = 3
	backoffMS = 100
	attemptTimeoutMS = 5000

	// Send event
	reqBody := `{"source":"test","payload":{"data":"value"}}`
	req := httptest.NewRequest(http.MethodPost, "/events", strings.NewReader(reqBody))
	w := httptest.NewRecorder()
	handleEvents(w, req)

	// Wait for delivery
	time.Sleep(500 * time.Millisecond)

	if len(receivedRequests) == 0 {
		t.Fatal("no requests received at mock server")
	}

	// Check the received request
	recv := receivedRequests[0]
	if recv.eventID != "1" {
		t.Errorf("X-Event-Id = %s, want 1", recv.eventID)
	}

	if recv.contentType != "application/json" {
		t.Errorf("Content-Type = %s, want application/json", recv.contentType)
	}

	if !strings.HasPrefix(recv.signature, "sha256=") {
		t.Errorf("signature format invalid: %s", recv.signature)
	}

	// Verify body is exactly what we sent
	if string(recv.body) != reqBody {
		t.Errorf("body = %s, want %s", string(recv.body), reqBody)
	}

	// Check event status
	req = httptest.NewRequest(http.MethodGet, "/events/1", nil)
	w = httptest.NewRecorder()
	handleEventStatus(w, req)

	var resp StatusResponse
	json.Unmarshal(w.Body.Bytes(), &resp)
	if resp.Status != "delivered" {
		t.Errorf("status = %s, want delivered", resp.Status)
	}
	if resp.Attempts != 1 {
		t.Errorf("attempts = %d, want 1", resp.Attempts)
	}
}

func TestRetryLogic(t *testing.T) {
	// Create a mock target that fails a few times then succeeds
	attemptCount := 0
	mockServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		attemptCount++
		if attemptCount < 3 {
			w.WriteHeader(http.StatusInternalServerError)
		} else {
			w.WriteHeader(http.StatusOK)
		}
	}))
	defer mockServer.Close()

	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = mockServer.URL
	secret = "test"
	maxAttempts = 5
	backoffMS = 50
	attemptTimeoutMS = 5000

	// Send event
	req := httptest.NewRequest(http.MethodPost, "/events",
		strings.NewReader(`{"source":"test","payload":{"data":"value"}}`))
	w := httptest.NewRecorder()
	handleEvents(w, req)

	// Wait for retries to complete
	time.Sleep(1000 * time.Millisecond)

	// Check event delivered
	req = httptest.NewRequest(http.MethodGet, "/events/1", nil)
	w = httptest.NewRecorder()
	handleEventStatus(w, req)

	var resp StatusResponse
	json.Unmarshal(w.Body.Bytes(), &resp)
	if resp.Status != "delivered" {
		t.Errorf("status = %s, want delivered", resp.Status)
	}
	if resp.Attempts != 3 {
		t.Errorf("attempts = %d, want 3", resp.Attempts)
	}
}

func TestSourceOrdering(t *testing.T) {
	// Create a mock target that records delivery order
	deliveryOrder := make([]string, 0)
	mockServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		eventID := r.Header.Get("X-Event-Id")
		deliveryOrder = append(deliveryOrder, eventID)
		// Delay to ensure ordering is visible
		time.Sleep(10 * time.Millisecond)
		w.WriteHeader(http.StatusOK)
	}))
	defer mockServer.Close()

	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = mockServer.URL
	secret = "test"
	maxAttempts = 1
	backoffMS = 100
	attemptTimeoutMS = 5000

	// Send multiple events with same source
	for i := 1; i <= 3; i++ {
		req := httptest.NewRequest(http.MethodPost, "/events",
			strings.NewReader(fmt.Sprintf(`{"source":"same","payload":%d}`, i)))
		w := httptest.NewRecorder()
		handleEvents(w, req)
	}

	// Wait for all deliveries
	time.Sleep(1000 * time.Millisecond)

	// Events with same source should be delivered in order
	if len(deliveryOrder) != 3 {
		t.Errorf("deliveries = %d, want 3", len(deliveryOrder))
	}
	for i, id := range deliveryOrder {
		expectedID := strconv.Itoa(i + 1)
		if id != expectedID {
			t.Errorf("delivery order[%d] = %s, want %s", i, id, expectedID)
		}
	}
}

func TestDifferentSourcesConcurrent(t *testing.T) {
	// Verify that different sources don't wait for each other
	source1Done := make(chan bool)
	source2Started := make(chan bool)

	mockServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		eventID := r.Header.Get("X-Event-Id")
		if eventID == "1" {
			// Slow response for source1 event
			source2Started <- true
			time.Sleep(200 * time.Millisecond)
			source1Done <- true
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer mockServer.Close()

	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0
	targetURL = mockServer.URL
	secret = "test"
	maxAttempts = 1
	backoffMS = 100
	attemptTimeoutMS = 5000

	// Send event to source1
	req := httptest.NewRequest(http.MethodPost, "/events",
		strings.NewReader(`{"source":"source1","payload":1}`))
	w := httptest.NewRecorder()
	handleEvents(w, req)

	// Wait for source1 to start delivery
	<-source2Started

	// Send event to source2 (should not wait)
	start := time.Now()
	req = httptest.NewRequest(http.MethodPost, "/events",
		strings.NewReader(`{"source":"source2","payload":2}`))
	w = httptest.NewRecorder()
	handleEvents(w, req)

	// source2 should deliver quickly without waiting for source1
	time.Sleep(100 * time.Millisecond)

	req = httptest.NewRequest(http.MethodGet, "/events/2", nil)
	w = httptest.NewRecorder()
	handleEventStatus(w, req)

	var resp StatusResponse
	json.Unmarshal(w.Body.Bytes(), &resp)

	elapsed := time.Since(start)
	if resp.Status != "delivered" {
		t.Errorf("source2 should have delivered quickly, but status = %s", resp.Status)
	}
	if elapsed > 150*time.Millisecond {
		t.Errorf("source2 delivery took %v, should be much faster", elapsed)
	}

	<-source1Done
}

func TestHTTPErrors(t *testing.T) {
	// Reset state
	events = make(map[int64]*Event)
	sourceQueues = make(map[string]chan int64)
	eventID = 0

	// Test 404 on invalid event ID
	req := httptest.NewRequest(http.MethodGet, "/events/invalid", nil)
	w := httptest.NewRecorder()
	handleEventStatus(w, req)

	if w.Code != http.StatusNotFound {
		t.Errorf("invalid event ID should return 404, got %d", w.Code)
	}
}

func TestPortBinding(t *testing.T) {
	// Find a free port
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("failed to find free port: %v", err)
	}
	port := strconv.Itoa(listener.Addr().(*net.TCPAddr).Port)
	listener.Close()

	// Set up environment
	os.Setenv("PORT", port)
	os.Setenv("TARGET_URL", "http://localhost:9999")
	os.Setenv("SECRET", "test")

	// This is just to verify the port format is valid
	if port == "" {
		t.Error("port should not be empty")
	}
}
