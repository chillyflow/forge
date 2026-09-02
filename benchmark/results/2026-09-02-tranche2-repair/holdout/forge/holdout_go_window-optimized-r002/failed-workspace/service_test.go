package holdout

import "testing"

func TestWindow(t *testing.T) {
	w := Window{Width: 5}
	for _, tc := range []struct{ at, value, want int }{{0, 3, 3}, {4, 2, 5}, {5, 7, 9}, {5, -2, 7}, {10, 1, 1}} {
		got, ok := w.Add(tc.at, tc.value)
		if !ok || got != tc.want {
			t.Fatalf("at %d sum %d", tc.at, got)
		}
	}
	if _, ok := w.Add(9, 100); ok {
		t.Fatal("backdated sample accepted")
	}
	if got, ok := w.Add(10, 2); !ok || got != 3 {
		t.Fatal(got, ok)
	}
}
