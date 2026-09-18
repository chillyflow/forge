package store

import "testing"

func TestMemory(t *testing.T) {
	var s Store = New([]Record{{ID: "x", Value: 9}})
	got, ok := s.Get("x")
	if !ok || got.Value != 9 {
		t.Fatalf("got %v %v", got, ok)
	}
}
