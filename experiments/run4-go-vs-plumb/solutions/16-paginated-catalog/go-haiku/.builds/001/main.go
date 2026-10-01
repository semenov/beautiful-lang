package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strconv"
	"strings"
)

// Product represents a single product in the catalog
type Product struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Category   string `json:"category"`
	PriceCents int    `json:"price_cents"`
	InStock    bool   `json:"in_stock"`
}

// ProductsResponse is the JSON response for GET /products
type ProductsResponse struct {
	Items      []Product `json:"items"`
	NextCursor *string   `json:"next_cursor"`
}

// ErrorResponse is the JSON response for errors
type ErrorResponse struct {
	Error string `json:"error"`
}

var catalog []Product
var catalogByID map[string]Product

func main() {
	// Get environment variables
	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}
	catalogPath := os.Getenv("CATALOG_PATH")
	if catalogPath == "" {
		fmt.Fprintf(os.Stderr, "error: CATALOG_PATH not set\n")
		os.Exit(1)
	}

	// Load catalog
	if err := loadCatalog(catalogPath); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	// Setup HTTP handlers
	http.HandleFunc("/products", handleProducts)
	http.HandleFunc("/products/", handleProductDetail)
	http.HandleFunc("/", handle404)

	// Start server
	addr := "127.0.0.1:" + port
	if err := http.ListenAndServe(addr, nil); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func loadCatalog(path string) error {
	file, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("failed to open catalog: %v", err)
	}
	defer file.Close()

	data, err := io.ReadAll(file)
	if err != nil {
		return fmt.Errorf("failed to read catalog: %v", err)
	}

	var products []Product
	if err := json.Unmarshal(data, &products); err != nil {
		return fmt.Errorf("invalid JSON in catalog: %v", err)
	}

	// Validate products
	catalogByID = make(map[string]Product)
	for _, p := range products {
		if err := validateProduct(p); err != nil {
			return err
		}
		if _, exists := catalogByID[p.ID]; exists {
			return fmt.Errorf("duplicate product ID: %s", p.ID)
		}
		catalogByID[p.ID] = p
	}

	catalog = products
	return nil
}

func validateProduct(p Product) error {
	if p.ID == "" {
		return fmt.Errorf("product has empty ID")
	}
	if !isValidID(p.ID) {
		return fmt.Errorf("product has invalid ID: %s", p.ID)
	}
	if p.PriceCents < 0 {
		return fmt.Errorf("product has negative price: %s", p.ID)
	}
	return nil
}

func isValidID(id string) bool {
	for _, ch := range id {
		if !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
			return false
		}
	}
	return true
}

func handle404(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusNotFound)
	json.NewEncoder(w).Encode(ErrorResponse{Error: "not found"})
}

