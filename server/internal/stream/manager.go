package stream

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/wilimini/wilimini/server/internal/bili"
)

const (
	firstByteTimeout = 25 * time.Second
	stderrKeep       = 4096
)

// Info describes the ffmpeg found at startup.
type Info struct {
	Path    string `json:"path"`
	Version string `json:"version"`
	OK      bool   `json:"ok"`
	Problem string `json:"problem,omitempty"`
}

// Probe locates ffmpeg and runs a tiny encode with the exact options the
// server uses, so a missing encoder or an old ffmpeg is reported at startup.
func Probe(ctx context.Context, path string) Info {
	if path == "" {
		path = "ffmpeg"
	}
	resolved, err := exec.LookPath(path)
	if err != nil {
		return Info{Path: path, Problem: "ffmpeg not found; install it or set --ffmpeg"}
	}
	info := Info{Path: resolved}
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, resolved, "-hide_banner", "-version").Output()
	if err == nil {
		if f := strings.Fields(string(out)); len(f) >= 3 && f[0] == "ffmpeg" && f[1] == "version" {
			info.Version = f[2]
		}
	}
	var stderr bytes.Buffer
	cmd := exec.CommandContext(ctx, resolved, "-hide_banner", "-nostdin", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc2=size=96x54:rate=30:duration=0.2",
		"-f", "lavfi", "-i", "sine=duration=0.2",
		"-vf", "scale=w='min(64,iw)':h='min(64,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2",
		"-fpsmax", "30", "-c:v", "libx264", "-profile:v", "baseline", "-x264-params", "cabac=0:bframes=0",
		"-c:a", "aac", "-f", "flv", "-")
	cmd.Stdout, cmd.Stderr = io.Discard, &stderr
	if err := cmd.Run(); err != nil {
		info.Problem = "ffmpeg cannot encode H.264+AAC to FLV (needs ffmpeg 5.1+ with libx264): " + lastLine(stderr.String())
		return info
	}
	info.OK = true
	return info
}

func lastLine(s string) string {
	s = strings.TrimSpace(s)
	if i := strings.LastIndexByte(s, '\n'); i >= 0 {
		s = s[i+1:]
	}
	if len(s) > 200 {
		s = s[:200]
	}
	return s
}

// SessionInfo is a read-only view for the TUI and /hello.
type SessionInfo struct {
	ID      int64     `json:"id"`
	Remote  string    `json:"remote"`
	Desc    string    `json:"desc"`
	Started time.Time `json:"started"`
	Bytes   int64     `json:"bytes"`
}

type session struct {
	SessionInfo
	cancel context.CancelFunc
	bytes  atomic.Int64
}

// Manager owns the running ffmpeg processes.
type Manager struct {
	Bili      *bili.Client
	FFmpeg    string // resolved path; empty means missing
	Max       int
	OnEvent   func(kind, message string)
	ClientKey func(*http.Request) string // overridden in tests

	mu     sync.Mutex
	active map[int64]*session
	nextID int64
}

func (m *Manager) event(kind, format string, args ...any) {
	if m.OnEvent != nil {
		m.OnEvent(kind, fmt.Sprintf(format, args...))
	}
}

