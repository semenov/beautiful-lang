package main

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"os"
	"sort"
	"strings"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "usage: %s FILE...\n", os.Args[0])
		os.Exit(64)
	}

	// Check for flags
	for i := 1; i < len(os.Args); i++ {
		if strings.HasPrefix(os.Args[i], "-") {
			fmt.Fprintf(os.Stderr, "usage: %s FILE...\n", os.Args[0])
			os.Exit(64)
		}
	}

	// Read and merge config files
	config := make(map[string]interface{})

	for _, file := range os.Args[1:] {
		data, err := ioutil.ReadFile(file)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", file)
			os.Exit(2)
		}

		var obj interface{}
		decoder := json.NewDecoder(strings.NewReader(string(data)))
		decoder.UseNumber() // Preserve number format

		err = decoder.Decode(&obj)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: invalid JSON in %s\n", file)
			os.Exit(2)
		}

		// Check if it's an object (map)
		objMap, isMap := obj.(map[string]interface{})
		if !isMap {
			fmt.Fprintf(os.Stderr, "error: %s is not an object\n", file)
			os.Exit(2)
		}

		// Merge obj into config
		config = merge(config, objMap)
	}

	// Apply environment variable overrides
	config = applyEnvironmentOverrides(config)

	// Validate
	errors := validate(config)
	if len(errors) > 0 {
		for _, err := range errors {
			fmt.Fprintf(os.Stderr, "error: invalid %s\n", err)
		}
		os.Exit(1)
	}

	// Output as JSON
	output := formatJSON(config, 0)
	fmt.Print(output)
}

// merge recursively merges src into dst
func merge(dst, src map[string]interface{}) map[string]interface{} {
	for key, srcValue := range src {
		if dstValue, exists := dst[key]; exists {
			// Both have the key
			srcMap, srcIsMap := srcValue.(map[string]interface{})
			dstMap, dstIsMap := dstValue.(map[string]interface{})

			if srcIsMap && dstIsMap {
				// Both are objects, merge recursively
				dst[key] = merge(dstMap, srcMap)
			} else {
				// Replace with source value
				dst[key] = srcValue
			}
		} else {
			// Only in source
			dst[key] = srcValue
		}
	}
	return dst
}

// applyEnvironmentOverrides applies environment variable overrides
func applyEnvironmentOverrides(config map[string]interface{}) map[string]interface{} {
	envVars := []string{}
	for _, env := range os.Environ() {
		if strings.HasPrefix(env, "APP_") {
			envVars = append(envVars, env)
		}
	}
	sort.Strings(envVars)

	for _, env := range envVars {
		parts := strings.SplitN(env, "=", 2)
		if len(parts) != 2 {
			continue
		}

		key := parts[0][4:] // Remove "APP_" prefix
		value := parts[1]

		// Split key on "__"
		keyParts := strings.Split(key, "__")

		// Check for empty keys
		hasEmpty := false
		for _, part := range keyParts {
			if part == "" {
				hasEmpty = true
				break
			}
		}
		if hasEmpty {
			continue
		}

		// Lowercase all key parts
		for i := range keyParts {
			keyParts[i] = strings.ToLower(keyParts[i])
		}

		// Parse value as JSON if possible
		var parsedValue interface{}
		decoder := json.NewDecoder(strings.NewReader(value))
		decoder.UseNumber()
		err := decoder.Decode(&parsedValue)
		if err != nil || !isValidJSON(value) {
			// If not valid JSON, treat as string
			parsedValue = value
		}

		// Set the value in the config
		setPath(config, keyParts, parsedValue)
	}

	return config
}

// isValidJSON checks if a string is valid JSON
func isValidJSON(s string) bool {
	decoder := json.NewDecoder(strings.NewReader(s))
	decoder.UseNumber()
	var v interface{}
	err := decoder.Decode(&v)
	if err != nil {
		return false
	}
	// Make sure it consumed the entire input
	var extra interface{}
	err = decoder.Decode(&extra)
	return err != nil // Should get an error at EOF
}

