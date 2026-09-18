package config

import (
	"reflect"
	"testing"
)

func TestPrecedence(t *testing.T) {
	got := Resolve(Values{"host": "default", "port": "80"}, Values{"host": "file"}, Values{"host": "runtime"})
	want := Values{"host": "runtime", "port": "80"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v", got)
	}
}
