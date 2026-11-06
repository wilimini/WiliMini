// Package stream turns a Bilibili video into something the PSP's
// hardware decoder handles: H.264 Main/CABAC (or Baseline/CAVLC) fitted into 480x272 with both sides a
// multiple of 16, plus AAC, in FLV, produced by an ffmpeg child process whose
// stdout is the HTTP response. The output size is fixed; the source quality (any
// Bilibili tier, up to 4K and 60 fps), the frame-rate cap and the bitrate are the
// request's knobs.
package stream

import (
	"fmt"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"

	"github.com/wilimini/wilimini/server/internal/bili"
)

type Profile string

const (
	ProfileAuto  Profile = "auto"
	ProfileCAVLC Profile = "cavlc"
	ProfileCABAC Profile = "cabac"
)

// The PSP screen. Every stream is fitted into this box whatever the source is.
const (
	OutputWidth  = 480
	OutputHeight = 272
)

// Limits and defaults for Wi-Fi and the decoder. H.264 level 3.0 allows 480x272 at up to 76 fps.
const (
	DefaultFPS       = 30
	MaxFPS           = 60
	DefaultVideoKbps = 1200
	MaxVideoKbps     = 4000
	DefaultAudioKbps = 96
	MaxSeekSeconds   = 12 * 3600
)

var bvidPattern = regexp.MustCompile(`^BV[0-9A-Za-z]{10}$`)

// Request is a validated /wili/v1/stream query.
type Request struct {
	BVID      string
	CID       int64
	PGC       bool
	StartSec  float64
	Profile   Profile
	FPS       int // cap: the output never exceeds the source's own frame rate
	VideoKbps int
	AudioKbps int
	QN        int // Bilibili quality tier of the source
}

// ParseRequest validates the query, applying defaults and clamping values. The old w and h
// parameters are ignored: the output size is fixed.
func ParseRequest(q url.Values) (Request, error) {
	r := Request{Profile: ProfileAuto, FPS: DefaultFPS,
		VideoKbps: DefaultVideoKbps, AudioKbps: DefaultAudioKbps, QN: bili.QN480}
	r.PGC = q.Get("pgc") == "1"
	r.BVID = q.Get("bvid")
	if !r.PGC && !bvidPattern.MatchString(r.BVID) {
		return r, fmt.Errorf("bvid must look like BV1xxxxxxxxx")
	}
	cid, err := strconv.ParseInt(q.Get("cid"), 10, 64)
	if err != nil || cid <= 0 {
		return r, fmt.Errorf("cid is required")
	}
	r.CID = cid
	num := func(name string, lo, hi int, dst *int) error {
		s := q.Get(name)
		if s == "" {
			return nil
		}
		v, err := strconv.Atoi(s)
		if err != nil {
			return fmt.Errorf("%s must be a number", name)
		}
		*dst = min(max(v, lo), hi)
		return nil
	}
	for _, f := range []struct {
		name   string
		lo, hi int
		dst    *int
	}{
		{"fps", 5, MaxFPS, &r.FPS},
		{"vb", 100, MaxVideoKbps, &r.VideoKbps}, {"ab", 32, 192, &r.AudioKbps}, {"qn", bili.QN360, bili.QN4K, &r.QN},
	} {
		if err := num(f.name, f.lo, f.hi, f.dst); err != nil {
			return r, err
		}
	}
	if s := q.Get("t"); s != "" {
		t, err := strconv.ParseFloat(s, 64)
		if err != nil || t < 0 {
			return r, fmt.Errorf("t must be a non-negative number of seconds")
		}
		r.StartSec = min(t, MaxSeekSeconds)
	}
	switch p := Profile(strings.ToLower(q.Get("profile"))); p {
	case "", ProfileAuto:
		r.Profile = ProfileAuto
	case ProfileCAVLC, ProfileCABAC:
		r.Profile = p
	default:
		return r, fmt.Errorf("profile must be auto, cavlc or cabac")
	}
	return r, nil
}

// EncoderProfile resolves auto to CABAC. The PSP's hardware decoder (driven the way Moonlight-PSP
// does) runs in one of two modes, Main/CABAC or Baseline/CAVLC, so CABAC means Main profile and
// CAVLC means Baseline profile. Main/CABAC is also what every PSP game movie uses.
func (r Request) EncoderProfile() Profile {
	if r.Profile == ProfileCAVLC {
		return ProfileCAVLC
	}
	return ProfileCABAC
}

// Inputs is one ffmpeg attempt: the URLs to read.
type Inputs struct {
	Video, Audio, Muxed string
	SourceFPS           float64 // the rendition's own frame rate, 0 when unknown
}

