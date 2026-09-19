package repair

import (
	"reflect"
	"testing"
)

func TestMatchRoute(t *testing.T) {
	routes := []Route{{"*", "/users/:id", "generic-user"}, {"GET", "/users/me", "self"}, {"GET", "/users/:id", "get-user"}, {"GET", "/files/*", "files"}}
	if name, p, ok := MatchRoute("GET", "/users/me", routes); !ok || name != "self" || len(p) != 0 {
		t.Fatalf("self: %s %v %v", name, p, ok)
	}
	if name, p, ok := MatchRoute("GET", "/users/42", routes); !ok || name != "get-user" || !reflect.DeepEqual(p, map[string]string{"id": "42"}) {
		t.Fatalf("user: %s %v %v", name, p, ok)
	}
	if name, p, ok := MatchRoute("GET", "/files/a/b.txt", routes); !ok || name != "files" || p["*"] != "a/b.txt" {
		t.Fatalf("files: %s %v %v", name, p, ok)
	}
	if _, _, ok := MatchRoute("GET", "/files", routes); ok {
		t.Fatal("empty wildcard matched")
	}
}
