package repair

import (
	"reflect"
	"testing"
)

func TestAllocateQuota(t *testing.T) {
	requests := []Request{{"a", 1, 10, 1}, {"b", 1, 10, 2}, {"c", 0, 10, 1}}
	if got, ok := AllocateQuota(10, requests); !ok || !reflect.DeepEqual(got, map[string]int{"a": 3, "b": 5, "c": 2}) {
		t.Fatalf("weighted=%v ok=%v", got, ok)
	}
	capped := []Request{{"a", 0, 1, 100}, {"b", 0, 9, 1}}
	if got, ok := AllocateQuota(5, capped); !ok || !reflect.DeepEqual(got, map[string]int{"a": 1, "b": 4}) {
		t.Fatalf("capped=%v ok=%v", got, ok)
	}
	tied := []Request{{"b", 0, 2, 1}, {"a", 0, 2, 1}}
	if got, ok := AllocateQuota(1, tied); !ok || got["a"] != 1 {
		t.Fatalf("tie=%v ok=%v", got, ok)
	}
	for _, bad := range [][]Request{{{"x", 2, 1, 1}}, {{"x", 0, 1, 0}}, {{"x", 0, 1, 1}, {"x", 0, 1, 1}}} {
		if got, ok := AllocateQuota(1, bad); ok || got != nil {
			t.Fatalf("accepted %v: %v", bad, got)
		}
	}
	if _, ok := AllocateQuota(3, []Request{{"x", 1, 2, 1}}); ok {
		t.Fatal("infeasible capacity accepted")
	}
}
