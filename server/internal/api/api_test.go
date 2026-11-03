package api

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/bili"
	"github.com/wilimini/wilimini/server/internal/proxy"
	"github.com/wilimini/wilimini/server/internal/stream"
)

func newServer(t *testing.T) (*Server, *httptest.Server) {
	t.Helper()
	store, _ := authstore.Open(t.TempDir())
	b := bili.New(store)
	s := &Server{
		Settings: Settings{Token: func() string { return "k7m2q9xa" }, Name: func() string { return "Desk" }, MaxStreams: 2},
		Store:    store,
		Proxy:    &proxy.Handler{Bili: b, Store: store},
		Streams:  &stream.Manager{Bili: b},
		FFmpeg:   stream.Info{OK: true, Version: "6.1"},
		Version:  "test",
	}
	srv := httptest.NewServer(s.Handler())
	t.Cleanup(srv.Close)
	return s, srv
}

func call(t *testing.T, req *http.Request) (*http.Response, string) {
	t.Helper()
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp, string(b)
}

func TestHelloNeedsToken(t *testing.T) {
	_, srv := newServer(t)
	resp, body := call(t, mustGet(srv.URL+"/wili/v1/hello"))
	if resp.StatusCode != 401 || !strings.Contains(body, "unauthorized") {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
	for _, req := range []*http.Request{mustGet(srv.URL + "/wili/v1/hello?k=k7m2q9xa"), withHeader(mustGet(srv.URL+"/wili/v1/hello"), TokenHeader, "k7m2q9xa")} {
		resp, body = call(t, req)
		var h helloReply
		if err := json.Unmarshal([]byte(body), &h); err != nil || resp.StatusCode != 200 {
			t.Fatalf("%d %s %v", resp.StatusCode, body, err)
		}
		if h.Proto != Proto || h.Name != "Desk" || h.Login.State != "signed_out" || !h.FFmpeg.OK || h.MaxStreams != 2 {
			t.Fatalf("%+v", h)
		}
	}
}

func TestHelloShowsAccount(t *testing.T) {
	s, srv := newServer(t)
	s.Store.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "x"}})
	s.Store.SetIdentity(42, "tester")
	_, body := call(t, mustGet(srv.URL+"/wili/v1/hello?k=k7m2q9xa"))
	if !strings.Contains(body, `"signed_in"`) || !strings.Contains(body, `"tester"`) || strings.Contains(body, "SESSDATA") {
		t.Fatal(body)
	}
}

func TestEveryRouteIsBehindTheToken(t *testing.T) {
	_, srv := newServer(t)
	for _, p := range []string{"/wili/v1/stream?bvid=BV1xx411c7mD&cid=1", "/p/api.bilibili.com/x/a", "/anything", "/wili/v1/hello"} {
		if resp, _ := call(t, mustGet(srv.URL+p)); resp.StatusCode != 401 {
			t.Errorf("%s -> %d without a token", p, resp.StatusCode)
		}
	}
	if resp, body := call(t, mustGet(srv.URL+"/anything?k=k7m2q9xa")); resp.StatusCode != 404 || !strings.Contains(body, "not_found") {
		t.Errorf("unknown route -> %d %s", resp.StatusCode, body)
	}
}

func TestWrongTokensAreRateLimitedPerClient(t *testing.T) {
	s, srv := newServer(t)
	now := time.Unix(1000, 0)
	s.Now = func() time.Time { return now }
	for i := 0; i < failLimit; i++ {
		if resp, _ := call(t, mustGet(srv.URL+"/wili/v1/hello?k=wrong")); resp.StatusCode != 401 {
			t.Fatalf("attempt %d -> %d", i, resp.StatusCode)
		}
	}
	resp, body := call(t, mustGet(srv.URL+"/wili/v1/hello?k=k7m2q9xa"))
	if resp.StatusCode != 429 || resp.Header.Get("Retry-After") == "" {
		t.Fatalf("even the right token is refused while blocked: %d %s", resp.StatusCode, body)
	}
	now = now.Add(failWindow + time.Second)
	if resp, _ := call(t, mustGet(srv.URL+"/wili/v1/hello?k=k7m2q9xa")); resp.StatusCode != 200 {
		t.Fatalf("block must expire: %d", resp.StatusCode)
	}
}

func TestLogoutClearsTheAccountInBilibiliEnvelope(t *testing.T) {
	s, srv := newServer(t)
	s.Store.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "x"}})
	if resp, _ := call(t, mustGet(srv.URL+"/wili/v1/logout")); resp.StatusCode != 401 {
		t.Fatal("logout needs the token")
	}
	resp, body := call(t, mustGet(srv.URL+"/wili/v1/logout?k=k7m2q9xa"))
	var env struct {
		Code int `json:"code"`
		Data struct {
			State string `json:"state"`
		} `json:"data"`
	}
	if err := json.Unmarshal([]byte(body), &env); err != nil || resp.StatusCode != 200 || env.Code != 0 || env.Data.State != "signed_out" {
		t.Fatalf("%d %s %v", resp.StatusCode, body, err)
	}
	if s.Store.LoggedIn() {
		t.Fatal("still signed in")
	}
}

func TestTokenChangesTakeEffectImmediately(t *testing.T) {
	token := "oldtoken"
	s, srv := newServer(t)
	s.Settings.Token = func() string { return token }
	if resp, _ := call(t, mustGet(srv.URL+"/wili/v1/hello?k=oldtoken")); resp.StatusCode != 200 {
		t.Fatal("old token should work")
	}
	token = "newtoken"
	if resp, _ := call(t, mustGet(srv.URL+"/wili/v1/hello?k=oldtoken")); resp.StatusCode != 401 {
		t.Fatal("old token must stop working after regeneration")
	}
}

func mustGet(u string) *http.Request {
	r, err := http.NewRequest(http.MethodGet, u, nil)
	if err != nil {
		panic(err)
	}
	return r
}

func withHeader(r *http.Request, k, v string) *http.Request {
	r.Header.Set(k, v)
	return r
}
