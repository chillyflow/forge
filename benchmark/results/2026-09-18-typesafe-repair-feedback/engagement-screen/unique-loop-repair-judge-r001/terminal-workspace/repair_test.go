package repair

import "testing"

func TestRepair(t *testing.T) {
	got := Unique([]int{3, 1, 3, 2, 1})
	want := []int{3, 1, 2}
	if len(got) != len(want) {
		t.Fatalf("got %v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %v", got)
		}
	}
}
