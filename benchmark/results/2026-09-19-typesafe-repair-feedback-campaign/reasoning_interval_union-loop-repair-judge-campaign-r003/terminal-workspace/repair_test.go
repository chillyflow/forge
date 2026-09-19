package repair

import (
	"reflect"
	"testing"
)

func TestUnionIntervals(t *testing.T) {
	in := []Interval{{8, 6}, {1, 2}, {4, 5}, {2, 3}, {20, 20}, {21, 22}}
	saved := append([]Interval(nil), in...)
	want := []Interval{{1, 8}, {20, 22}}
	if got := UnionIntervals(in); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v", got)
	}
	if !reflect.DeepEqual(in, saved) {
		t.Fatalf("mutated input: %v", in)
	}
	if got := UnionIntervals(nil); got == nil || len(got) != 0 {
		t.Fatalf("empty=%v", got)
	}
}
