package bili

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"slices"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/wilimini/wilimini/server/internal/authstore"
)

const (
	imgKey = "7cd084941338484aae1ad9425b84077c"
	subKey = "4932caff0ff746eab6f01bf08b70ac45"
)

func navBody(login bool) string {
	code := 0
	if !login {
		code = -101
	}
	b, _ := json.Marshal(map[string]any{"code": code, "message": "x", "data": map[string]any{
		"isLogin": login, "mid": 42, "uname": "tester",
		"wbi_img": map[string]string{
			"img_url": "https://i0.hdslb.com/bfs/wbi/" + imgKey + ".png",
			"sub_url": "https://i0.hdslb.com/bfs/wbi/" + subKey + ".png",
		}}})
	return string(b)
}

func newClient(t *testing.T, h http.Handler) (*Client, *authstore.Store) {
	t.Helper()
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	store, err := authstore.Open(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	c := New(store)
	c.APIBase, c.PassportBase = srv.URL, srv.URL
	return c, store
}

func TestNavLoggedOutStillYieldsSigningKey(t *testing.T) {
	var hits int32
	c, _ := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		w.Write([]byte(navBody(false)))
	}))
	m, err := c.Mixin(context.Background(), false)
	if err != nil {
		t.Fatal(err)
	}
	if m != MixinKey(imgKey, subKey) {
		t.Fatalf("mixin %q", m)
	}
	if _, err := c.Mixin(context.Background(), false); err != nil || atomic.LoadInt32(&hits) != 1 {
		t.Fatalf("mixin must be cached (hits=%d, err=%v)", hits, err)
	}
}

func TestNavRecordsIdentityWhenLoggedIn(t *testing.T) {
	c, store := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(navBody(true)))
	}))
	info, err := c.Nav(context.Background())
	if err != nil || !info.IsLogin {
		t.Fatalf("%v %+v", err, info)
	}
	if mid, name := store.Identity(); mid != 42 || name != "tester" {
		t.Fatalf("identity %d %q", mid, name)
	}
}

func TestSignedGetRetriesOnceAfterBadSignature(t *testing.T) {
	var calls, navs int32
	c, _ := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/x/web-interface/nav":
			atomic.AddInt32(&navs, 1)
			w.Write([]byte(navBody(false)))
		case "/x/frontend/finger/spi":
			w.Write([]byte(`{"code":0,"data":{"b_3":"B3","b_4":"B4"}}`))
		case "/signed":
			if atomic.AddInt32(&calls, 1) == 1 {
				w.Write([]byte(`{"code":-403,"message":"bad sign"}`))
				return
			}
			q := r.URL.Query()
			if q.Get("w_rid") == "" || q.Get("wts") == "" {
				t.Errorf("missing signature: %s", r.URL.RawQuery)
			}
			if !strings.Contains(r.Header.Get("Cookie"), "buvid3=B3") {
				t.Errorf("buvid cookie not sent: %q", r.Header.Get("Cookie"))
			}
			w.Write([]byte(`{"code":0,"data":{"ok":true}}`))
		}
	}))
	data, err := c.SignedGet(context.Background(), c.APIBase+"/signed", url.Values{"a": {"1"}})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "true") || atomic.LoadInt32(&calls) != 2 || atomic.LoadInt32(&navs) != 2 {
		t.Fatalf("calls=%d navs=%d data=%s", calls, navs, data)
	}
}

func TestQRLoginFlow(t *testing.T) {
	step := int32(0)
	c, store := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/x/passport-login/web/qrcode/generate":
			w.Write([]byte(`{"code":0,"data":{"url":"https://passport.bilibili.com/h5-app/passport/login/scan?qrcode_key=K","qrcode_key":"K"}}`))
		case "/x/passport-login/web/qrcode/poll":
			if r.URL.Query().Get("qrcode_key") != "K" {
				t.Errorf("wrong key")
			}
			switch atomic.AddInt32(&step, 1) {
			case 1:
				w.Write([]byte(`{"code":0,"data":{"code":86101}}`))
			case 2:
				w.Write([]byte(`{"code":0,"data":{"code":86090}}`))
			default:
				http.SetCookie(w, &http.Cookie{Name: "SESSDATA", Value: "secret"})
				http.SetCookie(w, &http.Cookie{Name: "bili_jct", Value: "jct"})
				w.Write([]byte(`{"code":0,"data":{"code":0}}`))
			}
		case "/x/web-interface/nav":
			w.Write([]byte(navBody(true)))
		}
	}))
	key, content, err := c.QRGenerate(context.Background())
	if err != nil || key != "K" || !strings.HasPrefix(content, "https://") {
		t.Fatalf("%v %q %q", err, key, content)
	}
	want := []QRState{QRWaitingScan, QRWaitingConfirm, QRDone}
	for i, w := range want {
		got, err := c.QRPoll(context.Background(), key)
		if err != nil || got != w {
			t.Fatalf("poll %d: got %v err %v, want %v", i, got, err, w)
		}
	}
	if !store.LoggedIn() || store.CSRF() != "jct" {
		t.Fatal("cookies not stored")
	}
	if _, name := store.Identity(); name != "tester" {
		t.Fatalf("name %q", name)
	}
}

