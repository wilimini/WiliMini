package stream

import (
	"net/http"
	"net/url"
	"slices"
	"strings"
	"testing"

	"github.com/wilimini/wilimini/server/internal/bili"
)

func mustParse(t *testing.T, raw string) Request {
	t.Helper()
	q, _ := url.ParseQuery(raw)
	r, err := ParseRequest(q)
	if err != nil {
		t.Fatalf("%s: %v", raw, err)
	}
	return r
}

func TestParseDefaultsAndClamping(t *testing.T) {
	r := mustParse(t, "bvid=BV1xx411c7mD&cid=12")
	if r.FPS != 30 || r.VideoKbps != 1200 || r.QN != 32 || r.EncoderProfile() != ProfileCABAC {
		t.Fatalf("%+v", r)
	}
	// Out-of-range values are clamped: fps to 60, qn to 4K.
	r = mustParse(t, "bvid=BV1xx411c7mD&cid=12&fps=120&vb=99999&ab=1&qn=500&t=61.5&profile=CABAC")
	if r.FPS != 60 || r.VideoKbps != 4000 || r.AudioKbps != 32 || r.QN != bili.QN4K || r.StartSec != 61.5 || r.EncoderProfile() != ProfileCABAC {
		t.Fatalf("%+v", r)
	}
	// 60 fps and every tier up to 4K pass through unchanged.
	r = mustParse(t, "bvid=BV1xx411c7mD&cid=12&fps=60&qn=116")
	if r.FPS != 60 || r.QN != bili.QN1080P60 {
		t.Fatalf("%+v", r)
	}
	// The output size is fixed: w and h are accepted for old clients and ignored.
	if _, err := ParseRequest(url.Values{"bvid": {"BV1xx411c7mD"}, "cid": {"1"}, "w": {"720"}, "h": {"wide"}}); err != nil {
		t.Fatalf("w/h must be ignored: %v", err)
	}
	if r = mustParse(t, "bvid=BV1xx411c7mD&cid=1&t=99999999"); r.StartSec != MaxSeekSeconds {
		t.Fatalf("t=%v", r.StartSec)
	}
}

func TestParseRejectsBadInput(t *testing.T) {
	for _, raw := range []string{
		"cid=1", "bvid=BV1xx411c7mD", "bvid=BV1xx411c7mD&cid=0", "bvid=BV1xx411c7mD&cid=x",
		"bvid=../etc&cid=1", "bvid=BV1xx411c7mD&cid=1&t=-3", "bvid=BV1xx411c7mD&cid=1&t=abc",
		"bvid=BV1xx411c7mD&cid=1&profile=h265", "bvid=BV1xx411c7mD&cid=1&fps=fast",
		"bvid=BV1xx411c7mD%0a-i%20/etc/passwd&cid=1",
	} {
		q, _ := url.ParseQuery(raw)
		if _, err := ParseRequest(q); err == nil {
			t.Errorf("%q should be rejected", raw)
		}
	}
	// A PGC episode has no BV id.
	if _, err := ParseRequest(url.Values{"pgc": {"1"}, "cid": {"5"}}); err != nil {
		t.Fatal(err)
	}
}

func TestBuildArgsDash(t *testing.T) {
	r := mustParse(t, "bvid=BV1xx411c7mD&cid=1&t=30&profile=cavlc")
	args := BuildArgs(r, Inputs{Video: "http://v/video", Audio: "http://v/audio"}, "UA")
	joined := strings.Join(args, " ")
	for _, want := range []string{
		"-ss 30.000 -user_agent UA", "-i http://v/video", "-i http://v/audio",
		"-map 0:v:0 -map 1:a:0", "-profile:v baseline", "cabac=0:bframes=0:ref=1", "pad=w=ceil(iw/16)*16", "-c:a aac", "-ar 44100 -ac 2",
		"-f flv", "-fpsmax 30", "force_divisible_by=2", "pipe:1", "min(480,iw)", "min(272,ih)",
	} {
		if !strings.Contains(joined, want) {
			t.Errorf("missing %q in %s", want, joined)
		}
	}
	if n := strings.Count(joined, "-ss "); n != 2 {
		t.Errorf("both inputs need their own -ss, got %d", n)
	}
	// -ss must precede its own -i, and no -ss may follow the last -i (that would seek the output).
	last := slices.Index(args, "http://v/audio")
	if slices.Contains(args[last:], "-ss") {
		t.Error("-ss after the last input")
	}
}

