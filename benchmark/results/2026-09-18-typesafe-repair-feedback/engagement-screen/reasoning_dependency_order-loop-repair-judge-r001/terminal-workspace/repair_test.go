package repair

import (
	"reflect"
	"testing"
)

func TestOrderBuild(t *testing.T) {
	tasks := []string{"package", "compile", "generate", "lint"}
	deps := map[string][]string{"package": {"compile"}, "compile": {"generate"}}
	if got, ok := OrderBuild(tasks, deps); !ok || !reflect.DeepEqual(got, []string{"generate", "compile", "lint", "package"}) {
		t.Fatalf("order=%v ok=%v", got, ok)
	}
	if _, ok := OrderBuild([]string{"a"}, map[string][]string{"a": {"missing"}}); ok {
		t.Fatal("unknown dependency accepted")
	}
	if _, ok := OrderBuild([]string{"a", "b"}, map[string][]string{"a": {"b"}, "b": {"a"}}); ok {
		t.Fatal("cycle accepted")
	}
}
