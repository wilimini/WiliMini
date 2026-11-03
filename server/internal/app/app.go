// Package app wires the server together and offers the operations the CLI and
// the TUI share: start/stop, login, token management, status snapshots.
package app

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/wilimini/wilimini/server/internal/api"
	"github.com/wilimini/wilimini/server/internal/authstore"
	"github.com/wilimini/wilimini/server/internal/bili"
	"github.com/wilimini/wilimini/server/internal/config"
	"github.com/wilimini/wilimini/server/internal/discovery"
	"github.com/wilimini/wilimini/server/internal/proxy"
	"github.com/wilimini/wilimini/server/internal/stream"
)

const logKeep = 200

// Options are command-line overrides of the saved configuration.
type Options struct {
	Dir         string
	Port        int    // 0 = from config
	Name        string // "" = from config
	Listen      string // host to bind, "" = all interfaces
	ReadOnly    *bool
	MaxStreams  int
	FFmpegPath  string
	NoDiscovery bool
	LogWriter   io.Writer // headless mode prints events here
	Version     string
}

type LogLine struct {
	Time    time.Time
	Kind    string
	Message string
}

type LoginView struct {
	Active  bool
	State   bili.QRState
	Content string // text to encode as a QR code
	Message string
	Done    bool
}

type App struct {
	Dir     string
	Version string

	mu    sync.Mutex
	cfg   *config.Config
	opts  Options
	logs  []LogLine
	login LoginView

	Store   *authstore.Store
	Bili    *bili.Client
	Streams *stream.Manager
	API     *api.Server
	FFmpeg  stream.Info

	listener   net.Listener
	srv        *http.Server
	mdnsCount  int
	mdnsErr    string
	beaconAddr string
	beaconErr  string
}

// New loads configuration and builds the components. Nothing listens yet.
func New(ctx context.Context, opts Options) (*App, error) {
	dir := opts.Dir
	if dir == "" {
		var err error
		if dir, err = config.Dir(); err != nil {
			return nil, err
		}
	}
	cfg, err := config.Load(dir)
	if err != nil {
		return nil, err
	}
	if opts.Port > 0 {
		cfg.Port = opts.Port
	}
	if opts.Name != "" {
		cfg.Name = opts.Name
	}
	if opts.ReadOnly != nil {
		cfg.ReadOnly = *opts.ReadOnly
	}
	if opts.MaxStreams > 0 {
		cfg.MaxStreams = opts.MaxStreams
	}
	if opts.FFmpegPath != "" {
		cfg.FFmpeg = opts.FFmpegPath
	}
	store, err := authstore.Open(dir)
	if err != nil {
		return nil, fmt.Errorf("session file: %w", err)
	}
	a := &App{Dir: dir, Version: opts.Version, cfg: cfg, opts: opts, Store: store}
	a.Bili = bili.New(store)
	a.FFmpeg = stream.Probe(ctx, cfg.FFmpeg)
	ffmpegPath := ""
	if a.FFmpeg.OK {
		ffmpegPath = a.FFmpeg.Path
	}
	a.Streams = &stream.Manager{Bili: a.Bili, FFmpeg: ffmpegPath, Max: cfg.MaxStreams, OnEvent: a.Log}
	prox := &proxy.Handler{Bili: a.Bili, Store: store, ReadOnly: cfg.ReadOnly,
		OnRequest: func(method, host, path string, status int) {
			a.Log("http", fmt.Sprintf("%s %s%s -> %d", method, host, path, status))
		}}
	a.API = &api.Server{
		Settings: api.Settings{Token: a.Token, Name: a.Name, ReadOnly: cfg.ReadOnly, MaxStreams: cfg.MaxStreams},
		Store:    store, Proxy: prox, Streams: a.Streams, FFmpeg: a.FFmpeg, Version: a.Version, OnEvent: a.Log,
	}
	return a, nil
}

func (a *App) Token() string { a.mu.Lock(); defer a.mu.Unlock(); return a.cfg.Token }
func (a *App) Name() string  { a.mu.Lock(); defer a.mu.Unlock(); return a.cfg.Name }
func (a *App) Port() int     { a.mu.Lock(); defer a.mu.Unlock(); return a.cfg.Port }

// NewToken replaces the pairing token; the old one stops working at once.
func (a *App) NewToken() (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.cfg.Token = config.NewToken()
	if err := a.cfg.Save(a.Dir); err != nil {
		return "", err
	}
	a.logLocked("info", "pairing token regenerated")
	return a.cfg.Token, nil
}

