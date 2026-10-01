// business-days: business-day arithmetic on the command line.
//
// Dates are held as day numbers: days since 0001-01-01, which is a Monday
// in the proleptic Gregorian calendar. That makes the weekday of day n
// simply n mod 7 (0 = Monday), and lets every query be answered by a
// counting function plus a binary search, so large N is as fast as small N.
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
)

const maxDay = 3652058 // 9999-12-31

type usageError struct{ msg string }
type dataError struct{ msg string }

func (e usageError) Error() string { return e.msg }
func (e dataError) Error() string  { return e.msg }

const usageText = "usage: app between START END [--holidays FILE] [--weekend DAYS]\n" +
	"       app add DATE N [--holidays FILE] [--weekend DAYS]"

func usage(why string) error {
	return usageError{usageText + "\n" + why}
}

func main() {
	out, err := run(os.Args[1:])
	if err != nil {
		switch e := err.(type) {
		case usageError:
			fmt.Fprintln(os.Stderr, e.msg)
			os.Exit(64)
		default:
			fmt.Fprintln(os.Stderr, e.Error())
			os.Exit(2)
		}
	}
	fmt.Println(out)
}

var weekdayNames = map[string]int{"mon": 0, "tue": 1, "wed": 2, "thu": 3, "fri": 4, "sat": 5, "sun": 6}

func run(args []string) (string, error) {
	if len(args) == 0 {
		return "", usage("missing command")
	}
	cmd := args[0]
	if cmd != "between" && cmd != "add" {
		return "", usage("unknown command: " + cmd)
	}
	var pos []string
	holidaysFile, hasHolidays := "", false
	weekendArg := "sat,sun"
	rest := args[1:]
	for i := 0; i < len(rest); i++ {
		a := rest[i]
		switch {
		case a == "--holidays" || a == "--weekend":
			if i+1 >= len(rest) {
				return "", usage("missing value for " + a)
			}
			i++
			if a == "--holidays" {
				holidaysFile, hasHolidays = rest[i], true
			} else {
				weekendArg = rest[i]
			}
		case strings.HasPrefix(a, "--"):
			return "", usage("unknown option: " + a)
		case strings.HasPrefix(a, "-") && !isNumberLike(a):
			return "", usage("unknown option: " + a)
		default:
			pos = append(pos, a)
		}
	}
	if len(pos) != 2 {
		return "", usage("wrong number of arguments")
	}

	var weekend [7]bool
	if weekendArg != "" {
		for _, name := range strings.Split(weekendArg, ",") {
			d, ok := weekdayNames[name]
			if !ok {
				return "", usage("unknown weekend day: " + name)
			}
			weekend[d] = true
		}
	}
	all := true
	for _, w := range weekend {
		all = all && w
	}
	if all {
		return "", usage("all seven days cannot be weekend days")
	}

	var n int64
	nClamped := false
	if cmd == "add" {
		if !isWholeNumber(pos[1]) {
			return "", usage("N is not a whole number: " + pos[1])
		}
		v, err := strconv.ParseInt(pos[1], 10, 64)
		if err != nil {
			nClamped = true // far beyond any representable date
			if strings.HasPrefix(pos[1], "-") {
				v = -1 << 62
			} else {
				v = 1 << 62
			}
		}
		n = v
	}

	first, ok := parseDate(pos[0])
	if !ok {
		return "", dataError{"error: invalid date: " + pos[0]}
	}
	second := 0
	if cmd == "between" {
		if second, ok = parseDate(pos[1]); !ok {
			return "", dataError{"error: invalid date: " + pos[1]}
		}
	}

	var hol []int
	if hasHolidays {
		var err error
		if hol, err = loadHolidays(holidaysFile, weekend); err != nil {
			return "", err
		}
	}
	cal := newCalendar(weekend, hol)

	if cmd == "between" {
		return strconv.Itoa(cal.before(second) - cal.before(first)), nil
	}
	_ = nClamped
	t, ok := cal.add(first, n)
	if !ok {
		return "", dataError{"error: result out of range"}
	}
	return formatDate(t), nil
}

