package ledger

import "testing"

func TestTransfer(t *testing.T) {
	a, b := &Ledger{10}, &Ledger{2}
	if err := Transfer(a, b, 10); err != nil || a.Balance != 0 || b.Balance != 12 {
		t.Fatalf("exact: %v %d %d", err, a.Balance, b.Balance)
	}
	c, d := &Ledger{3}, &Ledger{4}
	if err := Transfer(c, d, 5); err == nil || c.Balance != 3 || d.Balance != 4 {
		t.Fatalf("atomic: %v %d %d", err, c.Balance, d.Balance)
	}
}
