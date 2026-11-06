package stream

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/bili"
)

// fixture serves a generated "Bilibili": nav, playurl and the media files. The
// source is deliberately hostile to a PSP: 640x360, High profile, CABAC, B-frames.
type fixture struct {
	t      *testing.T
	ffmpeg string
	mgr    *Manager
	front  *httptest.Server
	events []string
	mu     sync.Mutex
	hits   map[string]int
}

func ffmpegOrSkip(t *testing.T) string {
	t.Helper()
	info := Probe(context.Background(), "")
	if !info.OK {
		t.Skipf("ffmpeg unusable here: %s", info.Problem)
	}
	return info.Path
}

func makeSources(t *testing.T, ffmpeg string, seconds int) (video, audio string) {
	t.Helper()
	dir := t.TempDir()
	video, audio = filepath.Join(dir, "v.mp4"), filepath.Join(dir, "a.m4a")
	run := func(args ...string) {
		out, err := exec.Command(ffmpeg, append([]string{"-hide_banner", "-loglevel", "error", "-y"}, args...)...).CombinedOutput()
		if err != nil {
			t.Fatalf("ffmpeg %v: %v\n%s", args, err, out)
		}
	}
	d := fmt.Sprint(seconds)
	run("-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration="+d, "-c:v", "libx264", "-profile:v", "high",
		"-bf", "2", "-pix_fmt", "yuv420p", "-movflags", "+faststart", video)
	run("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration="+d, "-c:a", "aac", "-b:a", "128k", audio)
	return video, audio
}

