package main

import (
	"encoding/csv"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
	"unicode"
)

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: app --by COLUMN [FILE]\n")
	}

	// Parse --by flag
	byColumn := flag.String("by", "", "column to group by")
	flag.Parse()

	if *byColumn == "" {
		fmt.Fprintf(os.Stderr, "usage: app --by COLUMN [FILE]\n")
		os.Exit(64)
	}

	args := flag.Args()
	if len(args) > 1 {
		fmt.Fprintf(os.Stderr, "usage: app --by COLUMN [FILE]\n")
		os.Exit(64)
	}

	var input io.Reader
	if len(args) == 0 {
		input = os.Stdin
	} else {
		file, err := os.Open(args[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", args[0])
			os.Exit(2)
		}
		defer file.Close()
		input = file
	}

	// Process the CSV
	exitCode := processCSV(input, *byColumn)
	os.Exit(exitCode)
}

func processCSV(input io.Reader, byColumn string) int {
	reader := csv.NewReader(input)
	reader.FieldsPerRecord = -1 // Don't check field count; we'll do it manually
	
	// Track if we've seen the header
	var headers []string
	var amountIdx, byIdx int
	var records map[string]int64 = make(map[string]int64)
	var lineNum int
	var hasError bool

	for {
		record, err := reader.Read()
		lineNum++

		if err != nil {
			if err == io.EOF {
				break
			}
			// Other errors (e.g., unterminated quote)
			fmt.Fprintf(os.Stderr, "line %d: unterminated quote\n", lineNum)
			hasError = true
			continue
		}

		// Skip empty lines
		if len(record) == 1 && record[0] == "" {
			lineNum--
			continue
		}

		// First record is the header
		if headers == nil {
			headers = record

			// Find the column indices
			amountIdx = -1
			byIdx = -1
			for i, h := range headers {
				if h == "amount" {
					amountIdx = i
				}
				if h == byColumn {
					byIdx = i
				}
			}

			if amountIdx == -1 {
				fmt.Fprintf(os.Stderr, "error: missing column amount\n")
				os.Exit(2)
			}
			if byIdx == -1 {
				fmt.Fprintf(os.Stderr, "error: missing column %s\n", byColumn)
				os.Exit(2)
			}
			continue
		}

		// Check field count
		if len(record) != len(headers) {
			fmt.Fprintf(os.Stderr, "line %d: wrong number of fields\n", lineNum)
			hasError = true
			continue
		}

		// Parse amount
		amountStr := record[amountIdx]
		cents, valid := parseAmount(amountStr)
		if !valid {
			fmt.Fprintf(os.Stderr, "line %d: invalid amount\n", lineNum)
			hasError = true
			continue
		}

		// Add to group
		groupValue := record[byIdx]
		records[groupValue] += cents
	}

	// Check if headers was set
	if headers == nil {
		fmt.Fprintf(os.Stderr, "error: empty input\n")
		os.Exit(2)
	}

	// Output results
	fmt.Printf("%s,total\n", byColumn)

	// Sort by group value
	var keys []string
	for k := range records {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	for _, key := range keys {
		cents := records[key]
		total := formatAmount(cents)
		quotedKey := quoteField(key)
		fmt.Printf("%s,%s\n", quotedKey, total)
	}

	if hasError {
		return 1
	}
	return 0
}

// parseAmount parses an amount string and returns cents and whether it's valid
func parseAmount(s string) (int64, bool) {
	// Remove leading and trailing ASCII spaces
	s = strings.TrimFunc(s, func(r rune) bool {
		return r == ' ' || r == '\t'
	})

	if s == "" {
		return 0, false
	}

	// Optional minus sign
	isNegative := false
	if s[0] == '-' {
		isNegative = true
		s = s[1:]
	}

	if s == "" {
		return 0, false
	}

	// Check for leading dot (invalid)
	if s[0] == '.' {
		return 0, false
	}

	// Split by decimal point
	parts := strings.Split(s, ".")
	if len(parts) > 2 {
		return 0, false
	}

	intPart := parts[0]
	decPart := ""
	hasDot := false
	if len(parts) == 2 {
		decPart = parts[1]
		hasDot = true
	}

	// Validate integer part
	if intPart == "" {
		return 0, false
	}
	for _, c := range intPart {
		if !unicode.IsDigit(c) {
			return 0, false
		}
	}

	// Validate decimal part (1-2 digits if decimal point is present)
	if hasDot {
		if len(decPart) < 1 || len(decPart) > 2 {
			return 0, false
		}
		for _, c := range decPart {
			if !unicode.IsDigit(c) {
				return 0, false
			}
		}
	}

	// Parse to cents
	intVal, _ := strconv.ParseInt(intPart, 10, 64)
	cents := intVal * 100

	if decPart != "" {
		// Pad to 2 digits
		if len(decPart) == 1 {
			decPart += "0"
		}
		decVal, _ := strconv.ParseInt(decPart, 10, 64)
		cents += decVal
	}

	if isNegative {
		cents = -cents
	}

	return cents, true
}

// formatAmount formats cents as a decimal string with exactly 2 decimals
func formatAmount(cents int64) string {
	isNegative := cents < 0
	if isNegative {
		cents = -cents
	}

	dollars := cents / 100
	remainingCents := cents % 100

	result := fmt.Sprintf("%d.%02d", dollars, remainingCents)
	if isNegative {
		result = "-" + result
	}

	return result
}

// quoteField quotes a CSV field if necessary
func quoteField(field string) string {
	// Check if field needs quoting
	needsQuote := false
	for _, c := range field {
		if c == ',' || c == '"' || c == '\r' || c == '\n' {
			needsQuote = true
			break
		}
	}

	if !needsQuote {
		return field
	}

	// Quote and escape internal quotes
	result := "\""
	for _, c := range field {
		if c == '"' {
			result += "\"\""
		} else {
			result += string(c)
		}
	}
	result += "\""
	return result
}
