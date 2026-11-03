// Package tui is the optional terminal dashboard (--tui).
package tui

import (
	"context"
	"fmt"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"

	"github.com/wilimini/wilimini/server/internal/app"
	"github.com/wilimini/wilimini/server/internal/qr"
)

var (
	pink    = lipgloss.Color("#fb7299")
	dim     = lipgloss.Color("#8a8a8a")
	good    = lipgloss.Color("#4caf50")
	warn    = lipgloss.Color("#e6a23c")
	title   = lipgloss.NewStyle().Bold(true).Foreground(pink)
	label   = lipgloss.NewStyle().Foreground(dim)
	box     = lipgloss.NewStyle().Border(lipgloss.RoundedBorder()).BorderForeground(dim).Padding(0, 1)
	tokenSt = lipgloss.NewStyle().Bold(true).Foreground(lipgloss.Color("#ffffff")).Background(pink).Padding(0, 1)
)

type tickMsg time.Time
type loginDone struct{ err error }

type model struct {
	app    *app.App
	ctx    context.Context
	snap   app.Snapshot
	w, h   int
	status string

	loginCancel context.CancelFunc
	qrFor       string
	qrText      string
}

// Run shows the dashboard until the user quits or ctx ends.
func Run(ctx context.Context, a *app.App) error {
	// Fixed colours, so skip the terminal background query: some terminals and
	// multiplexers never answer it and the dashboard would start late or blank.
	lipgloss.SetColorProfile(termenv.TrueColor)
	lipgloss.SetHasDarkBackground(true)
	m := &model{app: a, ctx: ctx, snap: a.Snapshot()}
	p := tea.NewProgram(m, tea.WithAltScreen(), tea.WithContext(ctx))
	_, err := p.Run()
	if m.loginCancel != nil {
		m.loginCancel()
	}
	if err != nil && ctx.Err() != nil {
		return nil
	}
	return err
}

func (m *model) Init() tea.Cmd { return tick() }

func tick() tea.Cmd {
	return tea.Tick(500*time.Millisecond, func(t time.Time) tea.Msg { return tickMsg(t) })
}

func (m *model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.w, m.h = msg.Width, msg.Height
	case tickMsg:
		m.snap = m.app.Snapshot()
		return m, tick()
	case loginDone:
		m.loginCancel = nil
		if msg.err != nil {
			m.status = "login: " + msg.err.Error()
		} else {
			m.status = "signed in"
		}
		m.snap = m.app.Snapshot()
	case tea.KeyMsg:
		switch msg.String() {
		case "q", "ctrl+c":
			return m, tea.Quit
		case "l":
			if m.loginCancel != nil { // second press cancels
				m.loginCancel()
				m.loginCancel = nil
				m.status = "login cancelled"
				return m, nil
			}
			ctx, cancel := context.WithCancel(m.ctx)
			m.loginCancel = cancel
			m.status = ""
			return m, func() tea.Msg { return loginDone{m.app.Login(ctx, nil)} }
		case "o":
			if err := m.app.Logout(); err != nil {
				m.status = "logout: " + err.Error()
			} else {
				m.status = "signed out"
			}
		case "t":
			if tok, err := m.app.NewToken(); err != nil {
				m.status = "token: " + err.Error()
			} else {
				m.status = "new token " + tok + " — the PSP must be updated"
			}
		}
		m.snap = m.app.Snapshot()
	}
	return m, nil
}

func (m *model) View() string {
	s := m.snap
	var left strings.Builder
	left.WriteString(title.Render("Connect the PSP") + "\n")
	for _, a := range s.Addrs {
		left.WriteString(label.Render("address ") + a + "\n")
	}
	if len(s.Addrs) == 0 {
		left.WriteString(lipgloss.NewStyle().Foreground(warn).Render("no LAN address found") + "\n")
	}
	left.WriteString(label.Render("name    ") + fmt.Sprintf("%s:%d", s.MDNSName, s.Port) + "\n")
	left.WriteString(label.Render("token   ") + tokenSt.Render(s.Token) + "\n")
	for _, l := range []string{s.MDNSLine, s.BeaconLine} {
		if l != "" {
			left.WriteString(label.Render(l) + "\n")
		}
	}

	var acct strings.Builder
	acct.WriteString(title.Render("Bilibili") + "\n")
	if s.LoggedIn {
		who := s.Uname
		if who == "" {
			who = "account"
		}
		acct.WriteString(lipgloss.NewStyle().Foreground(good).Render("signed in") + fmt.Sprintf(" as %s (uid %d)\n", who, s.Mid))
	} else {
		acct.WriteString(lipgloss.NewStyle().Foreground(warn).Render("not signed in") + " — guest quality\n")
	}
	if s.FFmpeg.OK {
		acct.WriteString(label.Render("ffmpeg  ") + s.FFmpeg.Version + "\n")
	} else {
		acct.WriteString(lipgloss.NewStyle().Foreground(warn).Render("ffmpeg  "+s.FFmpeg.Problem) + "\n")
	}
	if s.ReadOnly {
		acct.WriteString(label.Render("read-only mode") + "\n")
	}

	top := lipgloss.JoinHorizontal(lipgloss.Top, box.Render(left.String()), " ", box.Render(acct.String()))

	var body string
	if s.Login.Active && s.Login.Content != "" {
		if m.qrFor != s.Login.Content {
			m.qrFor = s.Login.Content
			m.qrText, _ = qr.Render(s.Login.Content)
		}
		body = box.Render(title.Render("Sign in") + "  " + s.Login.Message + "\n" + strings.TrimRight(m.qrText, "\n"))
	} else {
		body = m.streamsView(s) + "\n" + m.logView(s)
	}
	foot := label.Render("l sign in/cancel · o sign out · t new token · q quit")
	if m.status != "" {
		foot = lipgloss.NewStyle().Foreground(warn).Render(m.status) + "   " + foot
	}
	return title.Render(fmt.Sprintf("WiliMini server %s — %s", s.Version, s.Name)) + "\n" + top + "\n" + body + "\n" + foot
}

func (m *model) streamsView(s app.Snapshot) string {
	var b strings.Builder
	b.WriteString(title.Render(fmt.Sprintf("Streams (%d)", len(s.Streams))) + "\n")
	if len(s.Streams) == 0 {
		b.WriteString(label.Render("none playing") + "\n")
	}
	for _, st := range s.Streams {
		fmt.Fprintf(&b, "#%d %s  %s  %s  %d KiB\n", st.ID, st.Remote, st.Desc, time.Since(st.Started).Truncate(time.Second), st.Bytes/1024)
	}
	return b.String()
}

func (m *model) logView(s app.Snapshot) string {
	rows := 8
	if m.h > 0 {
		rows = max(4, m.h-18)
	}
	logs := s.Logs
	if len(logs) > rows {
		logs = logs[len(logs)-rows:]
	}
	var b strings.Builder
	b.WriteString(title.Render("Log") + "\n")
	for _, l := range logs {
		st := lipgloss.NewStyle()
		switch l.Kind {
		case "warn", "error":
			st = st.Foreground(warn)
		default:
			st = st.Foreground(dim)
		}
		line := fmt.Sprintf("%s %-6s %s", l.Time.Format("15:04:05"), l.Kind, l.Message)
		if m.w > 8 && len(line) > m.w-2 {
			line = line[:m.w-2]
		}
		b.WriteString(st.Render(line) + "\n")
	}
	return b.String()
}
