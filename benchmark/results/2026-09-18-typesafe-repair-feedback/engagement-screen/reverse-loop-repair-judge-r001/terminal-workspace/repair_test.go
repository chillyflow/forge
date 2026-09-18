package repair

import "testing"

func TestRepair(t *testing.T) {
	xs := []int{1, 2, 3, 4, 5}
	Reverse(xs)
	for i, v := range xs {
		if v != 5-i {
			t.Fatalf("got %v", xs)
		}
	}
	Reverse(nil)
}
