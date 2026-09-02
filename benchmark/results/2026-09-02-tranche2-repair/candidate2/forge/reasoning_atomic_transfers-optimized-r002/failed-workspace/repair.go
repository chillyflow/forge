package repair

type Transfer struct {
	From, To string
	Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
	maxInt := int(^uint(0) >> 1)
	
	// Create a copy of balances for validation
	validBalances := make(map[string]int, len(balances))
	for key, value := range balances {
		validBalances[key] = value
	}
	
	// First, validate all transfers to ensure atomicity
	for _, tr := range transfers {
		from, fromOK := validBalances[tr.From]
		to, toOK := validBalances[tr.To]
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		if tr.From == tr.To {
			continue
		}
		if to > maxInt-tr.Amount {
			return false
		}
		// Update the validation balances for the next iteration
		validBalances[tr.From] = from - tr.Amount
		validBalances[tr.To] = to + tr.Amount
	}
	
	// If all validations pass, apply transfers to the original balances
	for _, tr := range transfers {
		from, fromOK := balances[tr.From]
		to, toOK := balances[tr.To]
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		if tr.From == tr.To {
			continue
		}
		balances[tr.From] = from - tr.Amount
		balances[tr.To] = to + tr.Amount
	}
	return true
}