func newFixture(t *testing.T, max int, seconds int) *fixture {
	t.Helper()
	ff := ffmpegOrSkip(t)
	video, audio := makeSources(t, ff, seconds)
	f := &fixture{t: t, ffmpeg: ff, hits: map[string]int{}}
	mux := http.NewServeMux()
	var base string
	mux.HandleFunc("/media/v.mp4", func(w http.ResponseWriter, r *http.Request) { http.ServeFile(w, r, video) })
	mux.HandleFunc("/media/a.m4a", func(w http.ResponseWriter, r *http.Request) { http.ServeFile(w, r, audio) })
	mux.HandleFunc("/x/web-interface/nav", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":-101,"data":{"wbi_img":{"img_url":"https://i/7cd084941338484aae1ad9425b84077c.png","sub_url":"https://i/4932caff0ff746eab6f01bf08b70ac45.png"}}}`)
	})
	mux.HandleFunc("/x/frontend/finger/spi", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{"b_3":"B3","b_4":"B4"}}`)
	})
	mux.HandleFunc("/x/player/wbi/playurl", func(w http.ResponseWriter, r *http.Request) {
		f.mu.Lock()
		f.hits[r.URL.Query().Get("bvid")]++
		f.mu.Unlock()
		switch r.URL.Query().Get("bvid") {
		case "BV1LOGIN0000":
			fmt.Fprint(w, `{"code":-101,"message":"账号未登录"}`)
		case "BV1ONLYAV100":
			fmt.Fprint(w, `{"code":0,"data":{"dash":{"video":[{"id":32,"codecid":13,"base_url":"x"}],"audio":[{"id":30280,"base_url":"y"}]}}}`)
		case "BV1MULTITIER":
			// Several tiers, as a real video has; every one points at the same small test media.
			fmt.Fprintf(w, `{"code":0,"data":{"dash":{"video":[
				{"id":16,"codecid":7,"base_url":"%[1]s/media/v.mp4"},{"id":32,"codecid":7,"base_url":"%[1]s/media/v.mp4"},
				{"id":64,"codecid":7,"base_url":"%[1]s/media/v.mp4"},{"id":80,"codecid":7,"base_url":"%[1]s/media/v.mp4"},
				{"id":116,"codecid":12,"base_url":"%[1]s/media/v.mp4"}],
				"audio":[{"id":30280,"base_url":"%[1]s/media/a.m4a"}]}}}`, base)
		case "BV1BADCDN000":
			// The first CDN URL fails, the backup works.
			fmt.Fprintf(w, `{"code":0,"data":{"dash":{"video":[{"id":32,"codecid":7,"base_url":"%[1]s/media/missing.mp4","backup_url":["%[1]s/media/v.mp4"]}],"audio":[{"id":30280,"base_url":"%[1]s/media/missing.m4a","backup_url":["%[1]s/media/a.m4a"]}]}}}`, base)
		default:
			fmt.Fprintf(w, `{"code":0,"data":{"dash":{"video":[{"id":32,"codecid":7,"base_url":"%[1]s/media/v.mp4"}],"audio":[{"id":30280,"base_url":"%[1]s/media/a.m4a"}]}}}`, base)
		}
	})
	up := httptest.NewServer(mux)
	t.Cleanup(up.Close)
	base = up.URL

	store, _ := authstore.Open(t.TempDir())
	b := bili.New(store)
	b.APIBase, b.PassportBase = up.URL, up.URL
	f.mgr = &Manager{Bili: b, FFmpeg: ff, Max: max, OnEvent: func(kind, msg string) {
		f.mu.Lock()
		f.events = append(f.events, kind+": "+msg)
		f.mu.Unlock()
	}}
	f.front = httptest.NewServer(f.mgr)
	t.Cleanup(f.front.Close)
	return f
}

func fetch(t *testing.T, url string) (*http.Response, []byte) {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp, b
}

type probe struct {
	Streams []struct {
		CodecType  string `json:"codec_type"`
		CodecName  string `json:"codec_name"`
		Profile    string `json:"profile"`
		Width      int    `json:"width"`
		Height     int    `json:"height"`
		SampleRate string `json:"sample_rate"`
		Channels   int    `json:"channels"`
		RFrameRate string `json:"r_frame_rate"`
		Level      int    `json:"level"`
	} `json:"streams"`
	Format struct {
		Name     string `json:"format_name"`
		Duration string `json:"duration"`
	} `json:"format"`
}

func ffprobe(t *testing.T, data []byte) probe {
	t.Helper()
	path := filepath.Join(t.TempDir(), "out.flv")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	probeBin, err := exec.LookPath("ffprobe")
	if err != nil {
		t.Skip("ffprobe not installed")
	}
	out, err := exec.Command(probeBin, "-v", "error", "-show_streams", "-show_format", "-of", "json", path).Output()
	if err != nil {
		t.Fatalf("ffprobe: %v", err)
	}
	var p probe
	if err := json.Unmarshal(out, &p); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestStreamProducesPSPFriendlyFLV(t *testing.T) {
	f := newFixture(t, 2, 4)
	for _, tc := range []struct{ profile, want string }{{"cavlc", "Main"}, {"cabac", "Main"}} {
		resp, body := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9&profile="+tc.profile)
		if resp.StatusCode != 200 || resp.Header.Get("Content-Type") != "video/x-flv" {
			t.Fatalf("%s: %d %s %.200s", tc.profile, resp.StatusCode, resp.Header.Get("Content-Type"), body)
		}
		p := ffprobe(t, body)
		var haveV, haveA bool
		for _, s := range p.Streams {
			switch s.CodecType {
			case "video":
				haveV = true
				if s.CodecName != "h264" || !strings.Contains(s.Profile, tc.want) {
					t.Errorf("%s: video %s profile %q", tc.profile, s.CodecName, s.Profile)
				}
				if s.Width != 480 || s.Height != 272 || s.Width%16 != 0 || s.Height%16 != 0 {
					t.Errorf("%s: size %dx%d (16:9 padded to whole macroblocks)", tc.profile, s.Width, s.Height)
				}
				if s.Level != 30 {
					t.Errorf("%s: level %d", tc.profile, s.Level)
				}
			case "audio":
				haveA = true
				if s.CodecName != "aac" || s.SampleRate != "44100" || s.Channels != 2 {
					t.Errorf("%s: audio %+v", tc.profile, s)
				}
			}
		}
		if !haveV || !haveA || p.Format.Name != "flv" {
			t.Fatalf("%s: incomplete output %+v", tc.profile, p)
		}
	}
}

func TestStreamSeekStartsLaterAndNeverEmitsBFrames(t *testing.T) {
	f := newFixture(t, 2, 8)
	_, full := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9")
	_, tail := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9&t=4")
	dFull, dTail := durationOf(t, ffprobe(t, full)), durationOf(t, ffprobe(t, tail))
	if dFull < 7 || dFull > 9 {
		t.Fatalf("full duration %.2f", dFull)
	}
	if dTail < 3 || dTail > 5 {
		t.Fatalf("t=4 should leave about 4 s of an 8 s source, got %.2f", dTail)
	}
	// Timestamps restart near zero so the PSP can add the seek offset itself,
	// and the stream carries no B-frames (the PSP decoder is cheaper without).
	if first := firstVideoPTS(t, tail); first > 0.5 {
		t.Fatalf("first video pts %.2f s; a restarted stream must begin near 0", first)
	}
	if kinds := pictureTypes(t, tail); strings.Contains(kinds, "B") || !strings.HasPrefix(kinds, "I") {
		t.Fatalf("picture types %q", kinds)
	}
}

func ffprobeOut(t *testing.T, data []byte, args ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "x.flv")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	bin, err := exec.LookPath("ffprobe")
	if err != nil {
		t.Skip("ffprobe not installed")
	}
	out, err := exec.Command(bin, append(append([]string{"-v", "error"}, args...), path)...).Output()
	if err != nil {
		t.Fatalf("ffprobe: %v", err)
	}
	return string(out)
}

func firstVideoPTS(t *testing.T, data []byte) float64 {
	t.Helper()
	out := ffprobeOut(t, data, "-select_streams", "v:0", "-show_entries", "packet=pts_time", "-of", "csv=p=0", "-read_intervals", "%+#1")
	var v float64
	fmt.Sscan(strings.TrimSpace(out), &v)
	return v
}

func pictureTypes(t *testing.T, data []byte) string {
	t.Helper()
	out := ffprobeOut(t, data, "-select_streams", "v:0", "-show_entries", "frame=pict_type", "-of", "csv=p=0")
	return strings.ReplaceAll(strings.TrimSpace(out), "\n", "")
}

func durationOf(t *testing.T, p probe) float64 {
	t.Helper()
	var d float64
	fmt.Sscan(p.Format.Duration, &d)
	return d
}

func TestStreamErrors(t *testing.T) {
	f := newFixture(t, 2, 2)
	for _, tc := range []struct {
		query  string
		status int
		code   string
	}{
		{"bvid=BV1LOGIN0000&cid=1", http.StatusForbidden, "login_required"},
		{"bvid=BV1ONLYAV100&cid=1", http.StatusNotFound, "no_stream"},
		{"bvid=nope&cid=1", http.StatusBadRequest, "bad_request"},
	} {
		resp, body := fetch(t, f.front.URL+"/?"+tc.query)
		var e map[string]string
		_ = json.Unmarshal(body, &e)
		if resp.StatusCode != tc.status || e["error"] != tc.code {
			t.Errorf("%s -> %d %s", tc.query, resp.StatusCode, body)
		}
	}
}

func TestStreamFallsBackToBackupCDN(t *testing.T) {
	f := newFixture(t, 2, 2)
	resp, body := fetch(t, f.front.URL+"/?bvid=BV1BADCDN000&cid=1")
	if resp.StatusCode != 200 || len(body) < 1000 {
		t.Fatalf("%d %.200s", resp.StatusCode, body)
	}
	if p := ffprobe(t, body); len(p.Streams) != 2 {
		t.Fatalf("%+v", p)
	}
}

func TestFFmpegMissingIsReported(t *testing.T) {
	m := &Manager{FFmpeg: ""}
	srv := httptest.NewServer(m)
	defer srv.Close()
	resp, body := fetch(t, srv.URL+"/?bvid=BV1xx411c7mD&cid=1")
	if resp.StatusCode != http.StatusServiceUnavailable || !bytes.Contains(body, []byte("ffmpeg_missing")) {
		t.Fatalf("%d %s", resp.StatusCode, body)
	}
}

func TestDisconnectKillsFFmpegAndFreesSlot(t *testing.T) {
	f := newFixture(t, 1, 30)
	f.mgr.ClientKey = func(r *http.Request) string { return r.URL.Query().Get("who") }
	ctx, cancel := context.WithCancel(context.Background())
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9&who=a", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := io.ReadFull(resp.Body, make([]byte, 4096)); err != nil {
		t.Fatal(err)
	}
	if n := len(f.mgr.Snapshot()); n != 1 {
		t.Fatalf("active %d", n)
	}
	// A second client is refused while the single slot is taken.
	if r2, body := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9&who=b"); r2.StatusCode != 503 || !bytes.Contains(body, []byte("busy")) {
		t.Fatalf("expected busy, got %d %.100s", r2.StatusCode, body)
	}
	cancel()
	resp.Body.Close()
	deadline := time.Now().Add(5 * time.Second)
	for len(f.mgr.Snapshot()) != 0 {
		if time.Now().After(deadline) {
			t.Fatal("session not released after disconnect")
		}
		time.Sleep(50 * time.Millisecond)
	}
	if r3, _ := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9&who=b&t=28"); r3.StatusCode != 200 {
		t.Fatalf("slot not reusable: %d", r3.StatusCode)
	}
}

func TestNewStreamFromSameClientReplacesTheOldOne(t *testing.T) {
	f := newFixture(t, 1, 30)
	first, err := http.Get(f.front.URL + "/?bvid=BV1xx411c7mD&cid=9")
	if err != nil {
		t.Fatal(err)
	}
	defer first.Body.Close()
	io.ReadFull(first.Body, make([]byte, 2048))
	second, err := http.Get(f.front.URL + "/?bvid=BV1xx411c7mD&cid=9&t=20")
	if err != nil || second.StatusCode != 200 {
		t.Fatalf("seek from the same client must not be refused: %v", err)
	}
	second.Body.Close()
}

func TestProbeReportsMissingBinary(t *testing.T) {
	info := Probe(context.Background(), "/nonexistent/ffmpeg-nope")
	if info.OK || !strings.Contains(info.Problem, "not found") {
		t.Fatalf("%+v", info)
	}
}

// The PSP builds its quality menu from these two response headers, so check them on the real HTTP
// response of a real transcode, not only on the helper that sets them.
func TestStreamResponseTellsThePSPWhichTiersTheVideoHas(t *testing.T) {
	f := newFixture(t, 2, 3)
	for _, tc := range []struct{ query, wantQuality string }{
		{"qn=64", "64"},   // asked for 720P: streamed as 720P
		{"qn=120", "116"}, // asked for 4K: falls back to the best tier below it that exists
		{"qn=16", "16"},
	} {
		resp, _ := fetch(t, f.front.URL+"/?bvid=BV1MULTITIER&cid=9&"+tc.query)
		if resp.StatusCode != 200 {
			t.Fatalf("%s: status %d", tc.query, resp.StatusCode)
		}
		if got := resp.Header.Get("X-Wili-Quality"); got != tc.wantQuality {
			t.Errorf("%s: X-Wili-Quality %q, want %s", tc.query, got, tc.wantQuality)
		}
		// The list is the same whichever tier was asked for: it is what the video offers.
		if got := resp.Header.Get("X-Wili-Qualities"); got != "16,32,64,80,116" {
			t.Errorf("%s: X-Wili-Qualities %q", tc.query, got)
		}
	}
	// A video with a single tier lists just that one.
	resp, _ := fetch(t, f.front.URL+"/?bvid=BV1xx411c7mD&cid=9")
	if resp.Header.Get("X-Wili-Qualities") != "32" || resp.Header.Get("X-Wili-Quality") != "32" {
		t.Errorf("single tier: %v", resp.Header)
	}
}
