package repair

import "testing"

func TestRepair(t *testing.T) {
	if LastIndex([]int{2, 1, 2}, 2) != 2 || LastIndex(nil, 0) != -1 || LastIndex([]int{3}, 4) != -1 {
		t.Fatal("last occurrence failed")
	}
}
