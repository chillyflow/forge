package holdout

import (
	"reflect"
	"testing"
)

func TestReserve(t *testing.T) {
	for _, batch := range [][]Booking{
		{{"B", "bob"}, {"B", "bea"}},
		{{"B", "bob"}, {"A", "bea"}},
		{{"B", "bob"}, {"C", ""}},
	} {
		b := map[string]string{"A": "ann"}
		if Reserve(b, batch) || !reflect.DeepEqual(b, map[string]string{"A": "ann"}) {
			t.Fatalf("rejected reservation changed bookings: %v", b)
		}
	}
	b := map[string]string{"A": "ann"}
	if !Reserve(b, []Booking{{"B", "bob"}, {"C", "cam"}}) || len(b) != 3 {
		t.Fatal(b)
	}
	if !Reserve(b, nil) || b["B"] != "bob" {
		t.Fatal(b)
	}
}