func TestQRExpiredAndUnknownCode(t *testing.T) {
	code := 86038
	c, _ := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := json.Marshal(map[string]any{"code": 0, "data": map[string]any{"code": code, "message": "m"}})
		w.Write(b)
	}))
	if s, err := c.QRPoll(context.Background(), "k"); err != nil || s != QRExpired {
		t.Fatalf("%v %v", s, err)
	}
	code = 12345
	if _, err := c.QRPoll(context.Background(), "k"); err == nil {
		t.Fatal("unknown codes must be errors")
	}
}

func TestSelectMediaPrefersAVCNearQuality(t *testing.T) {
	data := json.RawMessage(`{"dash":{
	  "video":[
	    {"id":16,"codecid":7,"base_url":"v16","bandwidth":300,"width":640,"height":360},
	    {"id":32,"codecid":12,"base_url":"hevc32","bandwidth":200},
	    {"id":32,"codecid":7,"base_url":"v32hi","bandwidth":900,"width":852,"height":480},
	    {"id":32,"codecid":7,"baseUrl":"v32lo","bandwidth":500,"backupUrl":["b1"],"width":852,"height":480},
	    {"id":64,"codecid":7,"base_url":"v64","bandwidth":1500}],
	  "audio":[{"id":30216,"base_url":"a216"},{"id":30280,"base_url":"a280","backup_url":["a280b"]}]}}`)
	m, err := SelectMedia(data, QN480)
	if err != nil {
		t.Fatal(err)
	}
	if m.Video[0] != "v32lo" || m.Video[1] != "b1" || m.Audio[0] != "a280" || m.Audio[1] != "a280b" || m.QN != 32 {
		t.Fatalf("%+v", m)
	}
	// Asking for less than anything available falls back to the smallest rendition above it.
	m, err = SelectMedia(json.RawMessage(`{"dash":{"video":[{"id":64,"codecid":7,"base_url":"v64"},{"id":80,"codecid":7,"base_url":"v80"}],"audio":[{"id":30216,"base_url":"a"}]}}`), QN360)
	if err != nil || m.Video[0] != "v64" {
		t.Fatalf("%v %+v", err, m)
	}
}

func TestSelectMediaHighTiersAndFrameRate(t *testing.T) {
	data := json.RawMessage(`{"dash":{
	  "video":[
	    {"id":64,"codecid":7,"base_url":"v64","frame_rate":"30.000"},
	    {"id":80,"codecid":7,"base_url":"v80","frame_rate":"30.000"},
	    {"id":116,"codecid":12,"base_url":"hevc116","bandwidth":3000,"frame_rate":"60.000","width":1920,"height":1080},
	    {"id":116,"codecid":7,"base_url":"avc116","bandwidth":9000,"frame_rate":"60.000","width":1920,"height":1080},
	    {"id":120,"codecid":12,"base_url":"hevc4k","frame_rate":"59.940","width":3840,"height":2160},
	    {"id":120,"codecid":13,"base_url":"av14k"}],
	  "audio":[{"id":30280,"base_url":"a"}]}}`)
	// 1080P60 is offered as AVC and HEVC: AVC wins at the same tier even though it needs more bandwidth.
	m, err := SelectMedia(data, QN1080P60)
	if err != nil || m.Video[0] != "avc116" || m.QN != 116 || m.FPS != 60 || m.Height != 1080 {
		t.Fatalf("%v %+v", err, m)
	}
	// 4K is only offered as HEVC (and AV1, which is skipped).
	m, err = SelectMedia(data, QN4K)
	if err != nil || m.Video[0] != "hevc4k" || m.QN != 120 || m.FPS != 59.94 {
		t.Fatalf("%v %+v", err, m)
	}
	// A tier the account cannot get falls back to the best one below it.
	m, err = SelectMedia(json.RawMessage(`{"dash":{"video":[{"id":64,"codecid":7,"base_url":"v64"},{"id":80,"codecid":7,"base_url":"v80"}],"audio":[{"id":30280,"base_url":"a"}]}}`), QN4K)
	if err != nil || m.Video[0] != "v80" || m.FPS != 0 {
		t.Fatalf("%v %+v", err, m)
	}
}