func handleProducts(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	// Parse query parameters
	q := r.URL.Query()

	// Parse category filter
	category := q.Get("category")

	// Parse price filters
	minPrice := 0
	if mp := q.Get("min_price"); mp != "" {
		if v, err := strconv.Atoi(mp); err != nil || v < 0 {
			sendError(w, http.StatusBadRequest, "invalid parameter: min_price")
			return
		} else {
			minPrice = v
		}
	}

	maxPrice := 2147483647 // large number
	if mp := q.Get("max_price"); mp != "" {
		if v, err := strconv.Atoi(mp); err != nil || v < 0 {
			sendError(w, http.StatusBadRequest, "invalid parameter: max_price")
			return
		} else {
			maxPrice = v
		}
	}

	if minPrice > maxPrice {
		sendError(w, http.StatusBadRequest, "invalid parameter: max_price")
		return
	}

	// Parse in_stock filter
	var inStockFilter *bool
	if is := q.Get("in_stock"); is != "" {
		switch is {
		case "true":
			t := true
			inStockFilter = &t
		case "false":
			f := false
			inStockFilter = &f
		default:
			sendError(w, http.StatusBadRequest, "invalid parameter: in_stock")
			return
		}
	}

	// Parse sort
	sortBy := "id" // default
	if s := q.Get("sort"); s != "" {
		switch s {
		case "id", "price_asc", "price_desc", "name":
			sortBy = s
		default:
			sendError(w, http.StatusBadRequest, "invalid parameter: sort")
			return
		}
	}

	// Parse limit
	limit := 20 // default
	if l := q.Get("limit"); l != "" {
		if v, err := strconv.Atoi(l); err != nil || v < 1 || v > 100 {
			sendError(w, http.StatusBadRequest, "invalid parameter: limit")
			return
		} else {
			limit = v
		}
	}

	// Parse cursor
	cursor := q.Get("cursor")

	// Filter and sort products
	var filtered []Product
	for _, p := range catalog {
		if category != "" && p.Category != category {
			continue
		}
		if p.PriceCents < minPrice || p.PriceCents > maxPrice {
			continue
		}
		if inStockFilter != nil && p.InStock != *inStockFilter {
			continue
		}
		filtered = append(filtered, p)
	}

	// Sort the filtered results
	sort.Slice(filtered, func(i, j int) bool {
		return compareProducts(filtered[i], filtered[j], sortBy) < 0
	})

	// Handle cursor
	startIdx := 0
	if cursor != "" {
		decodedCursor, err := base64.StdEncoding.DecodeString(cursor)
		if err != nil {
			sendError(w, http.StatusBadRequest, "invalid parameter: cursor")
			return
		}

		// Cursor format: "params_hash,index"
		parts := strings.SplitN(string(decodedCursor), ",", 2)
		if len(parts) != 2 {
			sendError(w, http.StatusBadRequest, "invalid parameter: cursor")
			return
		}

		expectedParamsHash := computeParamsHash(category, minPrice, maxPrice, inStockFilter, sortBy)
		if parts[0] != expectedParamsHash {
			sendError(w, http.StatusBadRequest, "invalid parameter: cursor")
			return
		}

		idx, err := strconv.Atoi(parts[1])
		if err != nil || idx < 0 || idx >= len(filtered) {
			sendError(w, http.StatusBadRequest, "invalid parameter: cursor")
			return
		}
		startIdx = idx
	}

	// Build response
	endIdx := startIdx + limit
	if endIdx > len(filtered) {
		endIdx = len(filtered)
	}

	items := filtered[startIdx:endIdx]
	var nextCursor *string
	if endIdx < len(filtered) {
		paramsHash := computeParamsHash(category, minPrice, maxPrice, inStockFilter, sortBy)
		cursorStr := base64.StdEncoding.EncodeToString([]byte(paramsHash + "," + strconv.Itoa(endIdx)))
		nextCursor = &cursorStr
	}

	response := ProductsResponse{
		Items:      items,
		NextCursor: nextCursor,
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(response)
}

func handleProductDetail(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusMethodNotAllowed)
		json.NewEncoder(w).Encode(ErrorResponse{Error: "method not allowed"})
		return
	}

	// Extract product ID from path
	id := strings.TrimPrefix(r.URL.Path, "/products/")
	if id == "" || id == "products" {
		sendError(w, http.StatusNotFound, "not found")
		return
	}

	// Validate that id doesn't contain slashes
	if strings.Contains(id, "/") {
		sendError(w, http.StatusNotFound, "not found")
		return
	}

	// URL decode the ID
	decodedID, err := url.QueryUnescape(id)
	if err != nil {
		sendError(w, http.StatusNotFound, "not found")
		return
	}

	// Look up product
	product, exists := catalogByID[decodedID]
	if !exists {
		sendError(w, http.StatusNotFound, "not found")
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(product)
}

func compareProducts(a, b Product, sortBy string) int {
	var cmp int
	switch sortBy {
	case "id":
		cmp = strings.Compare(a.ID, b.ID)
	case "name":
		cmp = strings.Compare(a.Name, b.Name)
	case "price_asc":
		if a.PriceCents < b.PriceCents {
			cmp = -1
		} else if a.PriceCents > b.PriceCents {
			cmp = 1
		}
	case "price_desc":
		if a.PriceCents > b.PriceCents {
			cmp = -1
		} else if a.PriceCents < b.PriceCents {
			cmp = 1
		}
	}

	// Tie-break by ID ascending
	if cmp == 0 {
		cmp = strings.Compare(a.ID, b.ID)
	}

	return cmp
}

func computeParamsHash(category string, minPrice, maxPrice int, inStockFilter *bool, sortBy string) string {
	var inStockStr string
	if inStockFilter == nil {
		inStockStr = "nil"
	} else if *inStockFilter {
		inStockStr = "true"
	} else {
		inStockStr = "false"
	}
	return fmt.Sprintf("%s:%d:%d:%s:%s", category, minPrice, maxPrice, inStockStr, sortBy)
}

func sendError(w http.ResponseWriter, status int, message string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(ErrorResponse{Error: message})
}
