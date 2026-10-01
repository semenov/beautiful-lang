package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"
)

func main() {
	fs := flag.NewFlagSet("app", flag.ContinueOnError)
	fs.Usage = func() {}

	top := fs.Int("top", 10, "")
	minLength := fs.Int("min-length", 1, "")

	err := fs.Parse(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "usage: app [--top N] [--min-length L] [FILE...]\n")
		os.Exit(64)
	}

	// Validate flag values
	if *top < 0 {
		fmt.Fprintf(os.Stderr, "usage: app [--top N] [--min-length L] [FILE...]\n")
		os.Exit(64)
	}
	if *minLength < 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--top N] [--min-length L] [FILE...]\n")
		os.Exit(64)
	}

	files := fs.Args()
	var reader io.Reader

	if len(files) == 0 {
		reader = os.Stdin
	} else {
		readers := make([]io.Reader, len(files))
		for i, f := range files {
			file, err := os.Open(f)
			if err != nil {
				fmt.Fprintf(os.Stderr, "error: cannot read %s\n", f)
				os.Exit(2)
			}
			defer file.Close()
			readers[i] = file
		}
		reader = io.MultiReader(readers...)
	}

	wordCounts := make(map[string]int)
	scanner := bufio.NewScanner(reader)

	// Process input and extract words
	text := ""
	for scanner.Scan() {
		text += scanner.Text() + "\n"
	}

	// Extract words from text, handling invalid UTF-8
	words := extractWords(text)
	for _, word := range words {
		// Lowercase the word
		lowerWord := strings.ToLower(word)
		// Check min length in code points, not bytes
		if utf8.RuneCountInString(lowerWord) >= *minLength {
			wordCounts[lowerWord]++
		}
	}

	// Sort words by count (desc) then alphabetically
	type wordCount struct {
		word  string
		count int
	}

	var sorted []wordCount
	for word, count := range wordCounts {
		sorted = append(sorted, wordCount{word, count})
	}

	sort.Slice(sorted, func(i, j int) bool {
		if sorted[i].count != sorted[j].count {
			return sorted[i].count > sorted[j].count
		}
		return sorted[i].word < sorted[j].word
	})

	// Print results
	limit := *top
	if limit > len(sorted) {
		limit = len(sorted)
	}

	totalWords := 0
	for _, wc := range sorted {
		totalWords += wc.count
	}

	for i := 0; i < limit; i++ {
		fmt.Printf("%s %d\n", sorted[i].word, sorted[i].count)
	}

	fmt.Printf("total %d unique %d\n", totalWords, len(wordCounts))
}

// extractWords extracts words from text, handling Unicode letters and apostrophes
// and treating invalid UTF-8 bytes as separators
func extractWords(text string) []string {
	var words []string
	var currentWord strings.Builder

	// Iterate through runes
	for i := 0; i < len(text); {
		r, size := utf8.DecodeRuneInString(text[i:])
		if r == utf8.RuneError {
			// Invalid UTF-8 - treat as separator
			if currentWord.Len() > 0 {
				word := trimApostrophes(currentWord.String())
				if word != "" {
					words = append(words, word)
				}
				currentWord.Reset()
			}
			i += size
			continue
		}

		if unicode.IsLetter(r) {
			currentWord.WriteRune(r)
		} else if r == '\'' && currentWord.Len() > 0 {
			// Apostrophe is part of word if not at the start
			currentWord.WriteRune(r)
		} else {
			// Any other character is a separator
			if currentWord.Len() > 0 {
				word := trimApostrophes(currentWord.String())
				if word != "" {
					words = append(words, word)
				}
				currentWord.Reset()
			}
		}

		i += size
	}

	// Don't forget the last word
	if currentWord.Len() > 0 {
		word := trimApostrophes(currentWord.String())
		if word != "" {
			words = append(words, word)
		}
	}

	return words
}

// trimApostrophes removes leading and trailing apostrophes from a word
func trimApostrophes(word string) string {
	return strings.Trim(word, "'")
}
