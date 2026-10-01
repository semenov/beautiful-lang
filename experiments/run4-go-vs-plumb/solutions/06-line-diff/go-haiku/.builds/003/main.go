package main

import (
	"fmt"
	"os"
	"strconv"
)

func main() {
	// Manual argument parsing
	args := os.Args[1:]
	U := 3
	var files []string

	i := 0
	for i < len(args) {
		arg := args[i]
		if arg == "-U" {
			i++
			if i >= len(args) {
				fmt.Fprintf(os.Stderr, "usage: app [-U N] OLD NEW\n")
				os.Exit(2)
			}
			val, err := strconv.Atoi(args[i])
			if err != nil || val < 0 {
				fmt.Fprintf(os.Stderr, "usage: app [-U N] OLD NEW\n")
				os.Exit(2)
			}
			U = val
		} else if len(arg) > 0 && arg[0] == '-' {
			// Unknown option
			fmt.Fprintf(os.Stderr, "usage: app [-U N] OLD NEW\n")
			os.Exit(2)
		} else {
			files = append(files, arg)
		}
		i++
	}

	if len(files) != 2 {
		fmt.Fprintf(os.Stderr, "usage: app [-U N] OLD NEW\n")
		os.Exit(2)
	}

	oldPath, newPath := files[0], files[1]

	// Read files
	oldBytes, err := os.ReadFile(oldPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: cannot read %s\n", oldPath)
		os.Exit(2)
	}

	newBytes, err := os.ReadFile(newPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: cannot read %s\n", newPath)
		os.Exit(2)
	}

	// Split into lines
	oldLines := splitLines(string(oldBytes))
	newLines := splitLines(string(newBytes))

	// Compute LCS and determine operations
	ops := computeDiff(oldLines, newLines)

	// Check if there are any changes
	hasChanges := false
	for _, op := range ops {
		if op.typ != opKeep {
			hasChanges = true
			break
		}
	}

	if !hasChanges {
		os.Exit(0)
	}

	// Print header
	fmt.Printf("--- %s\n", oldPath)
	fmt.Printf("+++ %s\n", newPath)

	// Generate and print hunks
	generateHunks(oldLines, newLines, ops, *U)

	os.Exit(1)
}

// Line types
const (
	opKeep   = 0
	opRemove = 1
	opAdd    = 2
)

type operation struct {
	typ      int    // 0=keep, 1=remove, 2=add
	oldIdx   int    // index in oldLines (-1 if not applicable)
	newIdx   int    // index in newLines (-1 if not applicable)
	oldLine  string // content of line from oldLines
	newLine  string // content of line from newLines
}

// splitLines splits content into lines, preserving LF characters
// An empty file results in no lines
func splitLines(s string) []string {
	if s == "" {
		return []string{}
	}

	var lines []string
	var current string
	for i := 0; i < len(s); i++ {
		current += string(s[i])
		if s[i] == '\n' {
			lines = append(lines, current)
			current = ""
		}
	}
	if current != "" {
		lines = append(lines, current)
	}
	return lines
}

// memoization for LCS
type lcsKey struct {
	i, j int
}

var lcsCache map[lcsKey]int

// computeLCS computes the length of the longest common subsequence
// from position i in oldLines and position j in newLines
func computeLCS(oldLines, newLines []string, i, j int) int {
	if i >= len(oldLines) || j >= len(newLines) {
		return 0
	}

	key := lcsKey{i, j}
	if v, ok := lcsCache[key]; ok {
		return v
	}

	var result int
	if oldLines[i] == newLines[j] {
		result = 1 + computeLCS(oldLines, newLines, i+1, j+1)
	} else {
		r1 := computeLCS(oldLines, newLines, i+1, j)
		r2 := computeLCS(oldLines, newLines, i, j+1)
		if r1 > r2 {
			result = r1
		} else {
			result = r2
		}
	}

	lcsCache[key] = result
	return result
}

// computeDiff implements the algorithm from the spec
func computeDiff(oldLines, newLines []string) []operation {
	lcsCache = make(map[lcsKey]int)
	defer func() { lcsCache = nil }()

	var ops []operation
	n, m := len(oldLines), len(newLines)
	i, j := 0, 0

	for i < n || j < m {
		if i < n && j < m && oldLines[i] == newLines[j] {
			// Line is kept
			ops = append(ops, operation{
				typ:     opKeep,
				oldIdx:  i,
				newIdx:  j,
				oldLine: oldLines[i],
				newLine: newLines[j],
			})
			i++
			j++
		} else if j == m || (i < n && computeLCS(oldLines, newLines, i+1, j) >= computeLCS(oldLines, newLines, i, j+1)) {
			// A[i] is removed
			ops = append(ops, operation{
				typ:     opRemove,
				oldIdx:  i,
				newIdx:  -1,
				oldLine: oldLines[i],
				newLine: "",
			})
			i++
		} else {
			// B[j] is added
			ops = append(ops, operation{
				typ:     opAdd,
				oldIdx:  -1,
				newIdx:  j,
				oldLine: "",
				newLine: newLines[j],
			})
			j++
		}
	}

	return ops
}

