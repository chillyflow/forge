package holdout

import "testing"

func TestCache(t *testing.T) {
	c := Cache{Capacity: 2}
	c.Put("a", 1)
	c.Put("b", 2)
	c.Put("a", 3)
	c.Put("c", 4)
	if _, ok := c.Get("b"); ok {
		t.Fatal("least recent key retained")
	}
	if v, ok := c.Get("a"); !ok || v != 3 {
		t.Fatal("updated key evicted", v, ok)
	}
	c.Put("d", 5)
	if _, ok := c.Get("c"); ok {
		t.Fatal("Get did not refresh recency")
	}
	z := Cache{Capacity: 0}
	z.Put("x", 1)
	if _, ok := z.Get("x"); ok {
		t.Fatal("zero capacity")
	}
	one := Cache{Capacity: 1}
	one.Put("x", 1)
	one.Put("x", 2)
	if v, ok := one.Get("x"); !ok || v != 2 {
		t.Fatal(v, ok)
	}
}
