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

		// Parse JSON with number preservation
		var rawObj map[string]json.Number
		err := json.Unmarshal([]byte(line), &rawObj)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: line %d: invalid JSON\n", lineNum)
			hasError = true
			continue
		}

		// Parse again to get typed values for comparison
		var obj map[string]interface{}
		err = json.Unmarshal([]byte(line), &obj)
		if err != nil {
			// Check if it's because it's not an object
			var val interface{}
			json.Unmarshal([]byte(line), &val)
			fmt.Fprintf(os.Stderr, "error: line %d: not an object\n", lineNum)
			hasError = true
			continue
		}

		// Check where conditions
		matches := true
		for _, cond := range whereConditions {
			if !matchesCondition(obj, cond) {
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
			// Output selected fields with number preservation
			output := formatSelected(obj, rawObj, selectPaths, line)
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

func matchesCondition(obj map[string]interface{}, cond WhereCondition) bool {
	val := getValueAtPath(obj, cond.path)
	if val == nil {
		return false
	}

	// Check type and match
	switch v := val.(type) {
	case string:
		// Unescape the string and compare
		var unescaped string
		err := json.Unmarshal([]byte("\""+strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(v)+`"`), &unescaped)
		if err != nil {
			// If unescaping fails, just compare directly
			return v == cond.value
		}
		return unescaped == cond.value

	case float64:
		// For numbers, we need to compare JSON text representation
		// This is tricky since we've lost the original text
		// JSON unmarshaling converts numbers to float64
		// We'll compare the JSON text representation
		jsonText := fmt.Sprintf("%g", v)
		return jsonText == cond.value

	case bool:
		jsonText := fmt.Sprintf("%v", v)
		return jsonText == cond.value

	case nil:
		return cond.value == "null"

	default:
		// Objects and arrays never match
		return false
	}
}

func formatSelected(obj map[string]interface{}, rawObj map[string]json.Number, selectPaths []string, origLine string) string {
	// Build output preserving number formats
	parts := []string{"{"}

	for i, pathStr := range selectPaths {
		if i > 0 {
			parts = append(parts, ",")
		}

		// Add key
		parts = append(parts, "\"")
		parts = append(parts, escapeJSONString(pathStr))
		parts = append(parts, "\":")

		// Get value
		path, _ := parsePath(pathStr)
		val := getValueAtPath(obj, path)
		rawVal := getValueAtPathRaw(rawObj, path)

		// Add value
		parts = append(parts, formatValue(val, rawVal))
	}

	parts = append(parts, "}")
	return strings.Join(parts, "")
}

func getValueAtPathRaw(obj map[string]json.Number, path []string) json.Number {
	if len(path) == 0 {
		return ""
	}

	current := interface{}(obj)
	for i, key := range path {
		if i == len(path)-1 {
			// Last key - get from raw map
			m, ok := current.(map[string]json.Number)
			if !ok {
				return ""
			}
			val, exists := m[key]
			if !exists {
				return ""
			}
			return val
		}

		// Intermediate key - need to traverse through typed object
		// This is tricky with json.Number
		return ""
	}
	return ""
}

func formatValue(val interface{}, rawVal json.Number) string {
	if val == nil {
		return "null"
	}

	switch v := val.(type) {
	case string:
		return "\"" + escapeJSONString(v) + "\""
	case float64:
		if rawVal != "" {
			return string(rawVal)
		}
		// Fallback
		return fmt.Sprintf("%g", v)
	case bool:
		if v {
			return "true"
		}
		return "false"
	case map[string]interface{}:
		// Can't output objects
		return "null"
	case []interface{}:
		// Can't output arrays
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
