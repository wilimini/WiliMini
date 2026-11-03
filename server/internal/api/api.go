// Package api assembles the HTTP surface: /wili/v1/hello, /wili/v1/stream and
// the /p/ Bilibili proxy, all behind the pairing token.
package api

import (
	"crypto/subtle"
	"encoding/json"
	"net"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/proxy"
	"github.com/wilimini/wilimini/server/internal/stream"
)

// Proto is the wire protocol version. The PSP refuses a mismatch.
const Proto = 1

const (
	failLimit   = 10
	failWindow  = time.Minute
	TokenQuery  = "k"
	TokenHeader = "X-Wili-Token"
)

// Settings is what the server reads from the live configuration.
type Settings struct {
	Token      func() string
	Name       func() string
	ReadOnly   bool
	MaxStreams int
}

type Server struct {
	Settings Settings
	Store    *authstore.Store
	Proxy    *proxy.Handler
	Streams  *stream.Manager
	FFmpeg   stream.Info
	Version  string
	OnEvent  func(kind, message string)
	Now      func() time.Time

	mu    sync.Mutex
	fails map[string]*failure
}

type failure struct {
	count int
	since time.Time
}

func (s *Server) now() time.Time {
	if s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

func (s *Server) event(kind, msg string) {
	if s.OnEvent != nil {
		s.OnEvent(kind, msg)
	}
}

func jsonReply(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func errReply(w http.ResponseWriter, status int, code, message string) {
	jsonReply(w, status, map[string]string{"error": code, "message": message})
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// blocked reports whether ip has used up its failed-token budget.
func (s *Server) blocked(ip string) (time.Duration, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	f := s.fails[ip]
	if f == nil {
		return 0, false
	}
	if s.now().Sub(f.since) >= failWindow {
		delete(s.fails, ip)
		return 0, false
	}
	if f.count >= failLimit {
		return failWindow - s.now().Sub(f.since), true
	}
	return 0, false
}

func (s *Server) fail(ip string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.fails == nil {
		s.fails = map[string]*failure{}
	}
	f := s.fails[ip]
	if f == nil || s.now().Sub(f.since) >= failWindow {
		f = &failure{since: s.now()}
		s.fails[ip] = f
	}
	f.count++
}

func (s *Server) authorized(r *http.Request) bool {
	got := r.Header.Get(TokenHeader)
	if got == "" {
		got = r.URL.Query().Get(TokenQuery)
	}
	want := s.Settings.Token()
	return want != "" && subtle.ConstantTimeCompare([]byte(got), []byte(want)) == 1
}

// Handler returns the full server handler.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/wili/v1/hello", s.hello)
	mux.HandleFunc("/wili/v1/logout", s.logout)
	mux.Handle("/wili/v1/stream", s.Streams)
	mux.Handle(proxy.Prefix, s.Proxy)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		errReply(w, http.StatusNotFound, "not_found", "unknown route")
	})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ip := clientIP(r)
		if wait, blocked := s.blocked(ip); blocked {
			w.Header().Set("Retry-After", strconv.Itoa(int(wait.Seconds())+1))
			errReply(w, http.StatusTooManyRequests, "rate_limited", "too many wrong tokens; wait a minute")
			return
		}
		if !s.authorized(r) {
			s.fail(ip)
			s.event("warn", "rejected "+r.Method+" "+r.URL.Path+" from "+ip+" (token)")
			errReply(w, http.StatusUnauthorized, "unauthorized", "missing or wrong pairing token")
			return
		}
		mux.ServeHTTP(w, r)
	})
}

type helloReply struct {
	Proto      int    `json:"proto"`
	Name       string `json:"name"`
	Version    string `json:"version"`
	Login      login  `json:"login"`
	FFmpeg     ffmpeg `json:"ffmpeg"`
	ReadOnly   bool   `json:"read_only"`
	MaxStreams int    `json:"max_streams"`
	Streams    int    `json:"streams"`
}

type login struct {
	State string `json:"state"` // "signed_in" or "signed_out"
	Mid   int64  `json:"mid,omitempty"`
	Name  string `json:"name,omitempty"`
}

type ffmpeg struct {
	OK      bool   `json:"ok"`
	Version string `json:"version,omitempty"`
	Problem string `json:"problem,omitempty"`
}

// logout forgets the server's Bilibili account. The reply uses Bilibili's
// {"code","data"} envelope so the PSP's ordinary API client can read it.
func (s *Server) logout(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		errReply(w, http.StatusMethodNotAllowed, "method", "GET or POST only")
		return
	}
	if err := s.Store.Clear(); err != nil {
		errReply(w, http.StatusInternalServerError, "storage", err.Error())
		return
	}
	s.event("info", "signed out from "+clientIP(r))
	jsonReply(w, http.StatusOK, map[string]any{"code": 0, "message": "0", "data": map[string]string{"state": "signed_out"}})
}

func (s *Server) hello(w http.ResponseWriter, r *http.Request) {
	reply := helloReply{
		Proto: Proto, Name: s.Settings.Name(), Version: s.Version,
		Login:      login{State: "signed_out"},
		FFmpeg:     ffmpeg{OK: s.FFmpeg.OK, Version: s.FFmpeg.Version, Problem: s.FFmpeg.Problem},
		ReadOnly:   s.Settings.ReadOnly,
		MaxStreams: s.Settings.MaxStreams,
		Streams:    len(s.Streams.Snapshot()),
	}
	if s.Store.LoggedIn() {
		mid, name := s.Store.Identity()
		reply.Login = login{State: "signed_in", Mid: mid, Name: name}
	}
	jsonReply(w, http.StatusOK, reply)
}
