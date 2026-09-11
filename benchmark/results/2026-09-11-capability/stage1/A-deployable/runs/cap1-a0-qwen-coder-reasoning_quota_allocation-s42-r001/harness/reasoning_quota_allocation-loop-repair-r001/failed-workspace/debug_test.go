package repair

import "testing"

func TestDebug(t *testing.T) {
	// Test case that's failing
	capped := []Request{{"a", 0, 1, 100}, {"b", 0, 9, 1}}
	got, ok := AllocateQuota(5, capped)
	if !ok {
		t.Fatalf("Allocation failed")
	}
	t.Logf("Got: %v", got)
	
	// Expected: map[a:1 b:4]
	expected := map[string]int{"a": 1, "b": 4}
	if got["a"] != expected["a"] || got["b"] != expected["b"] {
		t.Fatalf("Expected %v, got %v", expected, got)
	}
}