# PSPLink device checks

Run `make package` from the repository root after each change.
Before each test, copy the entire `dist/PSP/GAME/WiliMini/` directory to
`ms0:/PSP/GAME/WiliMini/` or PSP Go internal storage at `ef0:/PSP/GAME/WiliMini/`.

The PSP must already be running PSPLink.
Start `usbhostfs_pc -vv -b 11000 "$PWD"` from the repository root.
Connect with `pspsh -p 11000` from a second host terminal.

Before each experiment, save `thlist` and `meminfo` output from `pspsh`.
In `pspsh`, run `cd host0:/`, then `./build/client/loader/bootstrap.prx`.
The loader selects a complete package from `ms0:`, then `ef0:`, then
`host0:/dist/PSP/GAME/WiliMini`.
Save the `LOADER` and `CLIENT` startup lines.

## Log format

Client and loader logs come from `platform/include/wili_log.h`. Each line is
`[sec.ms] L CHANNEL message`, for example `[   1.204] I MEM ui_init heap_used=...`.
`L` is `E`, `W`, `I`, `D` or `V`. The channel name stays the first word of the
message, so searches for `LOADER root=`, `CLIENT root=` or `MEM rgb_start` still work.

- Channels: `CLIENT`, `LOADER`, `NET`, `AUTH`, `UI`, `PLAYER`, `ME`, `MEM`, `IO`.
- Every channel defaults to `INFO`. The client reads two optional `settings.json` keys
  at startup (`App::init`) and applies them immediately; the loader does not read them.
  - `log_level`: `error`, `warn`, `info` (default) or `debug`. Settings > Log level
    cycles it and takes effect at once. `debug` is the most detail a production build
    can print, because `verbose` is compiled out.
  - `log_channels`: optional per-channel overrides, for example
    `"net=debug,player=warn"`, applied after `log_level`. Edit it by hand (no UI);
    letters, `=`, `,` and spaces only, up to 96 characters. It is kept when settings are saved.
  An unknown `log_level` or a bad `log_channels` makes the whole file unreadable, like other
  invalid settings: defaults are used and the file is not rewritten.
- `WILI_LOG_MAX_LEVEL` (default `DEBUG`) removes higher levels at compile time.
- **Log files and playback results are off by default.** Nothing about logging or playback
  results is written to the Memory Stick unless Settings > Save logs and results is on
  (`"log_files": true` in `settings.json`). Console output is unaffected. The switch takes
  effect at once. It controls:
  - `logs/wili.log`: the whole session, kept as `wili.log` and `wili.log.1` of about 256 KiB
    each at most. New sessions append to it.
  - `results/run-NNN/log.txt`: the lines logged during one playback, on both backends.
  - the newest log lines in `last-error.txt` (between `recent_log_begin` and
    `recent_log_end`); the error fields themselves are always written.
  - the `results/run-NNN/` folders themselves; see "Playback result folders" below.
- **How the files are written.** Lines go into a 64-line RAM ring. A low-priority writer
  thread copies them to the files, so the UI, video and network threads never wait on the
  Memory Stick. Lines are written at most about once a second, an ERROR line immediately, so
  a crash or hang loses at most the last second or so. If lines are logged faster than they
  can be written, the oldest are overwritten and the file gets `log_lines_dropped=N`. A
  failed write (card full or removed) stops that file quietly. At a normal exit (START) the
  thread flushes and stops, waiting at most 3 seconds.
  Lines longer than 384 bytes are truncated. The controlled playback test enables files per
  run with `"log_files": true` in `playback-test.json`.
- Converted so far: `main.cpp`, the loader, `bilibili.c` network/stream reports,
  and the player (`PLAYER`, `MEM`, `VIDEO` lines). `HTTP timing`, `STREAM` and
  `LIVE` lines now appear as `NET` lines. Not converted: `memory_profile.c` (`ALLOC`,
  profile builds only) and `decoder/me/bridge.c`, a kernel module that cannot
  link the user-mode logger.

