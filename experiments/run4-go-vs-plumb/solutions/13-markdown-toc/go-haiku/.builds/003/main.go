package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"unicode"
	"unicode/utf8"
)

func main() {
	flag.CommandLine.SetOutput(io.Discard)
	flag.CommandLine.Init("app", flag.ContinueOnError)

	minLevel := flag.Int("min-level", 1, "minimum heading level")
	maxLevel := flag.Int("max-level", 6, "maximum heading level")

	err := flag.CommandLine.Parse(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "usage: app [--min-level N] [--max-level M] [FILE]\n")
		os.Exit(64)
	}

	args := flag.CommandLine.Args()
	if len(args) > 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--min-level N] [--max-level M] [FILE]\n")
		os.Exit(64)
	}

	if *minLevel < 1 || *maxLevel < 1 || *minLevel > 6 || *maxLevel > 6 || *minLevel > *maxLevel {
		fmt.Fprintf(os.Stderr, "usage: app [--min-level N] [--max-level M] [FILE]\n")
		os.Exit(64)
	}

	var input io.Reader = os.Stdin
	if len(args) == 1 {
		f, err := os.Open(args[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", args[0])
			os.Exit(2)
		}
		defer f.Close()
		input = f
	}

	scanner := bufio.NewScanner(input)
	var headings []Heading
	slugCounts := make(map[string]int)

	inFence := false
	fenceChar := byte(0)
	fenceCount := 0

	for scanner.Scan() {
		line := scanner.Text()

		// Handle fence blocks
		if isFenceOpening(line, &fenceChar, &fenceCount) && !inFence {
			inFence = true
			continue
		}
		if inFence && isFenceClosing(line, fenceChar, fenceCount) {
			inFence = false
			continue
		}
		if inFence {
			continue
		}

		// Parse heading
		level, text := parseHeading(line)
		if level > 0 {
			slug := makeSlug(text)
			if slug != "" {
				// Track slug for duplicate handling
				count := slugCounts[slug]
				slugCounts[slug]++
				finalSlug := slug
				if count > 0 {
					finalSlug = fmt.Sprintf("%s-%d", slug, count)
				}
				headings = append(headings, Heading{
					Level: level,
					Text:  text,
					Slug:  finalSlug,
				})
			}
		}
	}

	// Output TOC for headings in the selected range
	for _, h := range headings {
		if h.Level >= *minLevel && h.Level <= *maxLevel {
			indent := strings.Repeat("  ", h.Level-*minLevel)
			fmt.Printf("%s- [%s](#%s)\n", indent, h.Text, h.Slug)
		}
	}
}

type Heading struct {
	Level int
	Text  string
	Slug  string
}

// isFenceOpening checks if a line opens a fence block and updates fenceChar/fenceCount
func isFenceOpening(line string, fenceChar *byte, fenceCount *int) bool {
	// Strip leading spaces (0-3)
	spaces := 0
	for i := 0; i < len(line) && spaces < 3; i++ {
		if line[i] == ' ' {
			spaces++
		} else if line[i] == '\t' {
			spaces++
		} else {
			break
		}
	}

	trimmed := line[spaces:]
	if len(trimmed) < 3 {
		return false
	}

	// Check for backticks or tildes
	char := trimmed[0]
	if char != '`' && char != '~' {
		return false
	}

	count := 0
	for i := 0; i < len(trimmed); i++ {
		if trimmed[i] == char {
			count++
		} else {
			break
		}
	}

	if count < 3 {
		return false
	}

	// Rest of the line should only be spaces/tabs
	rest := trimmed[count:]
	for i := 0; i < len(rest); i++ {
		if rest[i] != ' ' && rest[i] != '\t' {
			return false
		}
	}

	*fenceChar = char
	*fenceCount = count
	return true
}

