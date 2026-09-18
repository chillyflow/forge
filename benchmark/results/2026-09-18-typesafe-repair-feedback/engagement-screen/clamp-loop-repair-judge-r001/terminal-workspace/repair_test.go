package repair

import "testing"

func TestRepair(t *testing.T) {
	for _, c := range [][4]int{{-1, 0, 10, 0}, {11, 0, 10, 10}, {5, 0, 10, 5}, {0, 0, 10, 0}} {
		if got := Clamp(c[0], c[1], c[2]); got != c[3] {
			t.Fatalf("%v: got %d", c, got)
		}
	}
}
