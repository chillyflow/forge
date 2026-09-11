package repair

import (
	"math"
	"reflect"
	"testing"
)

func TestReplayEvents(t *testing.T) {
	events := []Event{{1, "a", "Add", "x", 3}, {2, "b", "Add", "x", -1}, {2, "b", "Add", "x", -1}, {3, "c", "Delete", "x", 0}, {4, "d", "Add", "y", 5}}
	if got, ok := ReplayEvents(events); !ok || !reflect.DeepEqual(got, map[string]int{"y": 5}) {
		t.Fatalf("state=%v ok=%v", got, ok)
	}
	invalid := [][]Event{{{2, "a", "Add", "x", 1}}, {{1, "a", "Add", "x", 1}, {1, "a", "Add", "x", 2}}, {{1, "a", "Delete", "x", 1}}, {{1, "a", "Other", "x", 0}}, {{1, "a", "Add", "x", math.MaxInt}, {2, "b", "Add", "x", 1}}}
	for _, stream := range invalid {
		if got, ok := ReplayEvents(stream); ok || got != nil {
			t.Fatalf("accepted %v: %v", stream, got)
		}
	}
}
