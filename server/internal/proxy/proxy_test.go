package proxy

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"

	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/bili"
)

type rtFunc func(*http.Request) (*http.Response, error)

func (f rtFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// fake is a stand-in for every allowed Bilibili host: requests are redirected
// to one httptest server and the intended host is passed along in X-Orig-Host.
type fake struct {
	t       *testing.T
	srv     *httptest.Server
	handler func(w http.ResponseWriter, r *http.Request, host string)
	mu      sync.Mutex
	seen    []*http.Request
}

func newFixture(t *testing.T, readOnly bool, handler func(w http.ResponseWriter, r *http.Request, host string)) (*httptest.Server, *fake, *authstore.Store) {
	t.Helper()
	f := &fake{t: t, handler: handler}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		f.seen = append(f.seen, r)
		f.mu.Unlock()
		f.handler(w, r, r.Header.Get("X-Orig-Host"))
	}))
	t.Cleanup(f.srv.Close)
	store, err := authstore.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	store.Merge([]*http.Cookie{{Name: "SESSDATA", Value: "sess"}, {Name: "bili_jct", Value: "jct"}})
	b := bili.New(store)
	h := &Handler{Bili: b, Store: store, ReadOnly: readOnly}
	h.Transport = rtFunc(func(r *http.Request) (*http.Response, error) {
		r2 := r.Clone(r.Context())
		r2.Header.Set("X-Orig-Host", r.URL.Hostname())
		r2.URL.Scheme, r2.URL.Host = "http", f.srv.Listener.Addr().String()
		return http.DefaultTransport.RoundTrip(r2)
	})
	// WBI endpoints are signed with the nav key, which also comes from the fake.
	b.HTTP = &http.Client{Transport: h.Transport}
	front := httptest.NewServer(h)
	t.Cleanup(front.Close)
	return front, f, store
}

func get(t *testing.T, rawURL string, hdr map[string]string) (*http.Response, string) {
	t.Helper()
	req, _ := http.NewRequestWithContext(context.Background(), http.MethodGet, rawURL, nil)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp, string(b)
}

func TestAllowedHosts(t *testing.T) {
	for host, want := range map[string]bool{
		"api.bilibili.com": true, "i0.hdslb.com": true, "s1.hdslb.com": true,
		"api.bilibili.com.evil.test": false, "evil.test": false, "127.0.0.1": false,
		"hdslb.com": false, ".hdslb.com": false, "a..hdslb.com": false, "x.bilibili.com": false,
		"api.bilibili.com:80": false, "": false, "I0.hdslb.com": false,
	} {
		if got := Allowed(host); got != want {
			t.Errorf("Allowed(%q)=%v want %v", host, got, want)
		}
	}
}

