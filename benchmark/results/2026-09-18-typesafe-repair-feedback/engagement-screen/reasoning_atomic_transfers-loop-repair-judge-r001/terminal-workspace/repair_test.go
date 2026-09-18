package repair

import (
	"math"
	"reflect"
	"testing"
)

func TestApplyTransfers(t *testing.T) {
	b := map[string]int{"a": 10, "b": 0, "c": 2}
	if !ApplyTransfers(b, []Transfer{{"a", "b", 7}, {"b", "c", 3}}) || !reflect.DeepEqual(b, map[string]int{"a": 3, "b": 4, "c": 5}) {
		t.Fatalf("success=%v", b)
	}
	for _, batch := range [][]Transfer{{{"a", "b", 4}, {"a", "c", 1}}, {{"a", "missing", 1}}, {{"a", "b", -1}}} {
		before := map[string]int{"a": 4, "b": 0, "c": 0}
		got := map[string]int{"a": 4, "b": 0, "c": 0}
		if ApplyTransfers(got, batch) || !reflect.DeepEqual(got, before) {
			t.Fatalf("batch %v left %v", batch, got)
		}
	}
	overflow := map[string]int{"a": 1, "b": math.MaxInt}
	if ApplyTransfers(overflow, []Transfer{{"a", "b", 1}}) || overflow["a"] != 1 {
		t.Fatalf("overflow=%v", overflow)
	}
	same := map[string]int{"a": 5}
	if !ApplyTransfers(same, []Transfer{{"a", "a", 4}}) || same["a"] != 5 {
		t.Fatalf("self transfer=%v", same)
	}
}