func TestBuildArgsNoSeekNoSSAndCABAC(t *testing.T) {
	r := mustParse(t, "bvid=BV1xx411c7mD&cid=1&profile=cabac")
	args := BuildArgs(r, Inputs{Muxed: "http://m/x.mp4"}, "UA")
	if slices.Contains(args, "-ss") {
		t.Error("t=0 must not add -ss")
	}
	j := strings.Join(args, " ")
	if !strings.Contains(j, "-profile:v main") || !strings.Contains(j, "cabac=1:bframes=0") || !strings.Contains(j, "-map 0:a:0?") {
		t.Fatal(j)
	}
}

func TestBuildArgsNeverInjectsThroughURLs(t *testing.T) {
	// A hostile URL is one argv element, not shell text.
	args := BuildArgs(mustParse(t, "bvid=BV1xx411c7mD&cid=1"), Inputs{Muxed: "http://x/ -i /etc/passwd"}, "UA")
	if !slices.Contains(args, "http://x/ -i /etc/passwd") {
		t.Fatal("URL must stay a single argument")
	}
}

func TestAttempts(t *testing.T) {
	got := Attempts(&bili.Media{Video: []string{"v1", "v2"}, Audio: []string{"a1"}})
	if len(got) != 2 || got[0] != (Inputs{Video: "v1", Audio: "a1"}) || got[1] != (Inputs{Video: "v2", Audio: "a1"}) {
		t.Fatalf("%+v", got)
	}
	got = Attempts(&bili.Media{Muxed: []string{"m1", "m2", "m3", "m4"}})
	if len(got) != 3 || got[2].Muxed != "m3" {
		t.Fatalf("%+v", got)
	}
}

func TestBuildArgs60FPSAndGOPFollowsEffectiveRate(t *testing.T) {
	r := mustParse(t, "bvid=BV1xx411c7mD&cid=1&fps=60")
	// A 60 fps source keeps 60 fps; the two-second GOP is 120 frames.
	j := strings.Join(BuildArgs(r, Inputs{Muxed: "m", SourceFPS: 59.94}, "UA"), " ")
	for _, want := range []string{"-fpsmax 60", "keyint=120:min-keyint=60"} {
		if !strings.Contains(j, want) {
			t.Errorf("missing %q in %s", want, j)
		}
	}
	// A 25 fps source under a 60 fps cap stays at 25: the GOP must not stretch to four seconds.
	j = strings.Join(BuildArgs(r, Inputs{Muxed: "m", SourceFPS: 25}, "UA"), " ")
	if !strings.Contains(j, "keyint=50:min-keyint=25") {
		t.Errorf("GOP should follow the source rate: %s", j)
	}
	// Unknown source rate falls back to the cap.
	j = strings.Join(BuildArgs(r, Inputs{Muxed: "m"}, "UA"), " ")
	if !strings.Contains(j, "keyint=120:min-keyint=60") {
		t.Errorf("unknown source rate: %s", j)
	}
	// The frame rate cap lowers it below the source.
	r30 := mustParse(t, "bvid=BV1xx411c7mD&cid=1&fps=30")
	if got := r30.EffectiveFPS(60); got != 30 {
		t.Errorf("cap 30 on a 60 fps source = %d", got)
	}
}

func TestAttemptsCarrySourceFPS(t *testing.T) {
	got := Attempts(&bili.Media{Video: []string{"v"}, Audio: []string{"a"}, FPS: 60})
	if len(got) != 1 || got[0].SourceFPS != 60 {
		t.Fatalf("%+v", got)
	}
}

func TestQualityHeadersListWhatTheVideoOffers(t *testing.T) {
	h := http.Header{}
	setQualityHeaders(h, &bili.Media{QN: 80, Qualities: []int{16, 32, 64, 80, 116}})
	if h.Get(HeaderQuality) != "80" || h.Get(HeaderQualities) != "16,32,64,80,116" {
		t.Fatalf("%v", h)
	}
	// A muxed fallback has one quality and no list; no media means no headers at all.
	h = http.Header{}
	setQualityHeaders(h, &bili.Media{QN: 32})
	if h.Get(HeaderQuality) != "32" || h.Get(HeaderQualities) != "" {
		t.Fatalf("%v", h)
	}
	h = http.Header{}
	setQualityHeaders(h, nil)
	setQualityHeaders(h, &bili.Media{})
	if len(h) != 0 {
		t.Fatalf("%v", h)
	}
}
