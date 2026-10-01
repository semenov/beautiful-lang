package main

import (
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

func main() {
	// Parse command-line arguments
	minSize := int64(1)
	args := os.Args[1:]

	// Look for --min-size flag
	var dir string
	dirCount := 0
	for i := 0; i < len(args); i++ {
		if args[i] == "--min-size" {
			if i+1 >= len(args) {
				fmt.Fprintf(os.Stderr, "usage: app [--min-size BYTES] DIR\n")
				os.Exit(64)
			}
			val, err := strconv.ParseInt(args[i+1], 10, 64)
			if err != nil || val < 0 {
				fmt.Fprintf(os.Stderr, "usage: app [--min-size BYTES] DIR\n")
				os.Exit(64)
			}
			minSize = val
			i++ // skip the next arg
		} else if strings.HasPrefix(args[i], "--") {
			// unknown option
			fmt.Fprintf(os.Stderr, "usage: app [--min-size BYTES] DIR\n")
			os.Exit(64)
		} else {
			dir = args[i]
			dirCount++
		}
	}

	if dirCount != 1 {
		fmt.Fprintf(os.Stderr, "usage: app [--min-size BYTES] DIR\n")
		os.Exit(64)
	}

	// Check that DIR exists and is a directory
	info, err := os.Stat(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: not a directory: %s\n", dir)
		os.Exit(2)
	}
	if !info.IsDir() {
		fmt.Fprintf(os.Stderr, "error: not a directory: %s\n", dir)
		os.Exit(2)
	}

	// Walk the directory and collect file hashes
	type FileInfo struct {
		Path string
		Size int64
		Hash string
	}

	var files []FileInfo
	var readErrors []string

	err = filepath.Walk(dir, func(path string, fi os.FileInfo, err error) error {
		if err != nil {
			// Can't read this path
			relPath, _ := filepath.Rel(dir, path)
			readErrors = append(readErrors, relPath)
			return nil // continue walking
		}

		// Skip non-regular files (directories, symlinks, etc.)
		if !fi.Mode().IsRegular() {
			return nil
		}

		// Skip files smaller than min-size
		if fi.Size() < minSize {
			return nil
		}

		// Hash the file
		hash, err := hashFile(path)
		if err != nil {
			relPath, _ := filepath.Rel(dir, path)
			readErrors = append(readErrors, relPath)
			return nil
		}

		relPath, _ := filepath.Rel(dir, path)
		files = append(files, FileInfo{
			Path: relPath,
			Size: fi.Size(),
			Hash: hash,
		})

		return nil
	})

	if err != nil {
		fmt.Fprintf(os.Stderr, "error: not a directory: %s\n", dir)
		os.Exit(2)
	}

	// Group files by hash
	type Group struct {
		Hash  string
		Size  int64
		Paths []string
	}

	groups := make(map[string]*Group)
	for _, f := range files {
		if g, exists := groups[f.Hash]; exists {
			g.Paths = append(g.Paths, f.Path)
		} else {
			groups[f.Hash] = &Group{
				Hash:  f.Hash,
				Size:  f.Size,
				Paths: []string{f.Path},
			}
		}
	}

	// Filter groups with 2+ files
	var duplicateGroups []*Group
	for _, g := range groups {
		if len(g.Paths) >= 2 {
			duplicateGroups = append(duplicateGroups, g)
		}
	}

	// Sort groups by size (descending) then by hash (ascending)
	sort.Slice(duplicateGroups, func(i, j int) bool {
		if duplicateGroups[i].Size != duplicateGroups[j].Size {
			return duplicateGroups[i].Size > duplicateGroups[j].Size
		}
		return duplicateGroups[i].Hash < duplicateGroups[j].Hash
	})

	// For each group, sort paths
	for _, g := range duplicateGroups {
		sort.Strings(g.Paths)
	}

	// Output results
	var totalWasted int64
	for _, g := range duplicateGroups {
		fmt.Printf("sha256:%s size %d\n", g.Hash, g.Size)
		for _, path := range g.Paths {
			fmt.Printf("  %s\n", path)
		}
		fmt.Printf("\n")
		wasted := g.Size * int64(len(g.Paths)-1)
		totalWasted += wasted
	}

	numGroups := len(duplicateGroups)
	numFiles := 0
	for _, g := range duplicateGroups {
		numFiles += len(g.Paths)
	}

	fmt.Printf("groups %d files %d wasted %d\n", numGroups, numFiles, totalWasted)

	// Print read errors and exit with appropriate code
	if len(readErrors) > 0 {
		for _, e := range readErrors {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", e)
		}
		os.Exit(1)
	}

	os.Exit(0)
}

func hashFile(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()

	h := sha256.New()
	if _, err := io.Copy(h, file); err != nil {
		return "", err
	}

	return fmt.Sprintf("%x", h.Sum(nil)), nil
}
