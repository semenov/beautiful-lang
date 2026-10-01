package main

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"sort"
	"strconv"
	"sync"
)

type Sample struct {
	Name  string      `json:"name"`
	Value json.Number `json:"value"`
	Ts    int64       `json:"ts"`
}

type MetricsResponse struct {
	Accepted int64 `json:"accepted"`
	Dropped  int64 `json:"dropped"`
}

type AggregateResponse struct {
	Name  string       `json:"name"`
	Count int64        `json:"count"`
	Sum   float64      `json:"sum"`
	Min   *float64     `json:"min"`
	Max   *float64     `json:"max"`
	P50   *float64     `json:"p50"`
	P99   *float64     `json:"p99"`
}

type StatsResponse struct {
	Accepted int64  `json:"accepted"`
	Dropped  int64  `json:"dropped"`
	Now      *int64 `json:"now"`
}

type ErrorResponse struct {
	Error string `json:"error"`
}

type Server struct {
	mu        sync.Mutex
	windowMs  int64
	now       *int64
	samples   map[string][]Sample // samples per metric name
	accepted  int64
	dropped   int64
}

func NewServer(windowMs int64) *Server {
	return &Server{
		windowMs: windowMs,
		samples:  make(map[string][]Sample),
	}
}

func (s *Server) processSample(sample Sample) bool {
	// Validate sample
	if sample.Name == "" {
		return false
	}

	// Parse value as float64
	val, err := strconv.ParseFloat(sample.Value.String(), 64)
	if err != nil {
		return false
	}

	// Check ts is non-negative
	if sample.Ts < 0 {
		return false
	}

	// Update now
	if s.now == nil {
		s.now = &sample.Ts
	} else {
		if sample.Ts > *s.now {
			*s.now = sample.Ts
		}
	}

	// Check if late
	if sample.Ts <= *s.now-s.windowMs {
		s.dropped++
		return false
	}

	// Accept sample
	s.samples[sample.Name] = append(s.samples[sample.Name], Sample{
		Name:  sample.Name,
		Value: json.Number(fmt.Sprintf("%v", val)),
		Ts:    sample.Ts,
	})
	s.accepted++
	return true
}

func (s *Server) handleMetrics(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid request body"})
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	initialAccepted := s.accepted
	initialDropped := s.dropped

	// Try to parse as a single object first
	var single Sample
	singleErr := json.Unmarshal(body, &single)

	var samples []Sample
	arrayErr := json.Unmarshal(body, &samples)

	// If neither single nor array parsed successfully
	if singleErr != nil && arrayErr != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid JSON"})
		return
	}

	// Determine if it's a single sample or array
	isSingle := singleErr == nil && single.Name != ""
	isArray := arrayErr == nil && len(samples) > 0

	// Handle single sample case
	if isSingle && !isArray {
		// Validate the single sample
		if single.Name == "" {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "name is missing or empty"})
			return
		}
		if single.Value == "" {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "value is missing"})
			return
		}
		_, parseErr := strconv.ParseFloat(single.Value.String(), 64)
		if parseErr != nil {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "value is not a number"})
			return
		}
		if single.Ts < 0 {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "ts is negative"})
			return
		}

		// Process the single sample
		s.processSample(single)
	} else if arrayErr == nil {
		// It's an array - validate all samples first
		if len(samples) == 0 {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid JSON"})
			return
		}

		for _, sample := range samples {
			if sample.Name == "" {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(ErrorResponse{Error: "name is missing or empty"})
				return
			}
			if sample.Value == "" {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(ErrorResponse{Error: "value is missing"})
				return
			}
			_, parseErr := strconv.ParseFloat(sample.Value.String(), 64)
			if parseErr != nil {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(ErrorResponse{Error: "value is not a number"})
				return
			}
			if sample.Ts < 0 {
				w.WriteHeader(http.StatusBadRequest)
				json.NewEncoder(w).Encode(ErrorResponse{Error: "ts is negative"})
				return
			}
		}

		// Process all samples in the array
		for _, sample := range samples {
			s.processSample(sample)
		}
	} else {
		// Can't determine what this is
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "invalid JSON"})
		return
	}

	accepted := s.accepted - initialAccepted
	dropped := s.dropped - initialDropped

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(MetricsResponse{
		Accepted: accepted,
		Dropped:  dropped,
	})
}

func (s *Server) handleAggregate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	name := r.URL.Query().Get("name")
	if name == "" {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "name parameter is required"})
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	samples := s.samples[name]

	// Filter samples in window
	var windowSamples []float64
	for _, sample := range samples {
		if s.now != nil && sample.Ts > *s.now-s.windowMs && sample.Ts <= *s.now {
			val, _ := strconv.ParseFloat(sample.Value.String(), 64)
			windowSamples = append(windowSamples, val)
		}
	}

	resp := AggregateResponse{Name: name}

	if len(windowSamples) == 0 {
		resp.Count = 0
		resp.Sum = 0
		resp.Min = nil
		resp.Max = nil
		resp.P50 = nil
		resp.P99 = nil
	} else {
		resp.Count = int64(len(windowSamples))

		// Calculate sum
		sum := 0.0
		for _, v := range windowSamples {
			sum += v
		}
		resp.Sum = sum

		// Sort for min, max, percentiles
		sort.Float64s(windowSamples)

		min := windowSamples[0]
		resp.Min = &min
		max := windowSamples[len(windowSamples)-1]
		resp.Max = &max

		// Calculate p50 using nearest-rank method
		p50Pos := int(math.Ceil(50.0 / 100.0 * float64(len(windowSamples))))
		if p50Pos > 0 {
			p50Val := windowSamples[p50Pos-1]
			resp.P50 = &p50Val
		}

		// Calculate p99 using nearest-rank method
		p99Pos := int(math.Ceil(99.0 / 100.0 * float64(len(windowSamples))))
		if p99Pos > 0 {
			p99Val := windowSamples[p99Pos-1]
			resp.P99 = &p99Val
		}
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	resp := StatsResponse{
		Accepted: s.accepted,
		Dropped:  s.dropped,
		Now:      s.now,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func main() {
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	windowMsStr := os.Getenv("WINDOW_MS")
	windowMs := int64(60000)
	if windowMsStr != "" {
		if parsed, err := strconv.ParseInt(windowMsStr, 10, 64); err == nil && parsed >= 1 {
			windowMs = parsed
		}
	}

	server := NewServer(windowMs)

	http.HandleFunc("/metrics", server.handleMetrics)
	http.HandleFunc("/aggregate", server.handleAggregate)
	http.HandleFunc("/stats", server.handleStats)
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
	})

	fmt.Printf("Starting server on 127.0.0.1:%s with window %dms\n", port, windowMs)
	http.ListenAndServe("127.0.0.1:"+port, nil)
}
