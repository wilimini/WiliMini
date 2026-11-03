# Stream server (wili-server)

WiliMini can use a PC on the same Wi-Fi as its Bilibili front end. The PC runs
`wili-server` (Go, in `server/`): it holds the Bilibili account, signs requests,
proxies the API over plain HTTP, and transcodes video with ffmpeg into something
the PSP decodes cheaply. The PSP does no TLS and never sees cookies in this mode.

Server mode is **off by default**. With it off, the PSP talks to Bilibili directly
exactly as before.

## Quick start

1. Install [ffmpeg](https://ffmpeg.org/) 5.1+ with libx264 on the PC (it must be on `PATH`).
2. Get the server: build it with `cd server && go build -o wili-server ./cmd/wili-server`
   (Go 1.24+), or use a CI artifact from the *Server* workflow.
3. Run `wili-server` (add `--tui` for a dashboard). It prints the addresses and the
   pairing **token**. Allow it through the PC's firewall (TCP 8787, UDP 5353 and 47612).
4. Optional: `wili-server login` shows a QR code to scan with the Bilibili app. Without
   an account you get guest quality. You can also sign in from the PSP's own login page.
5. On the PSP: *Settings → Stream server*
   - **Find server on network** (mDNS + UDP beacon) fills in the address, or use
     **Server address** (`192.168.1.20` or `192.168.1.20:8787`);
   - **Pairing token**: type the token the server printed;
   - turn **Stream server** on, then **Test connection**.

`wili-server token` prints the token, `wili-server token --new` replaces it (the PSP
must be updated), `wili-server logout` forgets the Bilibili account.

## What changes on the PSP

| | Direct (default) | Server mode |
|---|---|---|
| API calls | HTTPS to Bilibili, WBI signed on the PSP | `http://<pc>:8787/p/<host>/<path>`, signed on the PC |
| Account | cookies in `session.json` on the memory stick | on the PC; the PSP keeps nothing |
| Video | DASH ranges decoded by the ME decoder | one FLV: H.264 Main profile (CAVLC) + AAC, up to 480×272, **decoded by the firmware's sceMpeg** |
| Seeking | in place | the stream is reopened at the new time (`t=`), about a second of delay |
| Quality menu | Bilibili qualities | fixed by *Stream quality* in Settings |
| Live rooms | direct | API via the server, media still direct |

Settings (`settings.json`): `stream_server`, `server_host`, `server_port`,
`server_token` (plain text, like the Wi-Fi profile), `stream_preset`
(`performance|balanced|quality`), `stream_profile` (`auto|cavlc|cabac`).

## Protocol (version 1)

Plain HTTP. Every request carries the token as `?k=<token>` or `X-Wili-Token`;
failures are rate limited per client (10 wrong tokens per minute).

| Route | Purpose |
|---|---|
| `GET /wili/v1/hello` | `{proto, name, version, login:{state,mid,name}, ffmpeg:{ok,version,problem}, read_only, max_streams, streams}` |
| `GET /wili/v1/logout` | forget the account; replies in Bilibili's `{code,data}` envelope |
| `GET /wili/v1/stream` | transcoded FLV (see below) |
| `GET|POST|HEAD /p/<host>/<path>?<query>` | proxy to `https://<host>/<path>` |

`/p/` rules: hosts are allow-listed exactly (`api`, `app`, `api.vc`, `api.live`,
`passport`, `bangumi`, `s.search`, `message`, `www` on `bilibili.com`, and `*.hdslb.com`
for images); the PC adds the account cookies, fills `csrf`, signs paths containing
`/wbi/`, drops the PSP's `Cookie`/`Authorization` and the upstream `Set-Cookie`,
refuses redirects off the allow-list, and forwards `Accept-Encoding` untouched.
`--read-only` rejects POST.

`/wili/v1/stream` query: `bvid` (or `pgc=1` for bangumi/film), `cid`, `t` (seconds),
`profile` (`auto|cavlc|cabac`; `auto` is `cavlc`, both are Main profile), `w`, `h`, `fps`, `vb`, `ab` (kbps), `qn`. Output is
`video/x-flv` without a duration, timestamps starting at 0, Main profile level 3.0, no
B-frames, one reference frame, a two-second GOP, and a picture padded with black to a
multiple of 16 in both directions (480×270 becomes 480×272), which the PSP decoder wants. Errors are JSON `{"error": code, "message": text}` with a
status: `401 unauthorized`, `429 rate_limited`, `403 login_required`, `404 no_stream`,
`503 busy|ffmpeg_missing`, `502 upstream`, `400 bad_request`. A client that starts a
new stream replaces its previous one; the server caps simultaneous streams (default 2).

## Discovery

- **mDNS / DNS-SD**: `_wilimini._tcp.local`, SRV port, TXT `proto=`, `name=`, `ver=`.
  The PSP sends a classic query from an ephemeral port and gets a unicast reply.
- **UDP beacon** on port 47612: `WILIMINI-DISCOVER 1\n` is answered with
  `WILIMINI-SERVER 1 <http port> <name>\n`.

Both only list servers; the token is still required. The wire formats are pinned by
`tests/data/wili_mdns_*.hex`, which the Go responder generates and the C parser reads.

## Layout

- `server/`: the Go module (`cmd/wili-server`, `internal/{config,authstore,bili,proxy,stream,api,discovery,app,tui,qr}`).
- `client/stream_server.{h,c}`: URL rewriting and discovery packets (portable C, host-tested).
- `client/server_discovery.{h,c}`: PSP sockets for discovery.
- `client/bilibili.c` (`request()` rewrites URLs), `client/live_stream.c`
  (`bili_prepare_stream_server`), `client/playback.cpp`, `client/app_settings.cpp`.
- `decoder`: `PspVideoOptions.restart_seek / timeline_offset_ms / timeline_duration_ms`.

## Decoding with sceMpeg

Server streams are decoded by the firmware's sceMpeg library and by nothing else: there is no
fallback to the Media Engine decoder. It uses only the public, user-mode API (`pspmpeg.h`,
linked as `libpspmpeg`); the bootstrap loads the `AVCODEC` and `MPEGBASE` AV modules with
`sceUtilityLoadAvModule` before the client PRX is linked, so no kernel PRX is involved.

sceMpeg consumes an MPEG program stream, so `decoder/src/sce_mpeg_player.h` wraps each H.264
access unit that FFmpeg demuxes from the FLV into a PES packet, feeds 2048-byte packets through
a sceMpeg ring buffer behind a synthesized PSMF header, and gets RGB pictures back. Audio is
decoded by FFmpeg and keeps the existing clock; video frames are shown when the audio clock
reaches their time. The same file runs on PPSSPP's sceMpeg implementation.

Each access unit also has a PSMF private-stream-2 (`0xBF`, channel `01 E0 00`)
descriptor before its video PES packets. The descriptor supplies the complete Annex B
frame length, including any SPS/PPS prepended to a keyframe. Ordinary MPEG-PS video
packets alone are insufficient for the firmware's `sceMpegGetAvcAu`; it can return
`0x80618001` (`NO_DATA`) even when the video bytes have arrived. The client retries
with a refill when space is available, with a bounded retry count. Errors now identify
whether `sceMpegGetAvcAu` or `sceMpegAvcDecode` failed. This correction has passed host
packet checks and PSP packaging; playback on hardware still needs verification.

## Status and limits

- Tested on a PC: the Go packages (including real ffmpeg transcodes), the C module, preference
  parsing, and the PSP build. The server has **not** been run against the live Bilibili
  service from the development environment, and nothing here has been run on a real PSP.
- The sceMpeg path has not run on real hardware. Things only a PSP can confirm: that the
  firmware accepts the synthesized PSMF header and the PES wrapping, the `sceMpegCreate`
  arguments (512-pixel frame width, mode 0), and that Main/CAVLC at level 3.0 with one reference
  frame decodes. `profile=cabac` may be rejected by the firmware's decoder.
- Only server streams use sceMpeg. Direct playback still uses the ME decoder.
- Preset sizes and bitrates (384×216/700 k, 480×272/1000 k, 480×272/1500 k) are starting
  points and need tuning on a PSP.
- Bilibili may throttle or change its API; the proxy passes JSON through, so most changes
  need no client update, but WBI signing and login live on the server.
- Cookie refresh is not implemented: when the session expires, sign in again.
- Use only on a trusted LAN. Traffic is plain HTTP and the token is the only gate.
