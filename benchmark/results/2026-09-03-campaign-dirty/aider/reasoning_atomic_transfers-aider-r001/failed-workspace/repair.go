package repair

import (
	"math"
)

type Transfer struct {
	From, To string
	Amount   int
}

func ApplyTransfers(balances map[string]int, transfers []Transfer) bool {
	// Validate all transfers first
	for _, tr := range transfers {
		from, fromOK := balances[tr.From]
		to, toOK := balances[tr.To]
		
		// Check for invalid transfers
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		
		// Check for integer overflow
		if to > math.MaxInt32-tr.Amount {
			return false
		}
	}
	
	// Apply all transfers
	next := make(map[string]int, len(balances))
	for key, value := range balances {
		next[key] = value
	}
	
	for _, tr := range transfers {
		if tr.From == tr.To {
			continue
		}
		
		from := next[tr.From]
		to := next[tr.To]
		
		next[tr.From] = from - tr.Amount
		next[tr.To] = to + tr.Amount
	}
	
	// Update balances
	for key, value := range next {
		balances[key] = value
	}
	
	return true
}