func isWholeNumber(s string) bool {
	s = strings.TrimPrefix(s, "-")
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

func isNumberLike(s string) bool { return strings.HasPrefix(s, "-") && isWholeNumber(s) }

// ---- dates ----

func isLeap(y int) bool { return y%4 == 0 && (y%100 != 0 || y%400 == 0) }

func daysInMonth(y, m int) int {
	switch m {
	case 2:
		if isLeap(y) {
			return 29
		}
		return 28
	case 4, 6, 9, 11:
		return 30
	}
	return 31
}

// daysFromCivil returns days since 0001-01-01 for a valid date.
func daysFromCivil(y, m, d int) int {
	py := y - 1
	n := py*365 + py/4 - py/100 + py/400
	for i := 1; i < m; i++ {
		n += daysInMonth(y, i)
	}
	return n + d - 1
}

func parseDate(s string) (int, bool) {
	if len(s) != 10 || s[4] != '-' || s[7] != '-' {
		return 0, false
	}
	num := func(a string) (int, bool) {
		v := 0
		for i := 0; i < len(a); i++ {
			if a[i] < '0' || a[i] > '9' {
				return 0, false
			}
			v = v*10 + int(a[i]-'0')
		}
		return v, true
	}
	y, ok1 := num(s[0:4])
	m, ok2 := num(s[5:7])
	d, ok3 := num(s[8:10])
	if !ok1 || !ok2 || !ok3 || y < 1 || m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m) {
		return 0, false
	}
	return daysFromCivil(y, m, d), true
}

func formatDate(n int) string {
	// Find year by estimate then adjust.
	y := n/366 + 1
	for daysFromCivil(y+1, 1, 1) <= n {
		y++
	}
	rem := n - daysFromCivil(y, 1, 1)
	m := 1
	for rem >= daysInMonth(y, m) {
		rem -= daysInMonth(y, m)
		m++
	}
	return fmt.Sprintf("%04d-%02d-%02d", y, m, rem+1)
}

// ---- holidays ----

func loadHolidays(file string, weekend [7]bool) ([]int, error) {
	data, err := os.ReadFile(file)
	if err != nil {
		return nil, dataError{"error: cannot read " + file}
	}
	var days []int
	for i, line := range strings.Split(string(data), "\n") {
		if j := strings.IndexByte(line, '#'); j >= 0 {
			line = line[:j]
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		d, ok := parseDate(line)
		if !ok {
			return nil, dataError{fmt.Sprintf("error: %s:%d: invalid date", file, i+1)}
		}
		if !weekend[d%7] {
			days = append(days, d)
		}
	}
	return days, nil
}

// ---- calendar ----

type calendar struct {
	weekend  [7]bool
	perWeek  int
	prefix   [8]int // prefix[r] = business weekdays among the first r weekdays of a week
	holidays []int  // sorted, unique, non-weekend
}

func newCalendar(weekend [7]bool, hol []int) *calendar {
	c := &calendar{weekend: weekend}
	for i := 0; i < 7; i++ {
		c.prefix[i+1] = c.prefix[i]
		if !weekend[i] {
			c.prefix[i+1]++
		}
	}
	c.perWeek = c.prefix[7]
	sort.Ints(hol)
	for i, h := range hol {
		if i == 0 || h != hol[i-1] {
			c.holidays = append(c.holidays, h)
		}
	}
	return c
}

// before counts business days d with 0 <= d < day.
func (c *calendar) before(day int) int {
	n := day/7*c.perWeek + c.prefix[day%7]
	return n - sort.SearchInts(c.holidays, day)
}

// add returns the target for add DATE N; ok is false if it falls outside
// 0001-01-01..9999-12-31.
func (c *calendar) add(day int, n int64) (int, bool) {
	// m: 1-based index of the wanted business day among all business days.
	var m int64
	switch {
	case n > 0:
		m = int64(c.before(day+1)) + n
	case n < 0:
		m = int64(c.before(day)) + n + 1
	default:
		m = int64(c.before(day)) + 1
	}
	if m < 1 || m > int64(c.before(maxDay+1)) {
		return 0, false
	}
	// smallest t with before(t+1) >= m
	t := sort.Search(maxDay+1, func(t int) bool { return int64(c.before(t+1)) >= m })
	return t, true
}
