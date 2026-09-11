package ledger

import "errors"

func Transfer(from, to *Ledger, amount int) error {
	if amount < 0 {
		return errors.New("invalid amount")
	}

	// Check if source has sufficient funds
	if !from.debit(amount) {
		return errors.New("insufficient funds")
	}

	// Credit the destination ledger
	to.Balance += amount

	return nil
}