func TestForwardsWithAccountAndWithoutPairingToken(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		http.SetCookie(w, &http.Cookie{Name: "SESSDATA", Value: "rotated"})
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"code":0,"data":{"host":"` + host + `"}}`))
	})
	resp, body := get(t, front.URL+"/p/api.bilibili.com/x/web-interface/view?bvid=BV1&k=secret&aid=2", map[string]string{
		"Cookie": "SESSDATA=from-psp", "Authorization": "Bearer x",
	})
	if resp.StatusCode != 200 || !strings.Contains(body, `"host":"api.bilibili.com"`) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
	if resp.Header.Get("Set-Cookie") != "" {
		t.Fatal("Set-Cookie must not reach the PSP")
	}
	up := f.seen[0]
	if up.URL.Path != "/x/web-interface/view" || up.URL.RawQuery != "bvid=BV1&aid=2" {
		t.Fatalf("upstream %s?%s", up.URL.Path, up.URL.RawQuery)
	}
	if up.Header.Get("Cookie") != "SESSDATA=sess; bili_jct=jct" || up.Header.Get("Authorization") != "" {
		t.Fatalf("cookie/authorization handling wrong: %q / %q", up.Header.Get("Cookie"), up.Header.Get("Authorization"))
	}
	if up.Header.Get("Referer") != bili.DefaultReferer {
		t.Fatalf("referer %q", up.Header.Get("Referer"))
	}
}

func TestRotatedCookiesAreKeptOnServer(t *testing.T) {
	front, _, store := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		http.SetCookie(w, &http.Cookie{Name: "SESSDATA", Value: "rotated"})
		w.Write([]byte(`{"code":0}`))
	})
	get(t, front.URL+"/p/api.bilibili.com/x/a", nil)
	if store.Get("SESSDATA") != "rotated" {
		t.Fatalf("session not updated: %q", store.Get("SESSDATA"))
	}
}

func TestImageHostsGetNoCookies(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		w.Write([]byte("jpeg"))
	})
	_, body := get(t, front.URL+"/p/i0.hdslb.com/bfs/archive/x.jpg@320w_200h.jpg", nil)
	if body != "jpeg" {
		t.Fatal(body)
	}
	if c := f.seen[0].Header.Get("Cookie"); c != "" {
		t.Fatalf("cookies leaked to the CDN: %q", c)
	}
}

func TestRejectsUnknownHostsAndMethods(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {})
	for _, p := range []string{"/p/evil.test/x", "/p/api.bilibili.com.evil.test/x", "/p/127.0.0.1:80/x", "/p/", "/p/api.bilibili.com%2f..%2f@evil.test/x"} {
		resp, body := get(t, front.URL+p, nil)
		if resp.StatusCode != 403 || !strings.Contains(body, "host_not_allowed") {
			t.Errorf("%s -> %d %s", p, resp.StatusCode, body)
		}
	}
	req, _ := http.NewRequest(http.MethodPut, front.URL+"/p/api.bilibili.com/x", nil)
	resp, _ := http.DefaultClient.Do(req)
	if resp.StatusCode != 405 {
		t.Fatalf("PUT -> %d", resp.StatusCode)
	}
	if len(f.seen) != 0 {
		t.Fatal("rejected requests must not reach upstream")
	}
}

func TestPathTraversalIsContained(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) { w.Write([]byte("ok")) })
	get(t, front.URL+"/p/api.bilibili.com/a/../../x/y", nil)
	if f.seen[0].URL.Path != "/x/y" || f.seen[0].Header.Get("X-Orig-Host") != "api.bilibili.com" {
		t.Fatalf("path %q host %q", f.seen[0].URL.Path, f.seen[0].Header.Get("X-Orig-Host"))
	}
}

func TestPostFillsCSRFAndReadOnlyBlocksIt(t *testing.T) {
	var gotBody, gotType string
	handler := func(w http.ResponseWriter, r *http.Request, host string) {
		b, _ := io.ReadAll(r.Body)
		gotBody, gotType = string(b), r.Header.Get("Content-Type")
		w.Write([]byte(`{"code":0}`))
	}
	front, _, _ := newFixture(t, false, handler)
	resp, err := http.Post(front.URL+"/p/api.bilibili.com/x/web-interface/archive/like?k=zz", "application/x-www-form-urlencoded",
		strings.NewReader("bvid=BV1&like=1&csrf=&csrf_token=old"))
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("%v %v", err, resp)
	}
	form, _ := url.ParseQuery(gotBody)
	if form.Get("csrf") != "jct" || form.Get("csrf_token") != "jct" || form.Get("bvid") != "BV1" || !strings.HasPrefix(gotType, "application/x-www-form-urlencoded") {
		t.Fatalf("body %q type %q", gotBody, gotType)
	}

	ro, _, _ := newFixture(t, true, handler)
	resp, _ = http.Post(ro.URL+"/p/api.bilibili.com/x/web-interface/archive/like", "application/x-www-form-urlencoded", strings.NewReader("a=1"))
	if resp.StatusCode != 403 {
		t.Fatalf("read-only POST -> %d", resp.StatusCode)
	}
	if resp, _ := get(t, ro.URL+"/p/api.bilibili.com/x/ok", nil); resp.StatusCode != 200 {
		t.Fatalf("read-only GET -> %d", resp.StatusCode)
	}
}

func TestWBISigningAndRetry(t *testing.T) {
	var attempts int
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		switch r.URL.Path {
		case "/x/web-interface/nav":
			b, _ := json.Marshal(map[string]any{"code": -101, "data": map[string]any{"wbi_img": map[string]string{
				"img_url": "https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png",
				"sub_url": "https://i0.hdslb.com/bfs/wbi/4932caff0ff746eab6f01bf08b70ac45.png"}}})
			w.Write(b)
		case "/x/frontend/finger/spi":
			w.Write([]byte(`{"code":0,"data":{"b_3":"B3","b_4":"B4"}}`))
		case "/x/player/wbi/v2":
			attempts++
			if attempts == 1 {
				w.Write([]byte(`{"code":-403,"message":"signature"}`))
				return
			}
			q := r.URL.Query()
			if q.Get("w_rid") == "" || q.Get("wts") == "" || q.Get("bvid") != "BV1" || q.Has("k") {
				t.Errorf("bad signed query: %s", r.URL.RawQuery)
			}
			w.Write([]byte(`{"code":0,"data":{"ok":1}}`))
		default:
			t.Errorf("unexpected upstream %s", r.URL)
		}
	})
	resp, body := get(t, front.URL+"/p/api.bilibili.com/x/player/wbi/v2?bvid=BV1&cid=2&k=tok", nil)
	if resp.StatusCode != 200 || !strings.Contains(body, `"ok":1`) || attempts != 2 {
		t.Fatalf("%d %s attempts=%d seen=%d", resp.StatusCode, body, attempts, len(f.seen))
	}
}

func TestAlreadySignedRequestsAreNotResigned(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) { w.Write([]byte(`{"code":0}`)) })
	get(t, front.URL+"/p/api.bilibili.com/x/player/wbi/v2?bvid=B&w_rid=abc&wts=1", nil)
	if len(f.seen) != 1 || f.seen[0].URL.RawQuery != "bvid=B&w_rid=abc&wts=1" {
		t.Fatalf("seen %d %v", len(f.seen), f.seen[0].URL)
	}
}

func TestRedirectOutsideAllowlistIsRefused(t *testing.T) {
	front, _, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		http.Redirect(w, r, "https://evil.test/steal", http.StatusFound)
	})
	resp, body := get(t, front.URL+"/p/api.bilibili.com/x/r", nil)
	if resp.StatusCode != http.StatusBadGateway || !strings.Contains(body, "not allowed") {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
}

func TestEncodingIsForwardedUntouched(t *testing.T) {
	front, f, _ := newFixture(t, false, func(w http.ResponseWriter, r *http.Request, host string) {
		w.Header().Set("Content-Encoding", "deflate")
		w.Write([]byte("raw-deflate-bytes"))
	})
	req, _ := http.NewRequest(http.MethodGet, front.URL+"/p/api.bilibili.com/x/v1/dm/list.so?oid=1", nil)
	req.Header.Set("Accept-Encoding", "deflate")
	tr := &http.Transport{DisableCompression: true}
	resp, err := tr.RoundTrip(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.Header.Get("Content-Encoding") != "deflate" || f.seen[0].Header.Get("Accept-Encoding") != "deflate" {
		t.Fatalf("encoding changed: %q / %q", resp.Header.Get("Content-Encoding"), f.seen[0].Header.Get("Accept-Encoding"))
	}
}
