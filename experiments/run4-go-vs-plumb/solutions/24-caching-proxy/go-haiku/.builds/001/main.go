package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"sync"
	"time"
)

type CacheEntry struct {
	Status      int
	Body        []byte
	ContentType string
	Timestamp   time.Time
}

type RequestResult struct {
	Status      int
	Body        []byte
	ContentType string
	Err         error
}

type CacheManager struct {
	mu              sync.RWMutex
	entries         map[string]*CacheEntry
	lru             []string // LRU order, oldest first
	maxEntries      int
	ttlMs           int64
	inFlightMu      sync.Mutex
	inFlight        map[string]*sync.Cond // condition variables for coalescing
	inFlightResult  map[string]*RequestResult
}

func NewCacheManager(maxEntries int, ttlMs int64) *CacheManager {
	return &CacheManager{
		entries:        make(map[string]*CacheEntry),
		lru:            []string{},
		maxEntries:     maxEntries,
		ttlMs:          ttlMs,
		inFlight:       make(map[string]*sync.Cond),
		inFlightResult: make(map[string]*RequestResult),
	}
}

func (cm *CacheManager) Get(key string) (*CacheEntry, bool) {
	cm.mu.RLock()
	defer cm.mu.RUnlock()

	entry, exists := cm.entries[key]
	if !exists {
		return nil, false
	}

	// Check TTL
	if time.Since(entry.Timestamp).Milliseconds() > cm.ttlMs {
		return nil, false
	}

	return entry, true
}

func (cm *CacheManager) Set(key string, entry *CacheEntry) {
	cm.mu.Lock()
	defer cm.mu.Unlock()

	// If key exists, remove from LRU order
	for i, k := range cm.lru {
		if k == key {
			cm.lru = append(cm.lru[:i], cm.lru[i+1:]...)
			break
		}
	}

	// If at capacity, remove LRU entry
	if len(cm.entries) >= cm.maxEntries && !exists(cm.entries, key) {
		if len(cm.lru) > 0 {
			lruKey := cm.lru[0]
			cm.lru = cm.lru[1:]
			delete(cm.entries, lruKey)
		}
	}

	cm.entries[key] = entry
	cm.lru = append(cm.lru, key)
}

func exists(m map[string]*CacheEntry, key string) bool {
	_, exists := m[key]
	return exists
}

func (cm *CacheManager) WaitForInflight(key string) bool {
	cm.inFlightMu.Lock()
	cond, inFlight := cm.inFlight[key]
	cm.inFlightMu.Unlock()

	if !inFlight {
		return false
	}

	cond.Wait()
	return true
}

func (cm *CacheManager) BeginInflight(key string) {
	cm.inFlightMu.Lock()
	defer cm.inFlightMu.Unlock()
	cm.inFlight[key] = sync.NewCond(&sync.Mutex{})
	cm.inFlightResult[key] = nil
}

func (cm *CacheManager) SetInflightResult(key string, result *RequestResult) {
	cm.inFlightMu.Lock()
	defer cm.inFlightMu.Unlock()
	cm.inFlightResult[key] = result
	if cond, exists := cm.inFlight[key]; exists {
		cond.Broadcast()
	}
}

func (cm *CacheManager) GetInflightResult(key string) (*RequestResult, bool) {
	cm.inFlightMu.Lock()
	defer cm.inFlightMu.Unlock()
	result, exists := cm.inFlightResult[key]
	if exists {
		delete(cm.inFlight, key)
		delete(cm.inFlightResult, key)
	}
	return result, exists
}

func (cm *CacheManager) IsInflight(key string) bool {
	cm.inFlightMu.Lock()
	defer cm.inFlightMu.Unlock()
	_, exists := cm.inFlight[key]
	return exists
}

func makeUpstreamRequest(client *http.Client, upstreamUrl *url.URL, requestURI string) (*http.Response, error) {
	// Construct upstream URL
	fullURL := upstreamUrl.Scheme + "://" + upstreamUrl.Host + upstreamUrl.Path + requestURI

	req, err := http.NewRequest(http.MethodGet, fullURL, nil)
	if err != nil {
		return nil, err
	}

	return client.Do(req)
}

func isTimeoutError(err error) bool {
	if err == context.DeadlineExceeded {
		return true
	}
	if netErr, ok := err.(net.Error); ok {
		return netErr.Timeout()
	}
	return false
}

