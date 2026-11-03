# wili-server

Streaming server for WiliMini on the PSP. See [docs/streaming-server.md](../docs/streaming-server.md)
for the protocol and PSP setup.

```sh
go build -o wili-server ./cmd/wili-server   # Go 1.24+; ffmpeg 5.1+ with libx264 must be on PATH
./wili-server            # headless; prints addresses and the pairing token
./wili-server --tui      # terminal dashboard (l: sign in, o: sign out, t: new token)
./wili-server login      # QR sign-in in the terminal
./wili-server token      # print the token (--new replaces it)
```

Flags: `--port`, `--name`, `--listen`, `--read-only`, `--max-streams`, `--ffmpeg`, `--no-discovery`,
`--dir` (config directory; default per-user, or `WILI_SERVER_DIR`). Files: `config.json` and
`session.json` (the Bilibili cookies, owner-only). Never commit or share them.

`go test ./...` runs the unit tests; those that need ffmpeg skip without it.
