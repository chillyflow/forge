package registry

import "testing"

func TestNormalizedNames(t *testing.T) {
	r := New()
	r.Put("  Alpha ", 7)
	for _, key := range []string{"alpha", " ALPHA ", "Alpha"} {
		if got, ok := r.Get(key); !ok || got != 7 {
			t.Fatalf("%q=(%d,%v)", key, got, ok)
		}
	}
}