func main() {
	// Parse environment variables
	port := os.Getenv("PORT")
	if port == "" {
		fmt.Fprintf(os.Stderr, "PORT environment variable is required\n")
		os.Exit(1)
	}

	upstream := os.Getenv("UPSTREAM")
	if upstream == "" {
		fmt.Fprintf(os.Stderr, "UPSTREAM environment variable is required\n")
		os.Exit(1)
	}

	cacheTtlMs := int64(60000)
	if v := os.Getenv("CACHE_TTL_MS"); v != "" {
		val, err := strconv.ParseInt(v, 10, 64)
		if err != nil || val < 1 {
			fmt.Fprintf(os.Stderr, "Invalid CACHE_TTL_MS\n")
			os.Exit(1)
		}
		cacheTtlMs = val
	}

	cacheMaxEntries := 1000
	if v := os.Getenv("CACHE_MAX_ENTRIES"); v != "" {
		val, err := strconv.Atoi(v)
		if err != nil || val < 1 {
			fmt.Fprintf(os.Stderr, "Invalid CACHE_MAX_ENTRIES\n")
			os.Exit(1)
		}
		cacheMaxEntries = val
	}

	upstreamTimeoutMs := int64(5000)
	if v := os.Getenv("UPSTREAM_TIMEOUT_MS"); v != "" {
		val, err := strconv.ParseInt(v, 10, 64)
		if err != nil || val < 1 {
			fmt.Fprintf(os.Stderr, "Invalid UPSTREAM_TIMEOUT_MS\n")
			os.Exit(1)
		}
		upstreamTimeoutMs = val
	}

	// Parse upstream URL
	upstreamUrl, err := url.Parse(upstream)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Invalid UPSTREAM URL: %v\n", err)
		os.Exit(1)
	}

	// Create cache manager
	cache := NewCacheManager(cacheMaxEntries, cacheTtlMs)

	// Create HTTP client with timeout
	client := &http.Client{
		Timeout: time.Duration(upstreamTimeoutMs) * time.Millisecond,
	}

	// Create HTTP handler
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusMethodNotAllowed)
			json.NewEncoder(w).Encode(map[string]string{"error": "method not allowed"})
			return
		}

		// Construct cache key (path + query exactly as received)
		cacheKey := r.RequestURI

		// Check cache first
		if entry, isCacheHit := cache.Get(cacheKey); isCacheHit {
			w.Header().Set("Content-Type", entry.ContentType)
			w.Header().Set("X-Cache", "HIT")
			w.WriteHeader(entry.Status)
			w.Write(entry.Body)
			return
		}

		// Check if request is already in-flight
		if cache.IsInflight(cacheKey) {
			// Wait for the in-flight request to complete
			cache.WaitForInflight(cacheKey)
			if result, exists := cache.GetInflightResult(cacheKey); exists {
				if result.Err != nil {
					// Return error
					w.Header().Set("Content-Type", "application/json")
					w.Header().Set("X-Cache", "HIT")
					if isTimeoutError(result.Err) {
						w.WriteHeader(http.StatusGatewayTimeout)
						json.NewEncoder(w).Encode(map[string]string{"error": "upstream timeout"})
					} else {
						w.WriteHeader(http.StatusBadGateway)
						json.NewEncoder(w).Encode(map[string]string{"error": "upstream unavailable"})
					}
				} else {
					w.Header().Set("Content-Type", result.ContentType)
					w.Header().Set("X-Cache", "HIT")
					w.WriteHeader(result.Status)
					w.Write(result.Body)
				}
			}
			return
		}

		// Mark as in-flight
		cache.BeginInflight(cacheKey)

		// Make upstream request
		upstreamResp, err := makeUpstreamRequest(client, upstreamUrl, r.RequestURI)

		if err != nil {
			result := &RequestResult{Err: err}
			cache.SetInflightResult(cacheKey, result)
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("X-Cache", "MISS")
			if isTimeoutError(err) {
				w.WriteHeader(http.StatusGatewayTimeout)
				json.NewEncoder(w).Encode(map[string]string{"error": "upstream timeout"})
			} else {
				w.WriteHeader(http.StatusBadGateway)
				json.NewEncoder(w).Encode(map[string]string{"error": "upstream unavailable"})
			}
			return
		}

		// Read body
		body, _ := io.ReadAll(upstreamResp.Body)
		upstreamResp.Body.Close()

		contentType := upstreamResp.Header.Get("Content-Type")

		// Cache if 200
		if upstreamResp.StatusCode == http.StatusOK {
			entry := &CacheEntry{
				Status:      upstreamResp.StatusCode,
				Body:        body,
				ContentType: contentType,
				Timestamp:   time.Now(),
			}
			cache.Set(cacheKey, entry)
			result := &RequestResult{
				Status:      upstreamResp.StatusCode,
				Body:        body,
				ContentType: contentType,
			}
			cache.SetInflightResult(cacheKey, result)
		} else if upstreamResp.StatusCode >= 500 {
			result := &RequestResult{
				Status:      http.StatusBadGateway,
				ContentType: "application/json",
			}
			cache.SetInflightResult(cacheKey, result)
		} else {
			result := &RequestResult{
				Status:      upstreamResp.StatusCode,
				Body:        body,
				ContentType: contentType,
			}
			cache.SetInflightResult(cacheKey, result)
		}

		// Return response
		if upstreamResp.StatusCode >= 500 {
			w.Header().Set("Content-Type", "application/json")
			w.Header().Set("X-Cache", "MISS")
			w.WriteHeader(http.StatusBadGateway)
			json.NewEncoder(w).Encode(map[string]interface{}{
				"error":  "upstream error",
				"status": upstreamResp.StatusCode,
			})
		} else {
			w.Header().Set("Content-Type", contentType)
			w.Header().Set("X-Cache", "MISS")
			w.WriteHeader(upstreamResp.StatusCode)
			w.Write(body)
		}
	})

	// Start server
	listener, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Failed to listen: %v\n", err)
		os.Exit(1)
	}

	server := &http.Server{
		Handler: handler,
	}

	server.Serve(listener)
}
