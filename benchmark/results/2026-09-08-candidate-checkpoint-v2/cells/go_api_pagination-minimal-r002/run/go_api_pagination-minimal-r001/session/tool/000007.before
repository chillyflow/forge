package api

import (
	"encoding/json"
	"net/http/httptest"
	"reflect"
	"testing"
)

func TestPagination(t *testing.T) {
	h := ListHandler([]string{"a", "b", "c", "d", "e"})
	r := httptest.NewRequest("GET", "/?page=2&size=2", nil)
	w := httptest.NewRecorder()
	h(w, r)
	var got []string
	json.Unmarshal(w.Body.Bytes(), &got)
	if !reflect.DeepEqual(got, []string{"c", "d"}) {
		t.Fatalf("got %v", got)
	}
	bad := httptest.NewRequest("GET", "/?page=1&size=nope", nil)
	bw := httptest.NewRecorder()
	h(bw, bad)
	if bw.Code != 400 {
		t.Fatalf("bad status %d", bw.Code)
	}
}
