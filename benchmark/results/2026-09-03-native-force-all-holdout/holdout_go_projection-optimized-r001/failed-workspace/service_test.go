package holdout

import (
	"reflect"
	"testing"
)

func TestProject(t *testing.T) {
	events := []Event{{"x", 5, "new", false}, {"x", 6, "", true}, {"x", 4, "stale", false},
		{"y", 1, "first", false}, {"y", 1, "duplicate", false}, {"z", 0, "zero", false}}
	got := Project(events)
	if !reflect.DeepEqual(got, map[string]string{"y": "first", "z": "zero"}) {
		t.Fatalf("projection %v", got)
	}
	events = append(events, Event{"x", 7, "reborn", false})
	if Project(events)["x"] != "reborn" {
		t.Fatal("new revision rejected")
	}
	if len(Project(nil)) != 0 {
		t.Fatal("empty input")
	}
}