// isFenceClosing checks if a line closes a fence block
func isFenceClosing(line string, fenceChar byte, fenceCount int) bool {
	// Strip leading spaces (0-3)
	spaces := 0
	for i := 0; i < len(line) && spaces < 3; i++ {
		if line[i] == ' ' {
			spaces++
		} else if line[i] == '\t' {
			spaces++
		} else {
			break
		}
	}

	trimmed := line[spaces:]

	// Check for same character, at least as many as opening
	count := 0
	for i := 0; i < len(trimmed); i++ {
		if trimmed[i] == fenceChar {
			count++
		} else {
			break
		}
	}

	if count < fenceCount {
		return false
	}

	// Rest of the line should only be spaces/tabs
	rest := trimmed[count:]
	for i := 0; i < len(rest); i++ {
		if rest[i] != ' ' && rest[i] != '\t' {
			return false
		}
	}

	return true
}

// parseHeading parses an ATX heading line and returns the level and text
// Returns (0, "") if not a heading
func parseHeading(line string) (int, string) {
	// Check 0-3 leading spaces
	spaces := 0
	idx := 0
	for i := 0; i < len(line) && spaces < 3; i++ {
		if line[i] == ' ' {
			spaces++
			idx++
		} else if line[i] == '\t' {
			spaces++
			idx++
		} else {
			break
		}
	}

	if idx >= len(line) {
		return 0, ""
	}

	// Count # characters
	hashCount := 0
	for i := idx; i < len(line) && hashCount < 6; i++ {
		if line[i] == '#' {
			hashCount++
		} else {
			break
		}
	}

	if hashCount == 0 || hashCount > 6 {
		return 0, ""
	}

	idx += hashCount

	// Check what comes after the hashes
	if idx >= len(line) {
		// Just hashes, no text
		return hashCount, ""
	}

	// Must be followed by space or tab
	if line[idx] != ' ' && line[idx] != '\t' {
		return 0, ""
	}

	// Get the text part
	text := line[idx:]
	// Strip leading spaces/tabs
	text = strings.TrimLeft(text, " \t")
	// Strip trailing spaces/tabs
	text = strings.TrimRight(text, " \t")

	// Remove trailing # characters if they form a valid run
	text = stripTrailingHashes(text)

	return hashCount, text
}

// stripTrailingHashes removes trailing # characters if they are valid
// (either the whole text or preceded by space/tab)
func stripTrailingHashes(text string) string {
	if text == "" {
		return text
	}

	// Count trailing hashes
	trailingCount := 0
	for i := len(text) - 1; i >= 0 && text[i] == '#'; i-- {
		trailingCount++
	}

	if trailingCount == 0 {
		return text
	}

	// Check if the whole text is hashes, or if there's a space/tab before them
	beforeHashes := len(text) - trailingCount
	if beforeHashes == 0 {
		// Whole text is hashes
		return ""
	}

	if beforeHashes > 0 {
		if text[beforeHashes-1] == ' ' || text[beforeHashes-1] == '\t' {
			// Valid trailing hash run
			result := text[:beforeHashes]
			result = strings.TrimRight(result, " \t")
			return result
		}
	}

	// Hashes are part of the text (like in "C#")
	return text
}

// makeSlug converts heading text to a GitHub-style anchor slug
func makeSlug(text string) string {
	// Step 1: Convert to lowercase using Unicode simple lowercase mapping
	text = strings.ToLower(text)

	// Step 2 & 3: Build slug by keeping only letters/marks/digits/spaces/-/_ and replacing spaces
	var result strings.Builder
	iter := 0
	for iter < len(text) {
		r, width := utf8.DecodeRuneInString(text[iter:])
		iter += width

		if r == '-' || r == '_' {
			result.WriteRune(r)
		} else if r == ' ' || r == '\t' {
			result.WriteRune('-')
		} else if isLetterOrMark(r) || isDecimalDigit(r) {
			result.WriteRune(r)
		}
	}

	return result.String()
}

// isLetterOrMark checks if a rune is a Unicode letter or combining mark
func isLetterOrMark(r rune) bool {
	// L* = letters, M* = combining marks
	return unicode.IsLetter(r) || unicode.IsMark(r)
}

// isDecimalDigit checks if a rune is a decimal digit (category Nd)
func isDecimalDigit(r rune) bool {
	return unicode.IsDigit(r)
}
