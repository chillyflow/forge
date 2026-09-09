package repair

type Transfer struct {
	From, To string
	Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
	// Create a copy of balances to test the transaction
	next := make(map[string]int, len(balances))
	for key, value := range balances {
		next[key] = value
	}
	maxInt := int(^uint(0) >> 1)
	for _, tr := range transfers {
		from, fromOK := balances[tr.From]
		to, toOK := balances[tr.To]
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		if tr.From == tr.To {
			continue
		}
		if tr.Amount > maxInt-to {
			return false
		}
		next[tr.From] = from - tr.Amount
		next[tr.To] = to + tr.Amount
	}
	// Apply the changes to the original balances map
	for key, value := range next {
		balances[key] = value
	}
	return true
}
