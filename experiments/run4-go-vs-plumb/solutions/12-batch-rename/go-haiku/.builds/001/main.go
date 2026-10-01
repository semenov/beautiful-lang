package main

import (
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

func main() {
	var (
		regexPattern  string
		regexTemplate string
		counterTmpl   string
		startNum      int
		matchPattern  string
		dryRun        bool
	)

	fs := flag.NewFlagSet(os.Args[0], flag.ContinueOnError)
	fs.StringVar(&regexPattern, "regex", "", "regex pattern for file matching")
	fs.StringVar(&regexTemplate, "to", "", "template for regex mode")
	fs.StringVar(&counterTmpl, "counter", "", "template for counter mode")
	fs.IntVar(&startNum, "start", 1, "starting number for counter mode")
	fs.StringVar(&matchPattern, "match", "", "match pattern for counter mode")
	fs.BoolVar(&dryRun, "dry-run", false, "dry run mode")

	fs.Usage = func() {}

	err := fs.Parse(os.Args[1:])
	if err != nil {
		usageError("invalid arguments")
		os.Exit(64)
	}

	args := fs.Args()

	// Find DIR - it's the remaining positional argument
	var dir string
	if len(args) > 0 {
		dir = args[0]
	}

	// Validate mode selection
	regexMode := regexPattern != ""
	counterMode := counterTmpl != ""

	if regexMode && counterMode {
		usageError("cannot use both --regex and --counter")
		os.Exit(64)
	}
	if !regexMode && !counterMode {
		usageError("must use either --regex or --counter")
		os.Exit(64)
	}

	// Validate template-specific options
	if regexMode {
		if regexTemplate == "" {
			usageError("--regex requires --to")
			os.Exit(64)
		}
		if startNum != 1 || matchPattern != "" {
			usageError("--start and --match are only for --counter mode")
			os.Exit(64)
		}
	}

	if counterMode {
		if regexTemplate != "" {
			usageError("--to is only for --regex mode")
			os.Exit(64)
		}
		if startNum < 0 {
			usageError("--start must be >= 0")
			os.Exit(64)
		}
	}

	if dir == "" {
		usageError("DIR is required")
		os.Exit(64)
	}

	// Check if DIR is valid
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		fmt.Fprintf(os.Stderr, "error: not a directory: %s\n", dir)
		os.Exit(2)
	}

	// Read directory
	entries, err := ioutil.ReadDir(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: not a directory: %s\n", dir)
		os.Exit(2)
	}

	// Filter to regular files only, sort by name
	var files []string
	for _, entry := range entries {
		if entry.Mode().IsRegular() {
			files = append(files, entry.Name())
		}
	}
	sort.Strings(files)

	var renames []struct {
		old string
		new string
	}

	if regexMode {
		renames, err = processRegexMode(files, regexPattern, regexTemplate)
	} else {
		renames, err = processCounterMode(files, counterTmpl, matchPattern, startNum)
	}

	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err.Error())
		os.Exit(64)
	}

	// Validate batch before renaming
	existingFiles := make(map[string]bool)
	for _, f := range files {
		existingFiles[f] = true
	}

	if err := validateBatch(renames, existingFiles); err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err.Error())
		os.Exit(1)
	}

	// Print results
	for _, r := range renames {
		fmt.Printf("%s -> %s\n", r.old, r.new)
	}

	if dryRun {
		fmt.Printf("would rename %d\n", len(renames))
	} else {
		// Actually rename files
		for _, r := range renames {
			oldPath := filepath.Join(dir, r.old)
			newPath := filepath.Join(dir, r.new)
			if err := os.Rename(oldPath, newPath); err != nil {
				fmt.Fprintf(os.Stderr, "error: failed to rename %s: %v\n", r.old, err)
				os.Exit(1)
			}
		}
		fmt.Printf("renamed %d\n", len(renames))
	}
}

func processRegexMode(files []string, pattern, template string) ([]struct {
	old string
	new string
}, error) {
	re, err := regexp.Compile("^" + pattern + "$")
	if err != nil {
		return nil, fmt.Errorf("usage: invalid regex: %v", err)
	}

	// Validate template
	if err := validateRegexTemplate(template, re.NumSubexp()); err != nil {
		return nil, err
	}

	var renames []struct {
		old string
		new string
	}

	for _, file := range files {
		matches := re.FindStringSubmatch(file)
		if matches == nil {
			continue
		}

		newName, err := applyRegexTemplate(template, matches)
		if err != nil {
			return nil, err
		}

		if newName != file {
			renames = append(renames, struct {
				old string
				new string
			}{file, newName})
		}
	}

	return renames, nil
}

