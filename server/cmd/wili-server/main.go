// Command wili-server lets a WiliMini PSP stream Bilibili through a PC: it
// holds the account, proxies the API and transcodes video with ffmpeg.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"github.com/wilimini/wilimini/server/internal/app"
	"github.com/wilimini/wilimini/server/internal/qr"
	"github.com/wilimini/wilimini/server/internal/tui"
)

// version is set at release time with -ldflags "-X main.version=...".
var version = "dev"

const usage = `wili-server — Bilibili streaming server for WiliMini on the PSP

Usage:
  wili-server [flags]              run the server (add --tui for the dashboard)
  wili-server login [flags]        sign in to Bilibili by scanning a QR code
  wili-server logout [flags]       forget the saved Bilibili account
  wili-server token [--new]        show (or replace) the PSP pairing token
  wili-server version

Flags for run/login/logout/token:
  --dir PATH         config directory (default: per-user config dir; env WILI_SERVER_DIR)

Flags for run:
  --tui              show the terminal dashboard
  --port N           HTTP port (default 8787, saved in config.json)
  --name TEXT        name shown on the PSP
  --listen HOST      bind one address instead of all
  --read-only        block likes, comments and other writes
  --max-streams N    simultaneous video streams (default 2)
  --ffmpeg PATH      ffmpeg binary (default: search PATH)
  --no-discovery     do not announce on the network
`

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

func run(args []string, stdout, stderr io.Writer) int {
	cmd := "run"
	if len(args) > 0 && len(args[0]) > 0 && args[0][0] != '-' {
		cmd, args = args[0], args[1:]
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	var err error
	switch cmd {
	case "run":
		err = serve(ctx, args, stdout, stderr)
	case "login":
		err = login(ctx, args, stdout, stderr)
	case "logout":
		err = logout(ctx, args, stdout, stderr)
	case "token":
		err = token(ctx, args, stdout, stderr)
	case "version":
		fmt.Fprintln(stdout, "wili-server", version)
	case "help":
		fmt.Fprint(stdout, usage)
	default:
		fmt.Fprintf(stderr, "unknown command %q\n\n%s", cmd, usage)
		return 2
	}
	switch {
	case err == nil:
		return 0
	case errors.Is(err, flag.ErrHelp):
		return 0
	case errors.Is(err, context.Canceled):
		return 130
	default:
		fmt.Fprintln(stderr, "error:", err)
		return 1
	}
}

func newFlags(name string, stderr io.Writer) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() { fmt.Fprint(stderr, usage) }
	return fs
}

func serve(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := newFlags("run", stderr)
	var o app.Options
	var useTUI, readOnly bool
	fs.StringVar(&o.Dir, "dir", "", "")
	fs.BoolVar(&useTUI, "tui", false, "")
	fs.IntVar(&o.Port, "port", 0, "")
	fs.StringVar(&o.Name, "name", "", "")
	fs.StringVar(&o.Listen, "listen", "", "")
	fs.BoolVar(&readOnly, "read-only", false, "")
	fs.IntVar(&o.MaxStreams, "max-streams", 0, "")
	fs.StringVar(&o.FFmpegPath, "ffmpeg", "", "")
	fs.BoolVar(&o.NoDiscovery, "no-discovery", false, "")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if readOnly {
		o.ReadOnly = &readOnly
	}
	o.Version = version
	if useTUI {
		if f, ok := stdout.(*os.File); !ok || !isTerminal(f) {
			return errors.New("--tui needs an interactive terminal; run without it for a headless server")
		}
	} else {
		o.LogWriter = stderr
	}
	a, err := app.New(ctx, o)
	if err != nil {
		return err
	}
	if err := a.Start(ctx); err != nil {
		return err
	}
	if !useTUI {
		fmt.Fprint(stdout, a.Snapshot().Banner())
		fmt.Fprintln(stdout, "\nPress Ctrl+C to stop.")
		<-ctx.Done()
		fmt.Fprintln(stderr, "stopping")
		return nil
	}
	return tui.Run(ctx, a)
}

func login(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := newFlags("login", stderr)
	var o app.Options
	fs.StringVar(&o.Dir, "dir", "", "")
	if err := fs.Parse(args); err != nil {
		return err
	}
	o.Version = version
	a, err := app.New(ctx, o)
	if err != nil {
		return err
	}
	var shown string
	err = a.Login(ctx, func(v app.LoginView) {
		if v.Content != "" && v.Content != shown {
			shown = v.Content
			if text, qerr := qr.Render(v.Content); qerr == nil {
				fmt.Fprint(stdout, "\nScan with the Bilibili app:\n\n", text, "\n")
			}
		}
		if v.Message != "" {
			fmt.Fprintln(stdout, v.Message)
		}
	})
	if err != nil {
		return err
	}
	fmt.Fprintln(stdout, "Saved. Start the server with: wili-server")
	return nil
}

func logout(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := newFlags("logout", stderr)
	var o app.Options
	fs.StringVar(&o.Dir, "dir", "", "")
	if err := fs.Parse(args); err != nil {
		return err
	}
	a, err := app.New(ctx, o)
	if err != nil {
		return err
	}
	if err := a.Logout(); err != nil {
		return err
	}
	fmt.Fprintln(stdout, "Signed out; the saved session was deleted.")
	return nil
}

func token(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	fs := newFlags("token", stderr)
	var o app.Options
	var renew bool
	fs.StringVar(&o.Dir, "dir", "", "")
	fs.BoolVar(&renew, "new", false, "")
	if err := fs.Parse(args); err != nil {
		return err
	}
	a, err := app.New(ctx, o)
	if err != nil {
		return err
	}
	if renew {
		tok, err := a.NewToken()
		if err != nil {
			return err
		}
		fmt.Fprintln(stdout, tok)
		return nil
	}
	fmt.Fprintln(stdout, a.Token())
	return nil
}

func isTerminal(f *os.File) bool {
	info, err := f.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}
