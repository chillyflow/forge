package session

import (
	"testing"
	"time"
)

type fixed struct{ at time.Time }

func (f fixed) Now() time.Time { return f.at }
func TestExpiredUsesClock(t *testing.T) {
	base := time.Date(2030, 1, 1, 0, 0, 0, 0, time.UTC)
	if New(base.Add(time.Hour), fixed{base}).Expired() {
		t.Fatal("future marked expired")
	}
	if !New(base.Add(-time.Hour), fixed{base}).Expired() {
		t.Fatal("past marked active")
	}
}