Run `sh tools/test-logging.sh` for the host check. It does not cover the PSP build.
Check `root=`, the selected profile and the compiled heap/defaults.

Run one experiment at a time. Save its logs and result files, exit normally
with START, and compare `thlist` against the baseline. All application workers
must exit and PSPLink must remain responsive. A timed-out query does not prove
a crash: reconnect and inspect the device before another launch.

Use `reset` only for fault recovery after collecting available evidence. An
unresponsive PSPLink connection or persistent ME failure requires a manual
reboot into PSPLink. A responsive shell does not establish that ME recovered.

PSPLink `scrshot` is allowed only when no video is playing or about to play
(menus, settings, lists). Do not use it during or around playback: with the ME
decoder active it crashes the ME. Never use VRAM `savemem` for screenshots; it
also crashes the ME and has stalled USB transfers. Emulator screenshots do not
replace hardware validation.

## Pressing buttons from pspsh

`make psplink` builds PSPLink from the pinned `third_party/psplinkusb` submodule with
`patches/psplink-buttons.patch` applied (the submodule stays untouched; output is in
`build/psplink/`). The patch adds a `btn` command that injects button presses by hooking
the `sceCtrl` read/peek syscalls, so it does not depend on RemoteJoy or on firmware code
layout. Boot this PSPLink build on the PSP instead of the stock one, then in `pspsh`:

```text
btn down
btn -t 300 cross
btn up up cross
btn l+r
```

Buttons: `select start up down left right l r triangle circle cross square`. Join names
with `+` to press together; `-t` sets the hold time in ms (default 150). The command
blocks until the presses finish and prints an error if the hooks cannot be installed.
After the presses it prints how many times each `sceCtrl` hook was called and merged a
button, which shows whether the application is reading the pad through the hooks.
The stray `Unknown command btn` line after an error comes from the host `pspsh`, whose
command table predates the patch; it is cosmetic.

UI navigation in the client: `circle` opens or enters, `cross` goes back. Back from
a page returns focus to the sidebar, where up/down choose the section and `circle`
enters it. On settings lists left/right change the value, so do not press them
while only exploring.

Confirmed working by the maintainer on a PSP-3000 with ARK-5 using the WiliMini
client. Other firmware and models are untested. Install the built
`build/psplink/psplink/psplink.prx` as `PSPLINK.PRX` in the PSPLink folder on the Memory
Stick (keep a backup of the original) and relaunch PSPLink; the loaded module cannot be
replaced in place. RemoteJoy's own hook fails on ARK-5 because it expects the old
`sceCtrl` instruction layout, so RemoteJoy cannot be used for button input there.

## Playback result folders

With Save logs and results on, each playback creates `results/run-NNN/` on the Memory Stick:
`summary.json`, `log.txt`, and for native playback `diagnostics.txt` and CSV files. With it
off, nothing under `results/` is created or written. Profiling runs (`diagnostic_profile`),
the benchmark runner and the lab build always save results, because that is their purpose.

- The number is the highest existing one plus one, so numbers are not reused when old
  folders are deleted. Names are matched ignoring case because FAT lists them in uppercase.
- Only the newest 20 runs are kept (`WILI_RESULTS_KEEP`, 0 keeps all). The oldest are
  deleted when a new run starts, at most 8 per playback (`WILI_RESULTS_PRUNE_STEP`), so an
  old backlog drains over several playbacks instead of delaying one video.
- Runs with the diagnostic profile on are never pruned: save evidence from a hardware
  experiment before starting the next one, as above.
- If a numbered folder cannot be created, the run uses `results/latest` and overwrites it
  each time. Playback does not fail because its logs cannot be saved.
- Only folders named `run-NNN` are ever deleted; other files in `results/` are left alone.
- Other writes are not part of this switch: `settings.json`, the cover cache, session
  files, and `last-error.txt` when an error occurs.
