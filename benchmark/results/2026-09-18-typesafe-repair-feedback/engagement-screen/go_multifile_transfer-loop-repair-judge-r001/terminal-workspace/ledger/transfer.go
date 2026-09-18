package ledger

import "errors"

func Transfer(from, to *Ledger, amount int) error {
	if !from.debit(amount) {
		return errors.New("insufficient funds")
	}
	to.Balance += amount
	return nil
}