// Snapshot lists running streams, oldest first.
func (m *Manager) Snapshot() []SessionInfo {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]SessionInfo, 0, len(m.active))
	for _, s := range m.active {
		info := s.SessionInfo
		info.Bytes = s.bytes.Load()
		out = append(out, info)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func (m *Manager) clientKey(r *http.Request) string {
	if m.ClientKey != nil {
		return m.ClientKey(r)
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// admit registers a session, replacing an older one from the same client (a
// seek or retry from the PSP) and refusing when the server is full.
func (m *Manager) admit(key, desc string, cancel context.CancelFunc) (*session, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.active == nil {
		m.active = map[int64]*session{}
	}
	for id, s := range m.active {
		if s.Remote == key {
			s.cancel()
			delete(m.active, id)
		}
	}
	limit := m.Max
	if limit <= 0 {
		limit = 2
	}
	if len(m.active) >= limit {
		return nil, false
	}
	m.nextID++
	s := &session{SessionInfo: SessionInfo{ID: m.nextID, Remote: key, Desc: desc, Started: time.Now()}, cancel: cancel}
	m.active[s.ID] = s
	return s, true
}

func (m *Manager) release(s *session) {
	m.mu.Lock()
	if cur, ok := m.active[s.ID]; ok && cur == s {
		delete(m.active, s.ID)
	}
	m.mu.Unlock()
	s.cancel()
}

func writeErr(w http.ResponseWriter, code, message string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(ErrorStatus(code))
	_ = json.NewEncoder(w).Encode(map[string]string{"error": code, "message": message})
}

// ServeHTTP handles GET /wili/v1/stream.
func (m *Manager) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", "GET")
		writeErr(w, "bad_request", "GET only")
		return
	}
	req, err := ParseRequest(r.URL.Query())
	if err != nil {
		writeErr(w, "bad_request", err.Error())
		return
	}
	if m.FFmpeg == "" {
		writeErr(w, "ffmpeg_missing", "ffmpeg is not installed on the server")
		return
	}
	ctx, cancel := context.WithCancel(r.Context())
	sess, ok := m.admit(m.clientKey(r), req.String(), cancel)
	if !ok {
		cancel()
		writeErr(w, "busy", "too many streams are running")
		return
	}
	defer m.release(sess)
	m.event("stream", "#%d start %s for %s", sess.ID, req, sess.Remote)

	media, err := m.Bili.PlayURL(ctx, bili.PlayRequest{BVID: req.BVID, CID: req.CID, QN: req.QN, PGC: req.PGC})
	if err != nil {
		if ctx.Err() != nil {
			return
		}
		switch {
		case bili.IsLoginRequired(err):
			writeErr(w, "login_required", "this video needs a signed-in account; run `wili-server login`")
		case errors.Is(err, bili.ErrNoStream):
			writeErr(w, "no_stream", "Bilibili offers no H.264 or HEVC stream for this video")
		default:
			writeErr(w, "upstream", err.Error())
		}
		m.event("error", "#%d play url: %v", sess.ID, err)
		return
	}
	m.event("stream", "#%d source qn=%d %dx%d %.0f fps", sess.ID, media.QN, media.Width, media.Height, media.FPS)
	attempts := Attempts(media)
	var lastErr string
	for i, in := range attempts {
		started, serr := m.runFFmpeg(ctx, w, req, in, sess, media)
		if started {
			m.event("stream", "#%d finished after %d KiB", sess.ID, sess.bytes.Load()/1024)
			return
		}
		if ctx.Err() != nil {
			return
		}
		lastErr = serr
		m.event("warn", "#%d attempt %d/%d failed: %s", sess.ID, i+1, len(attempts), serr)
	}
	writeErr(w, "upstream", "ffmpeg could not read the video: "+lastErr)
}

// runFFmpeg runs one attempt. It returns started=true once response bytes were
// sent (after which a retry is impossible); otherwise it returns ffmpeg's last
// complaint without having written anything.
func (m *Manager) runFFmpeg(ctx context.Context, w http.ResponseWriter, req Request, in Inputs, sess *session, media *bili.Media) (started bool, problem string) {
	cmd := exec.CommandContext(ctx, m.FFmpeg, BuildArgs(req, in, m.Bili.UA)...)
	cmd.WaitDelay = 3 * time.Second
	var errTail tailBuffer
	cmd.Stderr = &errTail
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return false, err.Error()
	}
	if err := cmd.Start(); err != nil {
		return false, err.Error()
	}
	defer func() { _ = cmd.Wait() }()

	// Wait for the first bytes so failures can still be reported as HTTP errors.
	first := make([]byte, 32*1024)
	type result struct {
		n   int
		err error
	}
	done := make(chan result, 1)
	go func() {
		n, err := io.ReadAtLeast(stdout, first, 1)
		done <- result{n, err}
	}()
	timer := time.NewTimer(firstByteTimeout)
	defer timer.Stop()
	var res result
	select {
	case res = <-done:
	case <-timer.C:
		_ = killProcess(cmd)
		return false, "timed out waiting for the first frame"
	case <-ctx.Done():
		_ = killProcess(cmd)
		return false, "cancelled"
	}
	if res.err != nil && res.n == 0 {
		_ = killProcess(cmd)
		return false, errTail.Summary()
	}

	h := w.Header()
	h.Set("Content-Type", "video/x-flv")
	h.Set("Cache-Control", "no-store")
	h.Set("X-Content-Type-Options", "nosniff")
	setQualityHeaders(h, media)
	rc := http.NewResponseController(w)
	_ = rc.SetWriteDeadline(time.Time{}) // a long film must not hit a server write timeout
	w.WriteHeader(http.StatusOK)
	write := func(p []byte) bool {
		if _, err := w.Write(p); err != nil {
			return false
		}
		sess.bytes.Add(int64(len(p)))
		_ = rc.Flush() // best effort: a recorder in front may not support it
		return true
	}
	if !write(first[:res.n]) {
		_ = killProcess(cmd)
		return true, ""
	}
	buf := make([]byte, 32*1024)
	for {
		n, err := stdout.Read(buf)
		if n > 0 && !write(buf[:n]) {
			_ = killProcess(cmd)
			return true, ""
		}
		if err != nil {
			return true, ""
		}
	}
}

// Headers that tell the PSP which tier this stream is and which others the video offers, so its
// quality menu lists what this video really has (the tiers differ from video to video).
const (
	HeaderQuality   = "X-Wili-Quality"   // the tier being streamed
	HeaderQualities = "X-Wili-Qualities" // comma-separated tiers the video can be played at
)

func setQualityHeaders(h http.Header, media *bili.Media) {
	if media == nil || media.QN <= 0 {
		return
	}
	h.Set(HeaderQuality, strconv.Itoa(media.QN))
	if len(media.Qualities) > 0 {
		parts := make([]string, len(media.Qualities))
		for i, q := range media.Qualities {
			parts[i] = strconv.Itoa(q)
		}
		h.Set(HeaderQualities, strings.Join(parts, ","))
	}
}

func killProcess(cmd *exec.Cmd) error {
	if cmd.Process == nil {
		return nil
	}
	return cmd.Process.Kill()
}

// tailBuffer keeps the last few KiB of ffmpeg's stderr.
type tailBuffer struct {
	mu  sync.Mutex
	buf []byte
}

func (t *tailBuffer) Write(p []byte) (int, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.buf = append(t.buf, p...)
	if len(t.buf) > stderrKeep {
		t.buf = t.buf[len(t.buf)-stderrKeep:]
	}
	return len(p), nil
}

// Summary returns the most informative recent line.
func (t *tailBuffer) Summary() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	lines := strings.Split(strings.TrimSpace(string(t.buf)), "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if l := strings.TrimSpace(lines[i]); l != "" {
			if len(l) > 300 {
				l = l[:300]
			}
			return l
		}
	}
	return "ffmpeg exited without output"
}
