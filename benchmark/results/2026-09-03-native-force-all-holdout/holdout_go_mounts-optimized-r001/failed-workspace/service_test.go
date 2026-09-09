package holdout

import "testing"

func TestMounts(t *testing.T) {
	for _, tc := range []struct{ request, mount, rest string }{
		{"/apple/x", "/", "apple/x"}, {"/app/a", "/app", "a"}, {"/app", "/app", ""},
		{"/app/v2/x", "/app/v2", "x"}, {"/app/../apple", "/", "apple"},
	} {
		mount, rest, ok := ResolveMount(tc.request, []string{"/app", "/", "/app/v2"})
		if !ok || mount != tc.mount || rest != tc.rest {
			t.Fatalf("%s: %q %q %v", tc.request, mount, rest, ok)
		}
	}
	if _, _, ok := ResolveMount("/apple", []string{"/app"}); ok {
		t.Fatal("partial component matched")
	}
}
