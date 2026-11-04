package main

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

type seen struct {
	n      int
	cookie string
	tokHdr string
	bodyN  int
}

func upstream(t *testing.T) (*httptest.Server, *seen) {
	s := &seen{}
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.n++
		s.cookie = r.Header.Get("Cookie")
		s.tokHdr = r.Header.Get("X-VM-Token")
		b, _ := io.ReadAll(r.Body)
		s.bodyN = len(b)
		w.Write([]byte(`{"ok":true}`))
	}))
	t.Cleanup(ts.Close)
	return ts, s
}

func handler(t *testing.T, token string) (http.Handler, *seen) {
	ts, s := upstream(t)
	u, _ := url.Parse(ts.URL)
	return newVMHandler(u, token), s
}

func do(h http.Handler, method, path string, body []byte, hdr map[string]string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Host = "dash.local:20300"
	for k, v := range hdr {
		r.Header.Set(k, v)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}

var okOrigin = map[string]string{"Origin": "http://dash.local:20300"}

func TestMethodPathAllowList(t *testing.T) {
	h, s := handler(t, "")
	if w := do(h, "GET", "/api/vm/status", nil, nil); w.Code != 200 {
		t.Fatalf("GET status: %d", w.Code)
	}
	for _, c := range []struct{ m, p string }{
		{"PUT", "/api/vm/status"}, {"DELETE", "/api/vm/frames/1"}, {"PATCH", "/api/vm/search/image"},
		{"POST", "/api/vm/status"}, {"POST", "/api/vm/frames/1"},
	} {
		if w := do(h, c.m, c.p, nil, okOrigin); w.Code != 405 {
			t.Errorf("%s %s: got %d want 405", c.m, c.p, w.Code)
		}
	}
	n := s.n
	if w := do(h, "POST", "/api/vm/search/frame?frame_id=1", nil, okOrigin); w.Code != 200 {
		t.Fatalf("POST search: %d", w.Code)
	}
	if s.n != n+1 {
		t.Fatal("search POST not proxied")
	}
}

func TestOriginCheck(t *testing.T) {
	h, _ := handler(t, "")
	p := "/api/vm/search/image"
	if w := do(h, "POST", p, []byte("x"), nil); w.Code != 403 {
		t.Errorf("no origin: %d", w.Code)
	}
	if w := do(h, "POST", p, []byte("x"), map[string]string{"Origin": "http://evil.example"}); w.Code != 403 {
		t.Errorf("evil origin: %d", w.Code)
	}
	if w := do(h, "POST", p, []byte("x"), map[string]string{"Referer": "http://dash.local:20300/#search"}); w.Code != 200 {
		t.Errorf("referer ok: %d", w.Code)
	}
	if w := do(h, "POST", p, []byte("x"), okOrigin); w.Code != 200 {
		t.Errorf("origin ok: %d", w.Code)
	}
}

func TestTokenRequiredAndStripped(t *testing.T) {
	h, s := handler(t, "s3cret")
	if w := do(h, "GET", "/api/vm/status", nil, nil); w.Code != 401 {
		t.Fatalf("no token: %d", w.Code)
	}
	if w := do(h, "GET", "/api/vm/status", nil, map[string]string{"X-VM-Token": "bad"}); w.Code != 401 {
		t.Fatalf("bad token: %d", w.Code)
	}
	w := do(h, "GET", "/api/vm/status", nil, map[string]string{"X-VM-Token": "s3cret", "Cookie": "vm_token=s3cret; other=1"})
	if w.Code != 200 {
		t.Fatalf("good token: %d", w.Code)
	}
	if s.tokHdr != "" || strings.Contains(s.cookie, "vm_token") || s.cookie != "other=1" {
		t.Errorf("credentials leaked upstream: hdr=%q cookie=%q", s.tokHdr, s.cookie)
	}
	if w := do(h, "GET", "/api/vm/status", nil, map[string]string{"Cookie": "vm_token=s3cret"}); w.Code != 200 {
		t.Fatalf("cookie token: %d", w.Code)
	}
	// auth endpoint
	w = do(h, "GET", "/api/vm/auth", nil, nil)
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"required":true`) || !strings.Contains(w.Body.String(), `"authenticated":false`) {
		t.Errorf("auth GET: %d %s", w.Code, w.Body)
	}
	if w := do(h, "POST", "/api/vm/auth", []byte(`{"token":"nope"}`), okOrigin); w.Code != 401 {
		t.Errorf("auth bad: %d", w.Code)
	}
	if w := do(h, "POST", "/api/vm/auth", []byte(`{"token":"s3cret"}`), nil); w.Code != 403 {
		t.Errorf("auth no origin: %d", w.Code)
	}
	w = do(h, "POST", "/api/vm/auth", []byte(`{"token":"s3cret"}`), okOrigin)
	c := w.Header().Get("Set-Cookie")
	if w.Code != 200 || !strings.Contains(c, "vm_token=s3cret") || !strings.Contains(c, "HttpOnly") || !strings.Contains(c, "SameSite=Strict") {
		t.Errorf("auth ok: %d cookie=%q", w.Code, c)
	}
}

func TestTokenOptional(t *testing.T) {
	h, _ := handler(t, "")
	if w := do(h, "GET", "/api/vm/status", nil, nil); w.Code != 200 {
		t.Fatalf("open: %d", w.Code)
	}
	w := do(h, "GET", "/api/vm/auth", nil, nil)
	if !strings.Contains(w.Body.String(), `"required":false`) || !strings.Contains(w.Body.String(), `"authenticated":true`) {
		t.Errorf("auth GET: %s", w.Body)
	}
}

func TestBodyCap(t *testing.T) {
	h, s := handler(t, "")
	big := make([]byte, vmMaxBody+1)
	if w := do(h, "POST", "/api/vm/search/image", big, okOrigin); w.Code != 413 {
		t.Errorf("oversize: %d", w.Code)
	}
	if s.n != 0 {
		t.Error("oversize reached upstream")
	}
	// Unknown Content-Length (chunked): cap enforced while streaming.
	r := httptest.NewRequest("POST", "/api/vm/search/image", io.NopCloser(bytes.NewReader(big)))
	r.ContentLength = -1
	r.Host = "dash.local:20300"
	r.Header.Set("Origin", "http://dash.local:20300")
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 413 && w.Code != 502 {
		t.Errorf("streamed oversize: %d", w.Code)
	}
	ok := make([]byte, vmMaxBody)
	if w := do(h, "POST", "/api/vm/search/image", ok, okOrigin); w.Code != 200 || s.bodyN != vmMaxBody {
		t.Errorf("exact cap: %d body=%d", w.Code, s.bodyN)
	}
}

func TestUpstreamDown(t *testing.T) {
	ts, _ := upstream(t)
	u, _ := url.Parse(ts.URL)
	ts.Close()
	h := newVMHandler(u, "")
	w := do(h, "GET", "/api/vm/status", nil, nil)
	if w.Code != 502 || !strings.Contains(w.Body.String(), `"vm_api_unavailable"`) {
		t.Errorf("502 mapping: %d %s", w.Code, w.Body)
	}
}
