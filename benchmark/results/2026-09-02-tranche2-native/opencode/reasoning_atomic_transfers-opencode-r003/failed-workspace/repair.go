package repair

import "math"

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
	
	// Check all transfers first before applying any
	for _, tr := range transfers {
		// Check for negative amounts
		if tr.Amount < 0 {
			return false
		}
		
		// Check if from account exists
		from, fromOK := next[tr.From]
		if !fromOK {
			return false
		}
		
		// Check if to account exists
		to, toOK := next[tr.To]
		if !toOK {
			return false
		}
		
		// Check for insufficient funds
		if from < tr.Amount {
			return false
		}
		
		// Check for integer overflow on to account
		if to > math.MaxInt32-tr.Amount {
			return false
		}
	}
	
	// Apply all transfers
	for _, tr := range transfers {
		// Skip self transfers (they don't change balances)
		if tr.From == tr.To {
			continue
		}
		
		// Apply the transfer
		next[tr.From] = next[tr.From] - tr.Amount
		next[tr.To] = next[tr.To] + tr.Amount
	}
	
	// Update the original balances map
	for key, value := range next {
		balances[key] = value
	}
	
	return true
}
