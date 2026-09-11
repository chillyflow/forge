package ledger

type Ledger struct{ Balance int }

func (l *Ledger) Debit(amount int) bool {
	if amount < 0 || l.Balance < amount {
		return false
	}
	l.Balance -= amount
	return true
}
