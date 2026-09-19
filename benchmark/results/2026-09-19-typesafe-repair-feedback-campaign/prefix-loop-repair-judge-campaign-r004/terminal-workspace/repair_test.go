package repair

import "testing"

func TestRepair(t *testing.T) {
	if TrimPrefix("abab", "ab") != "ab" || TrimPrefix("zab", "ab") != "zab" || TrimPrefix("abc", "") != "abc" {
		t.Fatal("prefix semantics failed")
	}
}