// Log records an event. Messages must never contain the token or cookies.
func (a *App) Log(kind, message string) {
	a.mu.Lock()
	a.logLocked(kind, message)
	w := a.opts.LogWriter
	a.mu.Unlock()
	if w != nil {
		fmt.Fprintf(w, "%s %-6s %s\n", time.Now().Format("15:04:05"), kind, message)
	}
}

func (a *App) logLocked(kind, message string) {
	a.logs = append(a.logs, LogLine{Time: time.Now(), Kind: kind, Message: message})
	if len(a.logs) > logKeep {
		a.logs = a.logs[len(a.logs)-logKeep:]
	}
}

// Start binds the HTTP port, starts discovery and checks the saved login.
// It returns once the listener is up; Serve errors are logged.
func (a *App) Start(ctx context.Context) error {
	port := a.Port()
	ln, err := net.Listen("tcp", net.JoinHostPort(a.opts.Listen, strconv.Itoa(port)))
	if err != nil {
		return fmt.Errorf("cannot listen on port %d (is another wili-server running?): %w", port, err)
	}
	a.listener = ln
	a.srv = &http.Server{
		Handler:           a.API.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       2 * time.Minute,
		// No WriteTimeout: a stream lasts as long as the video.
	}
	go func() {
		if err := a.srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			a.Log("error", "http server stopped: "+err.Error())
		}
	}()
	go func() {
		<-ctx.Done()
		sctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = a.srv.Shutdown(sctx)
		_ = a.srv.Close()
	}()

	if !a.opts.NoDiscovery {
		a.startDiscovery(ctx, port)
	}
	go a.watchSession(ctx)
	return nil
}

func (a *App) startDiscovery(ctx context.Context, port int) {
	svc := &discovery.Service{Instance: a.Name(), Host: discovery.HostLabel(), Port: port, Proto: api.Proto, Version: a.Version}
	r := &discovery.Responder{Service: svc, Logf: func(f string, args ...any) { a.Log("mdns", fmt.Sprintf(f, args...)) }}
	if n, err := r.Run(ctx); err != nil {
		a.mu.Lock()
		a.mdnsErr = err.Error()
		a.mu.Unlock()
	} else {
		a.mu.Lock()
		a.mdnsCount = n
		a.mu.Unlock()
	}
	addr, err := discovery.ServeBeacon(ctx, fmt.Sprintf(":%d", discovery.BeaconPort), port, a.Name)
	a.mu.Lock()
	defer a.mu.Unlock()
	if err != nil {
		a.beaconErr = err.Error()
		return
	}
	a.beaconAddr = addr.String()
}

// watchSession validates the saved cookies at start and every few hours.
func (a *App) watchSession(ctx context.Context) {
	check := func() {
		if !a.Store.LoggedIn() {
			return
		}
		cctx, cancel := context.WithTimeout(ctx, 20*time.Second)
		defer cancel()
		info, err := a.Bili.Nav(cctx)
		switch {
		case err != nil:
			a.Log("warn", "could not check the Bilibili session: "+err.Error())
		case !info.IsLogin:
			a.Log("warn", "the saved Bilibili session has expired; sign in again")
			_ = a.Store.Clear()
		default:
			a.Log("info", fmt.Sprintf("signed in as %s (uid %d)", info.Uname, info.Mid))
		}
	}
	check()
	t := time.NewTicker(6 * time.Hour)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			check()
		}
	}
}

// Login runs the QR flow until success, cancellation or too many expiries.
// update is called whenever the view changes.
func (a *App) Login(ctx context.Context, update func(LoginView)) error {
	set := func(v LoginView) {
		a.mu.Lock()
		a.login = v
		a.mu.Unlock()
		if update != nil {
			update(v)
		}
	}
	defer func() {
		a.mu.Lock()
		a.login.Active = false
		v := a.login
		a.mu.Unlock()
		if update != nil {
			update(v)
		}
	}()
	for round := 0; round < 4; round++ {
		key, content, err := a.Bili.QRGenerate(ctx)
		if err != nil {
			set(LoginView{Message: "could not start login: " + err.Error()})
			return err
		}
		set(LoginView{Active: true, State: bili.QRWaitingScan, Content: content, Message: "scan with the Bilibili app"})
		deadline := time.Now().Add(3 * time.Minute)
		for time.Now().Before(deadline) {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(2 * time.Second):
			}
			state, err := a.Bili.QRPoll(ctx, key)
			if err != nil {
				set(LoginView{Message: "login failed: " + err.Error()})
				return err
			}
			switch state {
			case bili.QRDone:
				_, name := a.Store.Identity()
				set(LoginView{Done: true, State: state, Message: "signed in as " + name})
				a.Log("info", "signed in as "+name)
				return nil
			case bili.QRExpired:
				deadline = time.Now() // regenerate
			default:
				set(LoginView{Active: true, State: state, Content: content, Message: state.String()})
			}
		}
	}
	set(LoginView{Message: "the QR code kept expiring; try again"})
	return errors.New("login timed out")
}

