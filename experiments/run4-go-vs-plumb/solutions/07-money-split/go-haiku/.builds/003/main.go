package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
)

type Expense struct {
	payer     string
	amount    int64 // in cents
	split     map[string]int
	splitList []string // to preserve order
}

type Transfer struct {
	from   string
	to     string
	amount int64
}

func main() {
	// Parse command line arguments
	var input io.Reader = os.Stdin
	if len(os.Args) > 2 {
		fmt.Fprintf(os.Stderr, "usage: app [FILE]\n")
		os.Exit(64)
	}
	if len(os.Args) == 2 {
		if strings.HasPrefix(os.Args[1], "-") {
			fmt.Fprintf(os.Stderr, "usage: app [FILE]\n")
			os.Exit(64)
		}
		file, err := os.Open(os.Args[1])
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: cannot read %s\n", os.Args[1])
			os.Exit(2)
		}
		defer file.Close()
		input = file
	}

	// Read and parse expenses
	expenses, people, err := parseInput(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s\n", err)
		os.Exit(1)
	}

	// Calculate balances
	balances := make(map[string]int64)
	for person := range people {
		balances[person] = 0
	}

	for _, exp := range expenses {
		// Payer is credited the full amount
		balances[exp.payer] += exp.amount

		// Calculate how much each participant owes
		totalShares := int64(0)
		for _, shares := range exp.split {
			totalShares += int64(shares)
		}

		// Distribute the amount
		totalCharged := int64(0)
		for _, name := range exp.splitList {
			shares := int64(exp.split[name])
			charged := (exp.amount * shares) / totalShares
			balances[name] -= charged
			totalCharged += charged
		}

		// Distribute remainder to first R participants
		remainder := exp.amount - totalCharged
		for i := 0; i < int(remainder); i++ {
			balances[exp.splitList[i]]--
		}
	}

	// Settle debts
	transfers := settleDebts(balances)

	// Output results
	outputResults(balances, transfers)
}

func parseInput(input io.Reader) ([]Expense, map[string]bool, error) {
	scanner := bufio.NewScanner(input)
	expenses := []Expense{}
	people := make(map[string]bool)
	lineNum := 0

	for scanner.Scan() {
		lineNum++
		line := scanner.Text()

		// Skip empty lines and comment lines
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}

		exp, err := parseLine(trimmed)
		if err != nil {
			return nil, nil, fmt.Errorf("error: line %d: %v", lineNum, err)
		}

		expenses = append(expenses, exp)
		people[exp.payer] = true
		for name := range exp.split {
			people[name] = true
		}
	}

	if err := scanner.Err(); err != nil {
		return nil, nil, fmt.Errorf("error: %v", err)
	}

	return expenses, people, nil
}

func parseLine(line string) (Expense, error) {
	// Split by whitespace
	fields := strings.Fields(line)
	if len(fields) != 3 {
		return Expense{}, fmt.Errorf("invalid expense format")
	}

	payer := fields[0]
	amountStr := fields[1]
	splitStr := fields[2]

	// Validate and parse payer
	if !isValidName(payer) {
		return Expense{}, fmt.Errorf("invalid payer name")
	}

	// Validate and parse amount
	amount, err := parseAmount(amountStr)
	if err != nil {
		return Expense{}, err
	}

	// Parse split
	split, splitList, err := parseSplit(splitStr)
	if err != nil {
		return Expense{}, err
	}

	return Expense{
		payer:     payer,
		amount:    amount,
		split:     split,
		splitList: splitList,
	}, nil
}

func isValidName(name string) bool {
	if name == "" {
		return false
	}
	for _, ch := range name {
		if ch == ' ' || ch == '\t' || ch == ',' || ch == ':' || ch == '#' {
			return false
		}
	}
	return true
}

