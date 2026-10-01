package main

import (
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

type TokenBucket struct {
	tokens    float64
	lastRefill time.Time
}

type RateLimiter struct {
	capacity     int
	refillPerSec float64
	buckets      map[string]*TokenBucket
	mu           sync.Mutex
	allowedCount int
	deniedCount  int
	keyCount     int
	keySet       map[string]bool
}

func NewRateLimiter(capacity int, refillPerSec float64) *RateLimiter {
	return &RateLimiter{
		capacity:     capacity,
		refillPerSec: refillPerSec,
		buckets:      make(map[string]*TokenBucket),
		keySet:       make(map[string]bool),
	}
}

func (rl *RateLimiter) refill(bucket *TokenBucket) {
	now := time.Now()
	elapsed := now.Sub(bucket.lastRefill).Seconds()
	bucket.tokens = math.Min(float64(rl.capacity), bucket.tokens+elapsed*rl.refillPerSec)
	bucket.lastRefill = now
}

func (rl *RateLimiter) check(key string) (allowed bool, remaining int, retryAfter float64) {
	rl.mu.Lock()
	defer rl.mu.Unlock()

	// Track key for stats
	if !rl.keySet[key] {
		rl.keySet[key] = true
		rl.keyCount++
	}

	bucket, exists := rl.buckets[key]
	if !exists {
		bucket = &TokenBucket{
			tokens:     float64(rl.capacity),
			lastRefill: time.Now(),
		}
		rl.buckets[key] = bucket
	}

	// Refill tokens
	rl.refill(bucket)

	if bucket.tokens >= 1.0 {
		bucket.tokens -= 1.0
		remaining := int(math.Floor(bucket.tokens))
		rl.allowedCount++
		return true, remaining, 0
	}

	// Denied: calculate retry_after
	tokensNeeded := 1.0 - bucket.tokens
	secondsNeeded := tokensNeeded / rl.refillPerSec
	retryAfter = math.Ceil(secondsNeeded)
	if retryAfter < 1.0 {
		retryAfter = 1.0
	}
	rl.deniedCount++
	return false, 0, retryAfter
}

func (rl *RateLimiter) getStats() (allowed int, denied int, keys int) {
	rl.mu.Lock()
	defer rl.mu.Unlock()
	return rl.allowedCount, rl.deniedCount, rl.keyCount
}

func (rl *RateLimiter) handleCheck(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(map[string]string{"error": "method not allowed"})
		return
	}

	var req map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": "body is not a JSON object"})
		return
	}

	keyVal, ok := req["key"]
	if !ok {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": "key is missing"})
		return
	}

	key, ok := keyVal.(string)
	if !ok {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": "key is not a string"})
		return
	}

	if key == "" {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(map[string]string{"error": "key is empty"})
		return
	}

	allowed, remaining, retryAfter := rl.check(key)

	w.Header().Set("Content-Type", "application/json")
	if allowed {
		w.WriteHeader(http.StatusOK)
		json.NewEncoder(w).Encode(map[string]interface{}{
			"allowed":   true,
			"remaining": remaining,
		})
	} else {
		w.WriteHeader(http.StatusTooManyRequests)
		w.Header().Set("Retry-After", fmt.Sprintf("%d", int(retryAfter)))
		json.NewEncoder(w).Encode(map[string]interface{}{
			"allowed":     false,
			"remaining":   0,
			"retry_after": int(retryAfter),
		})
	}
}

func (rl *RateLimiter) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(map[string]string{"error": "method not allowed"})
		return
	}

	allowed, denied, keys := rl.getStats()
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"allowed": allowed,
		"denied":  denied,
		"keys":    keys,
	})
}

func (rl *RateLimiter) handleNotFound(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusNotFound)
	json.NewEncoder(w).Encode(map[string]string{"error": "not found"})
}

func main() {
	// Parse environment variables
	portStr := os.Getenv("PORT")
	if portStr == "" {
		portStr = "8080"
	}
	port, err := strconv.Atoi(portStr)
	if err != nil || port < 1 || port > 65535 {
		fmt.Fprintf(os.Stderr, "error: PORT must be a valid port number\n")
		os.Exit(2)
	}

	capacityStr := os.Getenv("CAPACITY")
	if capacityStr == "" {
		capacityStr = "10"
	}
	capacity, err := strconv.Atoi(capacityStr)
	if err != nil || capacity < 1 {
		fmt.Fprintf(os.Stderr, "error: CAPACITY must be a whole number >= 1\n")
		os.Exit(2)
	}

	refillPerSecStr := os.Getenv("REFILL_PER_SEC")
	if refillPerSecStr == "" {
		refillPerSecStr = "1"
	}
	refillPerSec, err := strconv.ParseFloat(refillPerSecStr, 64)
	if err != nil || refillPerSec <= 0 {
		fmt.Fprintf(os.Stderr, "error: REFILL_PER_SEC must be a decimal number > 0\n")
		os.Exit(2)
	}

	rl := NewRateLimiter(capacity, refillPerSec)

	// Set up HTTP handlers
	http.HandleFunc("/check", rl.handleCheck)
	http.HandleFunc("/stats", rl.handleStats)
	http.HandleFunc("/", rl.handleNotFound)

	addr := fmt.Sprintf("127.0.0.1:%d", port)
	log.Printf("Starting rate limiter on %s with CAPACITY=%d, REFILL_PER_SEC=%f\n", addr, capacity, refillPerSec)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
}
