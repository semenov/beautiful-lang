package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

func main() {
	if len(os.Args) < 2 {
		usageExit()
	}

	command := os.Args[1]

	// Parse all remaining arguments flexibly
	positionalArgs := []string{}
	var holidaysFile string
	var weekendStr string
	hasWeekend := false

	for i := 2; i < len(os.Args); i++ {
		arg := os.Args[i]

		if strings.HasPrefix(arg, "--holidays") {
			parts := strings.SplitN(arg, "=", 2)
			if len(parts) == 2 {
				holidaysFile = parts[1]
			} else if i+1 < len(os.Args) {
				i++
				holidaysFile = os.Args[i]
			} else {
				usageExit()
			}
		} else if strings.HasPrefix(arg, "--weekend") {
			hasWeekend = true
			parts := strings.SplitN(arg, "=", 2)
			if len(parts) == 2 {
				weekendStr = parts[1]
			} else if i+1 < len(os.Args) {
				i++
				weekendStr = os.Args[i]
			} else {
				usageExit()
			}
		} else if strings.HasPrefix(arg, "--") {
			usageExit()
		} else {
			positionalArgs = append(positionalArgs, arg)
		}
	}

	// Default weekend if not specified
	if !hasWeekend {
		weekendStr = "sat,sun"
	}

	// Parse weekend days
	weekend := parseWeekend(weekendStr)
	if weekend == nil {
		usageExit()
	}

	// Load holidays if specified
	holidays := make(map[string]bool)
	if holidaysFile != "" {
		var err error
		holidays, err = loadHolidays(holidaysFile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "%v\n", err)
			os.Exit(2)
		}
	}

	// Execute command
	if command == "between" {
		if len(positionalArgs) < 2 {
			usageExit()
		}
		start, err := parseDate(positionalArgs[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: invalid date: %s\n", positionalArgs[0])
			os.Exit(2)
		}
		end, err := parseDate(positionalArgs[1])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: invalid date: %s\n", positionalArgs[1])
			os.Exit(2)
		}

		count := businessDaysBetween(start, end, weekend, holidays)
		fmt.Println(count)

	} else if command == "add" {
		if len(positionalArgs) < 2 {
			usageExit()
		}
		date, err := parseDate(positionalArgs[0])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: invalid date: %s\n", positionalArgs[0])
			os.Exit(2)
		}

		n, err := strconv.Atoi(positionalArgs[1])
		if err != nil {
			usageExit()
		}

		result := addBusinessDays(date, n, weekend, holidays)
		fmt.Println(result.Format("2006-01-02"))

	} else {
		usageExit()
	}
}

func usageExit() {
	fmt.Fprintf(os.Stderr, "usage: app between START END [--holidays FILE] [--weekend DAYS]\nusage: app add DATE N [--holidays FILE] [--weekend DAYS]\n")
	os.Exit(64)
}

func parseDate(s string) (time.Time, error) {
	// Must be exactly YYYY-MM-DD format (10 chars)
	if len(s) != 10 || s[4] != '-' || s[7] != '-' {
		return time.Time{}, fmt.Errorf("invalid date")
	}

	// Check format
	for i, c := range s {
		if i == 4 || i == 7 {
			continue
		}
		if c < '0' || c > '9' {
			return time.Time{}, fmt.Errorf("invalid date")
		}
	}

	t, err := time.Parse("2006-01-02", s)
	if err != nil {
		return time.Time{}, fmt.Errorf("invalid date")
	}

	year := t.Year()
	if year < 1 || year > 9999 {
		return time.Time{}, fmt.Errorf("year out of range")
	}

	return t, nil
}

func parseWeekend(s string) map[time.Weekday]bool {
	weekend := make(map[time.Weekday]bool)

	if s == "" {
		return weekend // Empty weekend (no weekend days)
	}

	parts := strings.Split(s, ",")
	seenDays := make(map[time.Weekday]bool)

	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			// Empty part means invalid
			return nil
		}

		day, ok := weekdayMap[part]
		if !ok {
			return nil // Invalid day name
		}

		seenDays[day] = true
	}

	// Check if all seven days are specified
	if len(seenDays) == 7 {
		return nil
	}

	return seenDays
}

var weekdayMap = map[string]time.Weekday{
	"mon": time.Monday,
	"tue": time.Tuesday,
	"wed": time.Wednesday,
	"thu": time.Thursday,
	"fri": time.Friday,
	"sat": time.Saturday,
	"sun": time.Sunday,
}

func loadHolidays(filename string) (map[string]bool, error) {
	holidays := make(map[string]bool)

	file, err := os.Open(filename)
	if err != nil {
		return nil, fmt.Errorf("error: cannot read %s", filename)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	lineNum := 0

	for scanner.Scan() {
		lineNum++
		line := scanner.Text()

		// Remove comment
		if idx := strings.Index(line, "#"); idx != -1 {
			line = line[:idx]
		}

		// Trim whitespace
		line = strings.TrimSpace(line)

		// Skip empty lines
		if line == "" {
			continue
		}

		// Validate date
		_, err := parseDate(line)
		if err != nil {
			return nil, fmt.Errorf("error: %s:%d: invalid date", filename, lineNum)
		}

		holidays[line] = true
	}

	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("error: cannot read %s", filename)
	}

	return holidays, nil
}

func isBusinessDay(date time.Time, weekend map[time.Weekday]bool, holidays map[string]bool) bool {
	// Check if it's a weekend
	if weekend[date.Weekday()] {
		return false
	}

	// Check if it's a holiday
	dateStr := date.Format("2006-01-02")
	if holidays[dateStr] {
		return false
	}

	return true
}

func businessDaysBetween(start, end time.Time, weekend map[time.Weekday]bool, holidays map[string]bool) int {
	if start.Equal(end) {
		return 0
	}

	if start.After(end) {
		// If END is before START, return negative
		return -businessDaysBetween(end, start, weekend, holidays)
	}

	count := 0
	current := start
	for current.Before(end) {
		if isBusinessDay(current, weekend, holidays) {
			count++
		}
		current = current.AddDate(0, 0, 1)
	}

	return count
}

func addBusinessDays(date time.Time, n int, weekend map[time.Weekday]bool, holidays map[string]bool) time.Time {
	if n == 0 {
		// Return date if it's a business day, else first business day after
		current := date
		for !isBusinessDay(current, weekend, holidays) {
			current = current.AddDate(0, 0, 1)
		}
		return current
	}

	if n > 0 {
		// Move forward n business days (date itself not counted)
		current := date.AddDate(0, 0, 1)
		count := 0
		for {
			if isBusinessDay(current, weekend, holidays) {
				count++
				if count == n {
					return current
				}
			}
			current = current.AddDate(0, 0, 1)
		}
	} else {
		// Move backward |n| business days (date itself not counted)
		current := date.AddDate(0, 0, -1)
		count := 0
		target := -n
		for {
			if isBusinessDay(current, weekend, holidays) {
				count++
				if count == target {
					return current
				}
			}
			current = current.AddDate(0, 0, -1)
		}
	}
}
