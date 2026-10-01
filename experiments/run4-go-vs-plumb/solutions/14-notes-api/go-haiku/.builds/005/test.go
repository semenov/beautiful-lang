package main

import (
	"fmt"
	"strings"
)

func main() {
	query := "%"
	escapedQuery := strings.NewReplacer("\\", "\\\\", "%", "\\%", "_", "\\_").Replace(query)
	pattern := "%" + escapedQuery + "%"
	fmt.Printf("Query: %q\n", query)
	fmt.Printf("Escaped: %q\n", escapedQuery)
	fmt.Printf("Pattern: %q\n", pattern)
	fmt.Printf("Pattern (string): %s\n", pattern)
}
