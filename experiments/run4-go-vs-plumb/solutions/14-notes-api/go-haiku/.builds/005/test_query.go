package main

import (
	"fmt"
	"net/url"
	"strings"
)

func main() {
	// Test URL query parsing
	u, _ := url.Parse("http://127.0.0.1/notes?q=%25")
	query := u.Query().Get("q")
	fmt.Printf("Raw query: %q\n", query)
	
	// Decode
	decoded, _ := url.QueryUnescape(query)
	fmt.Printf("Decoded: %q\n", decoded)
	
	// Escape for LIKE
	escaped := strings.NewReplacer("\\", "\\\\", "%", "\\%", "_", "\\_").Replace(decoded)
	fmt.Printf("Escaped: %q\n", escaped)
	
	// Pattern
	pattern := "%" + escaped + "%"
	fmt.Printf("Pattern: %q\n", pattern)
}