func TestPlayURLAsks4KOnlyForTheTopTier(t *testing.T) {
	var gotFnval, gotFourk []string
	c, _ := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/x/web-interface/nav":
			w.Write([]byte(navBody(false)))
		case "/x/frontend/finger/spi":
			w.Write([]byte(`{"code":0,"data":{"b_3":"B3","b_4":"B4"}}`))
		case "/x/player/wbi/playurl":
			q := r.URL.Query()
			gotFnval, gotFourk = append(gotFnval, q.Get("fnval")), append(gotFourk, q.Get("fourk"))
			w.Write([]byte(`{"code":0,"data":{"dash":{"video":[{"id":32,"codecid":7,"base_url":"V"}],"audio":[{"id":30280,"base_url":"A"}]}}}`))
		}
	}))
	for _, qn := range []int{QN1080P60, QN4K} {
		if _, err := c.PlayURL(context.Background(), PlayRequest{BVID: "BV1xx", CID: 7, QN: qn}); err != nil {
			t.Fatal(err)
		}
	}
	if gotFnval[0] != "16" || gotFourk[0] != "0" || gotFnval[1] != "144" || gotFourk[1] != "1" {
		t.Fatalf("fnval=%v fourk=%v", gotFnval, gotFourk)
	}
}

func TestSelectMediaFallsBackToDurlAndPGC(t *testing.T) {
	m, err := SelectMedia(json.RawMessage(`{"durl":[{"url":"muxed","backup_url":["m2"]}]}`), QN480)
	if err != nil || m.Muxed[0] != "muxed" || m.Muxed[1] != "m2" {
		t.Fatalf("%v %+v", err, m)
	}
	m, err = SelectMedia(json.RawMessage(`{"video_info":{"dash":{"video":[{"id":32,"codecid":7,"base_url":"pv"}],"audio":[{"id":30280,"base_url":"pa"}]}}}`), QN480)
	if err != nil || m.Video[0] != "pv" || m.Audio[0] != "pa" {
		t.Fatalf("%v %+v", err, m)
	}
	// AV1 needs an optional ffmpeg decoder, so a video offered only as AV1 is unusable.
	if _, err := SelectMedia(json.RawMessage(`{"dash":{"video":[{"id":32,"codecid":13,"base_url":"av1"}],"audio":[]}}`), QN480); err != ErrNoStream {
		t.Fatalf("av1-only must be ErrNoStream, got %v", err)
	}
}

func TestPlayURLUGCSignsAndParses(t *testing.T) {
	c, _ := newClient(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/x/web-interface/nav":
			w.Write([]byte(navBody(false)))
		case "/x/frontend/finger/spi":
			w.Write([]byte(`{"code":0,"data":{"b_3":"B3","b_4":"B4"}}`))
		case "/x/player/wbi/playurl":
			q := r.URL.Query()
			if q.Get("bvid") != "BV1xx" || q.Get("cid") != "7" || q.Get("fnval") != "16" || q.Get("w_rid") == "" {
				t.Errorf("bad query %s", r.URL.RawQuery)
			}
			w.Write([]byte(`{"code":0,"data":{"dash":{"video":[{"id":32,"codecid":7,"base_url":"V"}],"audio":[{"id":30280,"base_url":"A"}]}}}`))
		default:
			t.Errorf("unexpected %s", r.URL)
		}
	}))
	m, err := c.PlayURL(context.Background(), PlayRequest{BVID: "BV1xx", CID: 7})
	if err != nil || m.Video[0] != "V" || m.Audio[0] != "A" {
		t.Fatalf("%v %+v", err, m)
	}
}

func TestSelectMediaListsOnlyTheTiersThisVideoOffers(t *testing.T) {
	// Videos differ: this one has no 720P60 and its 1080P60 is HEVC-only; AV1-only and
	// URL-less renditions are not playable and must not appear in the PSP's menu.
	data := json.RawMessage(`{"dash":{
	  "video":[
	    {"id":80,"codecid":7,"base_url":"v80"},
	    {"id":32,"codecid":7,"base_url":"v32"},
	    {"id":32,"codecid":12,"base_url":"h32"},
	    {"id":116,"codecid":12,"base_url":"h116"},
	    {"id":120,"codecid":13,"base_url":"av1"},
	    {"id":64,"codecid":7}],
	  "audio":[{"id":30280,"base_url":"a"}]}}`)
	m, err := SelectMedia(data, QN1080)
	if err != nil {
		t.Fatal(err)
	}
	if want := []int{32, 80, 116}; !slices.Equal(m.Qualities, want) || m.QN != 80 {
		t.Fatalf("qualities %v (want %v) qn %d", m.Qualities, want, m.QN)
	}
}
