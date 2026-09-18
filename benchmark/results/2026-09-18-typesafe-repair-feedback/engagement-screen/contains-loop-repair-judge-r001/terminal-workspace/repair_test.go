package repair

import "testing"

func TestRepair(t *testing.T) {
	if !Contains([]int{1, 2, 3}, 3) || !Contains([]int{7}, 7) || Contains(nil, 1) || Contains([]int{1}, 2) {
		t.Fatal("membership failed")
	}
}