// setPath sets a value at a path in the config
func setPath(config map[string]interface{}, path []string, value interface{}) {
	current := config
	for i := 0; i < len(path)-1; i++ {
		key := path[i]
		if v, exists := current[key]; exists {
			if m, isMap := v.(map[string]interface{}); isMap {
				current = m
			} else {
				// Replace with new object
				m = make(map[string]interface{})
				current[key] = m
				current = m
			}
		} else {
			// Create new object
			m := make(map[string]interface{})
			current[key] = m
			current = m
		}
	}
	current[path[len(path)-1]] = value
}

// validate checks that the config has required fields
func validate(config map[string]interface{}) []string {
	var errors []string

	// Check service.name
	if !isNonEmptyString(getPath(config, []string{"service", "name"})) {
		errors = append(errors, "service.name")
	}

	// Check db.host
	if !isNonEmptyString(getPath(config, []string{"db", "host"})) {
		errors = append(errors, "db.host")
	}

	// Check db.port
	port := getPath(config, []string{"db", "port"})
	if !isValidPort(port) {
		errors = append(errors, "db.port")
	}

	return errors
}

// getPath retrieves a value at a path in the config
func getPath(config map[string]interface{}, path []string) interface{} {
	current := interface{}(config)
	for _, key := range path {
		m, isMap := current.(map[string]interface{})
		if !isMap {
			return nil
		}
		current = m[key]
	}
	return current
}

// isNonEmptyString checks if a value is a non-empty string
func isNonEmptyString(v interface{}) bool {
	s, isString := v.(string)
	return isString && s != ""
}

// isValidPort checks if a value is a valid port number
func isValidPort(v interface{}) bool {
	switch val := v.(type) {
	case json.Number:
		// Check if it's a valid integer
		s := string(val)
		// Must be digits only, no sign, no exponent, no fraction
		if s == "" {
			return false
		}
		for _, c := range s {
			if c < '0' || c > '9' {
				return false
			}
		}
		// Parse as integer
		n := 0
		for _, c := range s {
			n = n*10 + int(c-'0')
			if n > 65535 {
				return false
			}
		}
		return n >= 1 && n <= 65535
	case float64:
		// This shouldn't happen if we use json.Number, but handle it
		if val != float64(int(val)) {
			return false // Has fraction or exponent
		}
		n := int(val)
		return n >= 1 && n <= 65535
	}
	return false
}

// formatJSON formats a value as JSON
func formatJSON(v interface{}, indent int) string {
	return formatJSONHelper(v, indent, true)
}

// formatJSONHelper is the recursive helper for formatJSON
func formatJSONHelper(v interface{}, indent int, topLevel bool) string {
	indentStr := strings.Repeat(" ", indent)
	nextIndentStr := strings.Repeat(" ", indent+2)

	switch val := v.(type) {
	case map[string]interface{}:
		if len(val) == 0 {
			return "{}"
		}
		// Sort keys
		keys := make([]string, 0, len(val))
		for k := range val {
			keys = append(keys, k)
		}
		sort.Strings(keys)

		result := "{\n"
		for i, k := range keys {
			result += nextIndentStr + `"` + escapeString(k) + `": `
			result += formatJSONHelper(val[k], indent+2, false)
			if i < len(keys)-1 {
				result += ","
			}
			result += "\n"
		}
		result += indentStr + "}"
		return result

	case []interface{}:
		if len(val) == 0 {
			return "[]"
		}
		result := "[\n"
		for i, elem := range val {
			result += nextIndentStr + formatJSONHelper(elem, indent+2, false)
			if i < len(val)-1 {
				result += ","
			}
			result += "\n"
		}
		result += indentStr + "]"
		return result

	case json.Number:
		return string(val)

	case string:
		return `"` + escapeString(val) + `"`

	case bool:
		if val {
			return "true"
		}
		return "false"

	case nil:
		return "null"

	default:
		// Fallback
		bytes, _ := json.Marshal(v)
		return string(bytes)
	}
}

// escapeString escapes a string for JSON output
func escapeString(s string) string {
	result := ""
	for _, r := range s {
		switch r {
		case '"':
			result += `\"`
		case '\\':
			result += `\\`
		case '\b':
			result += `\b`
		case '\t':
			result += `\t`
		case '\n':
			result += `\n`
		case '\f':
			result += `\f`
		case '\r':
			result += `\r`
		default:
			if r < 0x20 {
				result += fmt.Sprintf(`\u%04x`, r)
			} else {
				result += string(r)
			}
		}
	}
	return result
}