// generateHunks generates and prints the diff hunks
func generateHunks(oldLines, newLines []string, ops []operation, U int) {
	// Identify change runs (sequences of non-keep operations)
	var changeRuns []struct {
		startOp, endOp int // indices in ops array
	}

	i := 0
	for i < len(ops) {
		if ops[i].typ != opKeep {
			start := i
			for i < len(ops) && ops[i].typ != opKeep {
				i++
			}
			changeRuns = append(changeRuns, struct {
				startOp, endOp int
			}{start, i})
		} else {
			i++
		}
	}

	if len(changeRuns) == 0 {
		return
	}

	// Merge runs that are within 2*U context lines of each other
	var hunks []struct {
		startOp, endOp int
	}

	hunks = append(hunks, changeRuns[0])
	for i := 1; i < len(changeRuns); i++ {
		lastRun := hunks[len(hunks)-1]
		currentRun := changeRuns[i]

		// Count kept lines between lastRun.endOp and currentRun.startOp
		keptBetween := 0
		for j := lastRun.endOp; j < currentRun.startOp; j++ {
			if ops[j].typ == opKeep {
				keptBetween++
			}
		}

		if keptBetween <= 2*U {
			// Merge the hunks
			hunks[len(hunks)-1].endOp = currentRun.endOp
		} else {
			// Start a new hunk
			hunks = append(hunks, currentRun)
		}
	}

	// Now create hunks with context lines
	for _, hunk := range hunks {
		// Find the start of the hunk (with U context lines before)
		startOp := hunk.startOp
		contextStart := startOp
		keptCount := 0
		for contextStart > 0 && keptCount < U {
			contextStart--
			if ops[contextStart].typ == opKeep {
				keptCount++
			}
		}

		// Find the end of the hunk (with U context lines after)
		endOp := hunk.endOp
		contextEnd := endOp
		keptCount = 0
		for contextEnd < len(ops) && keptCount < U {
			if ops[contextEnd].typ == opKeep {
				keptCount++
			}
			contextEnd++
		}

		// Calculate start and count for this hunk in old and new files
		var oldStart, oldCount, newStart, newCount int
		oldStart = -1
		newStart = -1

		for i := contextStart; i < contextEnd; i++ {
			if ops[i].typ == opKeep || ops[i].typ == opRemove {
				if oldStart == -1 {
					oldStart = ops[i].oldIdx
				}
				if ops[i].typ == opRemove || ops[i].typ == opKeep {
					oldCount++
				}
			}
			if ops[i].typ == opKeep || ops[i].typ == opAdd {
				if newStart == -1 {
					newStart = ops[i].newIdx
				}
				if ops[i].typ == opAdd || ops[i].typ == opKeep {
					newCount++
				}
			}
		}

		// Handle edge cases for start/count
		if oldStart == -1 {
			oldStart = 0
			oldCount = 0
		} else {
			oldStart++ // Convert to 1-based
		}

		if newStart == -1 {
			newStart = 0
			newCount = 0
		} else {
			newStart++ // Convert to 1-based
		}

		// Print hunk header
		if oldCount == 1 {
			fmt.Printf("@@ -%d", oldStart)
		} else {
			fmt.Printf("@@ -%d,%d", oldStart, oldCount)
		}

		if newCount == 1 {
			fmt.Printf(" +%d @@\n", newStart)
		} else {
			fmt.Printf(" +%d,%d @@\n", newStart, newCount)
		}

		// Print hunk lines
		for i := contextStart; i < contextEnd; i++ {
			switch ops[i].typ {
			case opKeep:
				fmt.Printf(" %s", ops[i].oldLine)
				if !hasNewline(ops[i].oldLine) {
					fmt.Printf("\n\\ No newline at end of file\n")
				}
			case opRemove:
				fmt.Printf("-%s", ops[i].oldLine)
				if !hasNewline(ops[i].oldLine) {
					fmt.Printf("\n\\ No newline at end of file\n")
				}
			case opAdd:
				fmt.Printf("+%s", ops[i].newLine)
				if !hasNewline(ops[i].newLine) {
					fmt.Printf("\n\\ No newline at end of file\n")
				}
			}
		}
	}
}

func hasNewline(s string) bool {
	return len(s) > 0 && s[len(s)-1] == '\n'
}
