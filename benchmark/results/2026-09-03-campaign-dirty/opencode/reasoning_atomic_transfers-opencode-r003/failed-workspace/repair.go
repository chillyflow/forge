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
	
	// Check all transfers first for validity
	for _, tr := range transfers {
		// Check if accounts exist
		from, fromOK := next[tr.From]
		to, toOK := next[tr.To]
		
		// Reject if accounts don't exist, amount is negative, or insufficient funds
		if !fromOK || !toOK || tr.Amount < 0 || from < tr.Amount {
			return false
		}
		
		// Skip self-transfers (they don't change balances)
		if tr.From == tr.To {
			continue
		}
		
		// Check for integer overflow when adding to destination account
		if to > math.MaxInt-int(tr.Amount) {
			return false
		}
	}
	
	// Apply all transfers if they're all valid
	for _, tr := range transfers {
		// Skip self-transfers
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
