package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strings"
)

func main() {
	// Parse command-line arguments
	fs := flag.NewFlagSet("app", flag.ContinueOnError)
	fs.Usage = func() {} // Suppress default usage

	var whereStrs whereStringSlice
	var selectStr string

	fs.Var(&whereStrs, "where", "Filter condition PATH=VALUE")
	fs.StringVar(&selectStr, "select", "", "Comma-separated paths to select")

	err := fs.Parse(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "usage: app [--where PATH=VALUE]... [--select PATH,PATH...]\n")
		os.Exit(64)
	}

	// Parse --where conditions
	whereConditions, err := parseWhereConditions(whereStrs)
	if err != nil {
		fmt.Fprintf(os.Stderr, "usage: %s\n", err)
		os.Exit(64)
	}

	// Parse --select paths
	var selectPaths []string
	if selectStr != "" {
		selectPaths, err = parseSelectPaths(selectStr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "usage: %s\n", err)
			os.Exit(64)
		}
	}

	// Process input
	hasError := false
	scanner := bufio.NewScanner(os.Stdin)
	lineNum := 0

	for scanner.Scan() {
		line := scanner.Text()
		lineNum++

		// Skip empty or whitespace-only lines
		if strings.TrimSpace(line) == "" {
			continue
		}

		// Parse JSON
		var obj map[string]interface{}
		err := json.Unmarshal([]byte(line), &obj)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: line %d: invalid JSON\n", lineNum)
			hasError = true
			continue
		}

		// Check if it's actually an object (not an array or primitive)
		if obj == nil {
			fmt.Fprintf(os.Stderr, "error: line %d: not an object\n", lineNum)
			hasError = true
			continue
		}

		// Check where conditions
		matches := true
		for _, cond := range whereConditions {
			if !matchesCondition(obj, cond, line) {
				matches = false
				break
			}
		}

		if !matches {
			continue
		}

		// Output
		if len(selectPaths) == 0 {
			// Output whole record - just use original line
			fmt.Println(line)
		} else {
			// Output selected fields
			output := formatSelected(obj, selectPaths, line)
			fmt.Println(output)
		}
	}

	if scanner.Err() != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", scanner.Err())
		hasError = true
	}

	if hasError {
		os.Exit(1)
	}
}

type whereStringSlice []string

func (w *whereStringSlice) String() string {
	return strings.Join(*w, ",")
}

func (w *whereStringSlice) Set(value string) error {
	*w = append(*w, value)
	return nil
}

type WhereCondition struct {
	path  []string
	value string
}

func parseWhereConditions(whereStrs []string) ([]WhereCondition, error) {
	var conditions []WhereCondition
	for _, s := range whereStrs {
		idx := strings.Index(s, "=")
		if idx == -1 {
			return nil, fmt.Errorf("--where without =")
		}
		pathStr := s[:idx]
		valueStr := s[idx+1:]

		path, err := parsePath(pathStr)
		if err != nil {
			return nil, err
		}

		conditions = append(conditions, WhereCondition{path: path, value: valueStr})
	}
	return conditions, nil
}

func parseSelectPaths(selectStr string) ([]string, error) {
	paths := strings.Split(selectStr, ",")
	var result []string
	for _, p := range paths {
		p = strings.TrimSpace(p)
		if p == "" {
			return nil, fmt.Errorf("empty path in --select")
		}
		_, err := parsePath(p)
		if err != nil {
			return nil, err
		}
		result = append(result, p)
	}
	return result, nil
}

func parsePath(pathStr string) ([]string, error) {
	if pathStr == "" {
		return nil, fmt.Errorf("empty path")
	}
	parts := strings.Split(pathStr, ".")
	for _, part := range parts {
		if part == "" {
			return nil, fmt.Errorf("empty key in path")
		}
	}
	return parts, nil
}

func getValueAtPath(obj map[string]interface{}, path []string) interface{} {
	current := interface{}(obj)
	for _, key := range path {
		m, ok := current.(map[string]interface{})
		if !ok {
			return nil
		}
		val, exists := m[key]
		if !exists {
			return nil
		}
		current = val
	}
	return current
}

func matchesCondition(obj map[string]interface{}, cond WhereCondition, origLine string) bool {
	val := getValueAtPath(obj, cond.path)
	if val == nil {
		return false
	}

	// Check type and match
	switch v := val.(type) {
	case string:
		// String must match exactly after unescaping
		return v == cond.value

	case float64:
		// For numbers, we need to match the JSON text representation
		// Try to extract from the original line to preserve exact format
		origText := getNumberTextFromLine(origLine, cond.path)
		if origText != "" {
			return origText == cond.value
		}
		// Fallback to JSON representation
		jsonText := fmt.Sprintf("%g", v)
		return jsonText == cond.value

	case bool:
		if v {
			return cond.value == "true"
		}
		return cond.value == "false"

	case nil:
		return cond.value == "null"

	default:
		// Objects and arrays never match
		return false
	}
}

func getNumberTextFromLine(line string, path []string) string {
	// Parse with json.Number to preserve number text
	decoder := json.NewDecoder(strings.NewReader(line))
	decoder.UseNumber()

	var objWithNumbers map[string]interface{}
	err := decoder.Decode(&objWithNumbers)
	if err != nil {
		return ""
	}

	current := interface{}(objWithNumbers)
	for i, key := range path {
		m, ok := current.(map[string]interface{})
		if !ok {
			return ""
		}
		val, exists := m[key]
		if !exists {
			return ""
		}

		if i == len(path)-1 {
			// Last element
			if n, ok := val.(json.Number); ok {
				return string(n)
			}
			return ""
		}

		current = val
	}
	return ""
}

func formatSelected(obj map[string]interface{}, selectPaths []string, origLine string) string {
	// Build output as compact JSON
	var buf strings.Builder
	buf.WriteString("{")

	for i, pathStr := range selectPaths {
		if i > 0 {
			buf.WriteString(",")
		}

		// Add key
		buf.WriteString("\"")
		buf.WriteString(escapeJSONString(pathStr))
		buf.WriteString("\":")

		// Get value
		path, _ := parsePath(pathStr)
		val := getValueAtPath(obj, path)

		// Add value with number preservation
		path, _ = parsePath(pathStr)
		numText := getNumberTextFromLine(origLine, path)
		buf.WriteString(formatValue(val, numText))
	}

	buf.WriteString("}")
	return buf.String()
}

func formatValue(val interface{}, numText string) string {
	if val == nil {
		return "null"
	}

	switch v := val.(type) {
	case string:
		return "\"" + escapeJSONString(v) + "\""
	case float64:
		if numText != "" {
			return numText
		}
		// Fallback
		return fmt.Sprintf("%g", v)
	case bool:
		if v {
			return "true"
		}
		return "false"
	case map[string]interface{}:
		// Objects can't be output from --select
		return "null"
	case []interface{}:
		// Arrays can't be output from --select
		return "null"
	default:
		return "null"
	}
}

func escapeJSONString(s string) string {
	result := ""
	for _, r := range s {
		switch r {
		case '"':
			result += "\\\""
		case '\\':
			result += "\\\\"
		case '\b':
			result += "\\b"
		case '\t':
			result += "\\t"
		case '\n':
			result += "\\n"
		case '\f':
			result += "\\f"
		case '\r':
			result += "\\r"
		default:
			if r < 0x20 {
				result += fmt.Sprintf("\\u%04x", r)
			} else {
				result += string(r)
			}
		}
	}
	return result
}
