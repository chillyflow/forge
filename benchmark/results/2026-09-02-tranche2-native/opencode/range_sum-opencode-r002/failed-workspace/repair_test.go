package repair

import "testing"

func TestRepair(t *testing.T) {
	if RangeSum(1, 3) != 6 || RangeSum(5, 5) != 5 || RangeSum(3, 1) != 0 || RangeSum(-2, 2) != 0 {
		t.Fatal("inclusive sum failed")
	}
}