func processCounterMode(files []string, template, matchPattern string, startNum int) ([]struct {
	old string
	new string
}, error) {
	// Validate template
	hashCount := strings.Count(template, "#")
	if hashCount != 1 {
		return nil, fmt.Errorf("usage: template must contain exactly one run of #")
	}
	hashStart := strings.Index(template, "#")
	hashEnd := hashStart
	for hashEnd < len(template) && template[hashEnd] == '#' {
		hashEnd++
	}
	hashLen := hashEnd - hashStart

	var matchRe *regexp.Regexp
	var err error
	if matchPattern != "" {
		matchRe, err = regexp.Compile("^" + matchPattern + "$")
		if err != nil {
			return nil, fmt.Errorf("usage: invalid match pattern: %v", err)
		}
	}

	var renames []struct {
		old string
		new string
	}

	counter := startNum
	for _, file := range files {
		if matchRe != nil && matchRe.FindString(file) == "" {
			continue
		}

		// Generate new name
		numStr := strconv.Itoa(counter)
		numStr = strings.Repeat("0", hashLen-len(numStr)) + numStr

		newName := template[:hashStart] + numStr + template[hashEnd:]

		if newName != file {
			renames = append(renames, struct {
				old string
				new string
			}{file, newName})
		}

		counter++
	}

	return renames, nil
}

func applyRegexTemplate(template string, matches []string) (string, error) {
	result := ""
	i := 0
	for i < len(template) {
		if template[i] == '{' {
			if i+1 < len(template) {
				if template[i+1] == '{' {
					result += "{"
					i += 2
					continue
				}
				// Parse group number
				j := i + 1
				for j < len(template) && template[j] >= '0' && template[j] <= '9' {
					j++
				}
				if j > i+1 && j < len(template) && template[j] == '}' {
					numStr := template[i+1 : j]
					groupNum, _ := strconv.Atoi(numStr)
					if groupNum >= len(matches) {
						return "", fmt.Errorf("usage: group %d does not exist", groupNum)
					}
					result += matches[groupNum]
					i = j + 1
					continue
				}
				return "", fmt.Errorf("usage: invalid template: unexpected {")
			}
			return "", fmt.Errorf("usage: invalid template: unexpected {")
		} else if template[i] == '}' {
			if i+1 < len(template) && template[i+1] == '}' {
				result += "}"
				i += 2
				continue
			}
			return "", fmt.Errorf("usage: invalid template: unexpected }")
		}
		result += string(template[i])
		i++
	}
	return result, nil
}

func validateRegexTemplate(template string, numGroups int) error {
	i := 0
	for i < len(template) {
		if template[i] == '{' {
			if i+1 < len(template) && template[i+1] == '{' {
				i += 2
				continue
			}
			j := i + 1
			for j < len(template) && template[j] >= '0' && template[j] <= '9' {
				j++
			}
			if j > i+1 && j < len(template) && template[j] == '}' {
				numStr := template[i+1 : j]
				groupNum, _ := strconv.Atoi(numStr)
				if groupNum > numGroups {
					return fmt.Errorf("usage: group %d does not exist", groupNum)
				}
				i = j + 1
				continue
			}
			return fmt.Errorf("usage: invalid template: unexpected {")
		} else if template[i] == '}' {
			if i+1 < len(template) && template[i+1] == '}' {
				i += 2
				continue
			}
			return fmt.Errorf("usage: invalid template: unexpected }")
		}
		i++
	}
	return nil
}

func validateBatch(renames []struct {
	old string
	new string
}, existing map[string]bool) error {
	// Check for invalid names and collect all new names
	newNames := make(map[string]bool)
	for _, r := range renames {
		// Check for invalid characters and values
		if r.new == "" || r.new == "." || r.new == ".." {
			return fmt.Errorf("error: invalid name: %s", r.new)
		}
		if strings.Contains(r.new, "/") || strings.Contains(r.new, "\x00") {
			return fmt.Errorf("error: invalid name: %s", r.new)
		}

		// Check for conflicts with other new names
		if newNames[r.new] {
			return fmt.Errorf("error: conflict: %s", r.new)
		}
		newNames[r.new] = true

		// Check if new name exists (and is not being renamed away)
		if existing[r.new] {
			// It's okay if it's the old file being renamed
			isOwnFile := false
			for _, other := range renames {
				if other.old == r.new {
					isOwnFile = true
					break
				}
			}
			if !isOwnFile {
				return fmt.Errorf("error: exists: %s", r.new)
			}
		}
	}

	return nil
}

func usageError(msg string) {
	fmt.Fprintf(os.Stderr, "usage: %s\n", msg)
}
