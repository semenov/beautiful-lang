package main

import (
	"fmt"
	"net/url"
)

func main() {
	// Test URL query decoding
	tests := []string{"%25", "%", "_", "%5C"}
	for _, test := range tests {
		decoded, _ := url.QueryUnescape(test)
		fmt.Printf("QueryUnescape(%q) = %q\n", test, decoded)
	}
}
