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
| Video | DASH ranges decoded by the ME decoder | one FLV: H.264 Main profile (CAVLC) + AAC, always fitted into 480×272, **decoded by the firmware's sceMpeg** |
| Seeking | in place | a new stream is requested at the new time (`t=`) while the player keeps running (see *Seeking* below) |
| Quality menu | Bilibili qualities | none; the source tier is *Stream source* in Settings |
| Live rooms | direct | API via the server, media still direct |

Settings (`settings.json`): `stream_server`, `server_host`, `server_port`,
`server_token` (plain text, like the Wi-Fi profile), `stream_quality` (the source tier, a
number: 16, 32, 64, 74, 80, 112, 116 or 120; default 32), `stream_fps` (`auto|30`; `auto` allows
up to 60 fps), `stream_preset` (the bitrate: `performance` 700 k, `balanced` 1000 k, `quality`
1500 k), `stream_profile` (`auto|cavlc|cabac`). The picture is always 480×272; only the source,
the frame-rate cap and the bitrate vary. Files from before these settings load with the old
behaviour (480P source, frame rate left to the source).

### Source quality and frame rate

*Stream source* is the Bilibili tier the PC downloads, so the 60 fps tiers and 4K are
available even though the PSP shows 480×272. The server decodes the source with ffmpeg and
scales it down, so a higher tier costs the PC more CPU, not the PSP or the Wi-Fi.

| Setting | `qn` | Notes |
|---|---|---|
| 360P / 480P / 720P | 16 / 32 / 64 | |
| 720P60 | 74 | 60 fps; needs a signed-in account |
| 1080P / 1080P+ | 80 / 112 | 1080P+ is the high-bitrate tier |
| 1080P60 | 116 | needs a signed-in account (VIP on some videos) |
| 4K | 120 | needs a signed-in account (VIP); often offered only as HEVC |

The ids come from Bilibili's public API documentation and have not been checked against the
live service from the development environment. A tier the account or video cannot get falls
back to the best one below it (logged by the server as `source qn=…`). H.264 is preferred at
a tier; HEVC is used when that is all Bilibili offers. AV1 is not used.

The output frame rate is `min(source rate, cap)`; ffmpeg never adds frames. *Auto* caps at
60 fps, *30 fps* halves the 60 fps tiers. Level 3.0 allows 480×272 at 60 fps, but the sceMpeg
decoder's real-time limit at 60 fps and the Wi-Fi bitrate it needs are only known from a
device; pick a bitrate with that in mind.

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
`profile` (`auto|cavlc|cabac`; `auto` is `cavlc`, both are Main profile), `qn` (source tier,
16 to 120), `fps` (frame-rate cap, 5 to 60, default 30), `vb`, `ab` (kbps). `w` and `h` are
accepted and ignored: the picture is always fitted into 480×272. Output is
`video/x-flv` without a duration, timestamps starting at 0, Main profile level 3.0, no
B-frames, one reference frame, a two-second GOP (counted at the frame rate actually produced),
and a picture padded with black to a
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
- `decoder`: `PspVideoOptions.restart_seek / timeline_offset_ms / timeline_duration_ms / reopen`.

## Seeking

The stream has no index, so the server cannot seek inside it: a seek asks for a new stream
at `t=`. What the client does with it depends on `PspVideoOptions.reopen`:

- **With `reopen`** (what the client sets for server streams on the sceMpeg backend): the
  seek does not leave `psp_video_play`. The player stops reading, closes its demuxer, audio
  and sceMpeg objects (`sce_mpeg_run_segment`), asks the client for a new stream at the target
  (`App::reopenServerPlayback`), and starts the next segment on it. The controls, OSD,
  presenter and the results folder stay up; the OSD shows the target position and the
  buffering state. The wait is the server starting ffmpeg and sending the first frames.
- **Without it, or if it fails** (server busy or unreachable): the session ends with
  `seek_requested` set and the caller prepares the stream again, as before.

Pressing back or HOME while the new stream is starting cancels it. This is implemented
and has been read through, but it has **not been compiled or run**: it was written without a
PSP toolchain. Real-device behaviour of the sceMpeg teardown and rebuild on every seek is
unverified.

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
- sceMpeg is the third **ME backend** (Settings → ME backend: MIST, Custom Core, sceMpeg). It owns
  the Media Engine, so with it selected the loader starts **no ME bridge or core at all** and the
  client never calls into one; changing it needs a restart of WiliMini. Scaling, colour
  conversion, audio and drawing run on the CPU and GE.
- With sceMpeg selected, every video and live room plays through it, whether or not *Stream
  server* is on (as under PPSSPP). Direct Bilibili video is whatever the firmware's decoder
  accepts; the server's transcode is what is shaped for it. Turning *Stream server* on selects
  sceMpeg (turning the server off, or choosing another ME backend, goes back), and the server
  only plays after the restart.
- The bitrates (700 k, 1000 k, 1500 k) are starting points and need tuning on a PSP.
- Bilibili may throttle or change its API; the proxy passes JSON through, so most changes
  need no client update, but WBI signing and login live on the server.
- Cookie refresh is not implemented: when the session expires, sign in again.
- Use only on a trusted LAN. Traffic is plain HTTP and the token is the only gate.