func parseAmount(amountStr string) (int64, error) {
	// Parse as a decimal number with up to 2 decimal places
	parts := strings.Split(amountStr, ".")
	if len(parts) > 2 {
		return 0, fmt.Errorf("invalid amount")
	}

	// Validate format: digits only before decimal, 1-2 digits after
	if len(parts[0]) == 0 || !isAllDigits(parts[0]) {
		return 0, fmt.Errorf("invalid amount")
	}

	cents := int64(0)
	dollars, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid amount")
	}

	cents = dollars * 100
	if len(parts) > 1 {
		if len(parts[1]) == 0 || len(parts[1]) > 2 || !isAllDigits(parts[1]) {
			return 0, fmt.Errorf("invalid amount")
		}
		frac := parts[1]
		// Pad with zero to make it 2 digits
		for len(frac) < 2 {
			frac += "0"
		}
		fracCents, _ := strconv.ParseInt(frac, 10, 64)
		cents += fracCents
	}

	// Validate range: more than 0 and at most 1000000000.00 (100000000000 cents)
	if cents <= 0 || cents > 100000000000 {
		return 0, fmt.Errorf("invalid amount")
	}

	return cents, nil
}

func isAllDigits(s string) bool {
	for _, ch := range s {
		if ch < '0' || ch > '9' {
			return false
		}
	}
	return true
}

func parseSplit(splitStr string) (map[string]int, []string, error) {
	parts := strings.Split(splitStr, ",")
	if len(parts) == 0 {
		return nil, nil, fmt.Errorf("invalid split")
	}

	split := make(map[string]int)
	splitList := []string{}
	seenNames := make(map[string]bool)

	for _, part := range parts {
		if part == "" {
			return nil, nil, fmt.Errorf("invalid split")
		}

		var name string
		var shares int = 1

		if strings.Contains(part, ":") {
			components := strings.Split(part, ":")
			if len(components) != 2 {
				return nil, nil, fmt.Errorf("invalid split")
			}
			name = components[0]
			sharesStr := components[1]

			if !isValidName(name) {
				return nil, nil, fmt.Errorf("invalid participant name")
			}

			s, err := strconv.Atoi(sharesStr)
			if err != nil || s < 1 || s > 1000 {
				return nil, nil, fmt.Errorf("invalid shares")
			}
			shares = s
		} else {
			name = part
			if !isValidName(name) {
				return nil, nil, fmt.Errorf("invalid participant name")
			}
		}

		if seenNames[name] {
			return nil, nil, fmt.Errorf("duplicate name in split")
		}
		seenNames[name] = true
		split[name] = shares
		splitList = append(splitList, name)
	}

	return split, splitList, nil
}

func settleDebts(balances map[string]int64) []Transfer {
	transfers := []Transfer{}

	// Work with a copy of balances
	workingBalances := make(map[string]int64)
	for k, v := range balances {
		workingBalances[k] = v
	}

	for {
		// Find debtor and creditor
		var debtor string
		var debtorBalance int64
		var creditor string
		var creditorBalance int64

		for person, balance := range workingBalances {
			if balance < 0 {
				if debtor == "" || balance < debtorBalance || (balance == debtorBalance && person < debtor) {
					debtor = person
					debtorBalance = balance
				}
			}
			if balance > 0 {
				if creditor == "" || balance > creditorBalance || (balance == creditorBalance && person < creditor) {
					creditor = person
					creditorBalance = balance
				}
			}
		}

		// If no debts remain, we're done
		if debtor == "" || creditor == "" {
			break
		}

		// Transfer the minimum amount
		transferAmount := -debtorBalance
		if creditorBalance < transferAmount {
			transferAmount = creditorBalance
		}

		transfers = append(transfers, Transfer{
			from:   debtor,
			to:     creditor,
			amount: transferAmount,
		})

		workingBalances[debtor] += transferAmount
		workingBalances[creditor] -= transferAmount
	}

	return transfers
}

func outputResults(balances map[string]int64, transfers []Transfer) {
	// Get sorted list of people
	var people []string
	for person := range balances {
		people = append(people, person)
	}
	sort.Strings(people)

	// Output balances
	fmt.Println("balances:")
	for _, person := range people {
		balance := balances[person]
		sign := "+"
		if balance < 0 {
			sign = "-"
			balance = -balance
		}
		if balance == 0 {
			sign = ""
		}
		dollars := balance / 100
		cents := balance % 100
		fmt.Printf("  %s %s%d.%02d\n", person, sign, dollars, cents)
	}

	// Output transfers
	fmt.Println("transfers:")
	if len(transfers) > 0 {
		for _, t := range transfers {
			dollars := t.amount / 100
			cents := t.amount % 100
			fmt.Printf("  %s -> %s %d.%02d\n", t.from, t.to, dollars, cents)
		}
	}
}
