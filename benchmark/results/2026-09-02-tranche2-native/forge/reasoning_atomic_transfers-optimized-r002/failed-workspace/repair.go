package repair

type Transfer struct {
	From, To string
	Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
	maxInt := int(^uint(0) >> 1)

	// First, validate all transfers to ensure atomicity
	// Create a copy of balances to track intermediate state
	next := make(map[string]int, len(balances))
	for key, value := range balances {
		next[key] = value
	}

	for _, tr := range transfers {
		from, fromOK := next[tr.From]
		to, toOK := next[tr.To]

		// Check for negative amounts
		if tr.Amount < 0 {
			return false
		}

		// Check for missing accounts
		if !fromOK || !toOK {
			return false
		}

		// Check for insufficient funds
		if from < tr.Amount {
			return false
		}

		// Check for integer overflow when adding to destination
		if to > maxInt-tr.Amount {
			return false
		}
	}

	// If all validations pass, apply transfers to the original balances
	for _, tr := range transfers {
		if tr.From == tr.To {
			continue
		}
		next[tr.From] = next[tr.From] - tr.Amount
		next[tr.To] = next[tr.To] + tr.Amount
	}

	// Update balances with new values
	for key, value := range next {
		balances[key] = value
	}
	return true
}