// LoginView returns the latest login progress.
func (a *App) LoginView() LoginView { a.mu.Lock(); defer a.mu.Unlock(); return a.login }

// Logout forgets the account.
func (a *App) Logout() error {
	if err := a.Store.Clear(); err != nil {
		return err
	}
	a.Log("info", "signed out; the saved session was deleted")
	return nil
}

// Snapshot is a consistent view for the TUI and the banner.
type Snapshot struct {
	Name, Token, Version string
	Port                 int
	Addrs                []string // host:port the PSP can type
	MDNSName             string
	LoggedIn             bool
	Mid                  int64
	Uname                string
	FFmpeg               stream.Info
	Streams              []stream.SessionInfo
	Logs                 []LogLine
	Login                LoginView
	ReadOnly             bool
	MDNSLine, BeaconLine string
}

func (a *App) Snapshot() Snapshot {
	a.mu.Lock()
	s := Snapshot{Name: a.cfg.Name, Token: a.cfg.Token, Version: a.Version, Port: a.cfg.Port,
		ReadOnly: a.cfg.ReadOnly, FFmpeg: a.FFmpeg, Login: a.login, Logs: append([]LogLine(nil), a.logs...)}
	switch {
	case a.mdnsErr != "":
		s.MDNSLine = "mDNS off: " + a.mdnsErr
	case a.mdnsCount > 0:
		s.MDNSLine = fmt.Sprintf("mDNS on %d interface(s) as %q", a.mdnsCount, a.cfg.Name)
	case a.opts.NoDiscovery:
		s.MDNSLine = "discovery disabled"
	}
	switch {
	case a.beaconErr != "":
		s.BeaconLine = "UDP beacon off: " + a.beaconErr
	case a.beaconAddr != "":
		s.BeaconLine = "UDP beacon on port " + strconv.Itoa(discovery.BeaconPort)
	}
	a.mu.Unlock()
	for _, ip := range discovery.LANAddrs() {
		s.Addrs = append(s.Addrs, net.JoinHostPort(ip.String(), strconv.Itoa(s.Port)))
	}
	s.MDNSName = discovery.HostLabel() + ".local"
	s.LoggedIn = a.Store.LoggedIn()
	s.Mid, s.Uname = a.Store.Identity()
	s.Streams = a.Streams.Snapshot()
	return s
}

// Banner is the text printed in headless mode and before the TUI starts.
func (s Snapshot) Banner() string {
	var b strings.Builder
	fmt.Fprintf(&b, "WiliMini server %s  —  %q\n\n", s.Version, s.Name)
	b.WriteString("On the PSP: Settings → Stream server → enter\n")
	if len(s.Addrs) == 0 {
		b.WriteString("    (no LAN address found; check the network connection)\n")
	}
	for _, a := range s.Addrs {
		fmt.Fprintf(&b, "    address   %s\n", a)
	}
	fmt.Fprintf(&b, "    or name   %s:%d\n", s.MDNSName, s.Port)
	fmt.Fprintf(&b, "    token     %s\n\n", s.Token)
	if s.LoggedIn {
		who := s.Uname
		if who == "" {
			who = "unknown"
		}
		fmt.Fprintf(&b, "Bilibili:   signed in as %s (uid %d)\n", who, s.Mid)
	} else {
		b.WriteString("Bilibili:   not signed in (guest quality). Run `wili-server login` or press l in the TUI.\n")
	}
	if s.FFmpeg.OK {
		fmt.Fprintf(&b, "ffmpeg:     %s\n", s.FFmpeg.Version)
	} else {
		fmt.Fprintf(&b, "ffmpeg:     PROBLEM — %s\n", s.FFmpeg.Problem)
	}
	if s.MDNSLine != "" {
		fmt.Fprintf(&b, "Discovery:  %s\n", s.MDNSLine)
	}
	if s.BeaconLine != "" {
		fmt.Fprintf(&b, "            %s\n", s.BeaconLine)
	}
	if s.ReadOnly {
		b.WriteString("Mode:       read-only (likes, comments and other writes are blocked)\n")
	}
	return b.String()
}
