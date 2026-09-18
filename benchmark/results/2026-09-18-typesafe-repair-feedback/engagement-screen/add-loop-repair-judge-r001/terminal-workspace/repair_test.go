package repair

import "testing"

func TestRepair(t *testing.T) {
	if got := Add(2, 3); got != 5 {
		t.Fatalf("expected 5, got %d", got)
	}
	if Add(-2, 3) != 1 {
		t.Fatal("negative operands")
	}
}
