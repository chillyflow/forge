package repair

import "testing"

func TestRepair(t *testing.T) {
	if Average([]int{1, 2}) != 1.5 {
		t.Fatal("fraction truncated")
	}
	if Average(nil) != 0 {
		t.Fatal("empty input")
	}
}
