package stream

import (
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
	if r.Width != 480 || r.Height != 272 || r.FPS != 30 || r.VideoKbps != 1200 || r.QN != 32 || r.EncoderProfile() != ProfileCAVLC {
		t.Fatalf("%+v", r)
	}
	r = mustParse(t, "bvid=BV1xx411c7mD&cid=12&w=4000&h=1&fps=120&vb=99999&ab=1&qn=500&t=61.5&profile=CABAC")
	if r.Width != 720 || r.Height != 64 || r.FPS != 30 || r.VideoKbps != 4000 || r.AudioKbps != 32 || r.QN != 80 || r.StartSec != 61.5 || r.EncoderProfile() != ProfileCABAC {
		t.Fatalf("%+v", r)
	}
	if r = mustParse(t, "bvid=BV1xx411c7mD&cid=1&t=99999999"); r.StartSec != MaxSeekSeconds {
		t.Fatalf("t=%v", r.StartSec)
	}
}

func TestParseRejectsBadInput(t *testing.T) {
	for _, raw := range []string{
		"cid=1", "bvid=BV1xx411c7mD", "bvid=BV1xx411c7mD&cid=0", "bvid=BV1xx411c7mD&cid=x",
		"bvid=../etc&cid=1", "bvid=BV1xx411c7mD&cid=1&t=-3", "bvid=BV1xx411c7mD&cid=1&t=abc",
		"bvid=BV1xx411c7mD&cid=1&profile=h265", "bvid=BV1xx411c7mD&cid=1&w=wide",
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
		"-map 0:v:0 -map 1:a:0", "-profile:v main", "cabac=0:bframes=0:ref=1", "pad=w=ceil(iw/16)*16", "-c:a aac", "-ar 44100 -ac 2",
		"-f flv", "-fpsmax 30", "force_divisible_by=2", "pipe:1",
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
