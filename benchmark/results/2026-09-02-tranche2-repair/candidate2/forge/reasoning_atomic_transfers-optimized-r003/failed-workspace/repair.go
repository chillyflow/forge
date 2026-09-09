package repair

type Transfer struct {
	From, To string
	Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
	maxInt := int(^uint(0) >> 1)

	// First, validate all transfers to ensure atomicity
	for _, tr := range transfers {
		from, fromOK := balances[tr.From]
		to, toOK := balances[tr.To]
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		if tr.From == tr.To {
			continue
		}
		if to > maxInt-tr.Amount {
			return false
		}
	}

	// If all validations pass, apply transfers
	for _, tr := range transfers {
		if tr.From == tr.To {
			continue
		}
		from := balances[tr.From]
		to := balances[tr.To]
		balances[tr.From] = from - tr.Amount
		balances[tr.To] = to + tr.Amount
	}
	return true
}