// Attempts lists the URL combinations to try in order (primary, then CDN
// fallbacks), at most three.
func Attempts(m *bili.Media) []Inputs {
	pick := func(list []string, i int) string {
		if len(list) == 0 {
			return ""
		}
		return list[min(i, len(list)-1)]
	}
	n := 1
	for _, l := range [][]string{m.Video, m.Audio, m.Muxed} {
		n = max(n, len(l))
	}
	n = min(n, 3)
	out := make([]Inputs, 0, n)
	for i := 0; i < n; i++ {
		if len(m.Muxed) > 0 {
			out = append(out, Inputs{Muxed: pick(m.Muxed, i), SourceFPS: m.FPS})
		} else {
			out = append(out, Inputs{Video: pick(m.Video, i), Audio: pick(m.Audio, i), SourceFPS: m.FPS})
		}
	}
	return out
}

// BuildArgs returns the ffmpeg argument vector (without the program name).
func BuildArgs(r Request, in Inputs, ua string) []string {
	args := []string{"-hide_banner", "-nostdin", "-loglevel", "warning"}
	headers := "Referer: " + bili.DefaultReferer + "\r\n"
	input := func(u string) {
		if r.StartSec > 0 {
			args = append(args, "-ss", strconv.FormatFloat(r.StartSec, 'f', 3, 64))
		}
		args = append(args, "-user_agent", ua, "-headers", headers,
			"-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "4", "-i", u)
	}
	if in.Muxed != "" {
		input(in.Muxed)
		args = append(args, "-map", "0:v:0", "-map", "0:a:0?")
	} else {
		input(in.Video)
		input(in.Audio)
		args = append(args, "-map", "0:v:0", "-map", "1:a:0")
	}
	// Fit inside the box, then pad to whole macroblocks: the PSP decoder wants a coded size
	// that is a multiple of 16 and no cropping, and padding keeps the picture undistorted.
	scale := fmt.Sprintf("scale=w='min(%d,iw)':h='min(%d,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2,"+
		"pad=w=ceil(iw/16)*16:h=ceil(ih/16)*16:x=(ow-iw)/2:y=(oh-ih)/2:color=black,setsar=1",
		OutputWidth, OutputHeight)
	args = append(args, "-vf", scale, "-fpsmax", strconv.Itoa(r.FPS), "-pix_fmt", "yuv420p")

	// The decoder's two modes: Main with CABAC (the default) or Baseline with CAVLC.
	profile, cabac := "main", "1"
	if r.EncoderProfile() == ProfileCAVLC {
		profile, cabac = "baseline", "0"
	}
	// No B-frames, one reference frame and a two-second GOP keep decoding cheap, stay within
	// what the firmware decoder accepts, and make restart-based seeking land close to the target.
	// The GOP is counted in output frames, so it follows the frame rate actually produced.
	fps := r.EffectiveFPS(in.SourceFPS)
	x264 := fmt.Sprintf("cabac=%s:bframes=0:ref=1:keyint=%d:min-keyint=%d:scenecut=0:rc-lookahead=10",
		cabac, fps*2, fps)
	args = append(args,
		"-c:v", "libx264", "-preset", "veryfast", "-profile:v", profile, "-level:v", "3.0",
		"-x264-params", x264,
		"-b:v", fmt.Sprintf("%dk", r.VideoKbps), "-maxrate", fmt.Sprintf("%dk", r.VideoKbps*5/4),
		"-bufsize", fmt.Sprintf("%dk", r.VideoKbps*2),
		"-c:a", "aac", "-b:a", fmt.Sprintf("%dk", r.AudioKbps), "-ar", "44100", "-ac", "2",
		"-f", "flv", "-flvflags", "no_duration_filesize", "-flush_packets", "1", "pipe:1")
	return args
}

// EffectiveFPS is the frame rate the output will have: the cap, or the source's own rate when
// that is lower (ffmpeg's -fpsmax never adds frames).
func (r Request) EffectiveFPS(source float64) int {
	if source >= 1 && int(source+0.5) < r.FPS {
		return max(int(source+0.5), 5)
	}
	return r.FPS
}

// String describes the request in logs.
func (r Request) String() string {
	return fmt.Sprintf("%s cid=%d t=%.1f %s qn=%d %dx%d<=%dfps %dk", r.BVID, r.CID, r.StartSec, r.EncoderProfile(),
		r.QN, OutputWidth, OutputHeight, r.FPS, r.VideoKbps)
}

// ErrorStatus maps a failure class to an HTTP status.
func ErrorStatus(code string) int {
	switch code {
	case "bad_request":
		return http.StatusBadRequest
	case "login_required":
		return http.StatusForbidden
	case "no_stream":
		return http.StatusNotFound
	case "busy", "ffmpeg_missing":
		return http.StatusServiceUnavailable
	default:
		return http.StatusBadGateway
	}
}
