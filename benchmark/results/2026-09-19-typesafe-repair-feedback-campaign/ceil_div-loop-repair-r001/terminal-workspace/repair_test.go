package repair

import "testing"

func TestRepair(t *testing.T) {
	for _, c := range [][3]int{{0, 3, 0}, {1, 3, 1}, {6, 3, 2}, {7, 3, 3}} {
		if CeilDiv(c[0], c[1]) != c[2] {
			t.Fatalf("%v", c)
		}
	}
}
