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

Do not use PSPLink `scrshot`: its partition-4 allocation overlaps ME firmware.
Do not use VRAM `savemem` for routine screenshots; it has stalled USB transfers.
Emulator screenshots do not replace hardware validation.

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

## Playback freezes and HOME exit diagnostics

While playback is active, the exit-callback thread samples `PLAYER health` every
2 seconds without taking playback locks. `operation` and `age_ms` identify the
last playback call; RGB and audio produced/consumed counters show whether workers
still advance. Samples are approximate, not a synchronized snapshot. An increasing
age can also mean a legitimate network wait or pause; compare consecutive samples.
These INFO lines use the existing asynchronous sink and require Save logs and
results to be enabled for files. They do not write each frame to the Memory Stick.

`CLIENT home_exit callback_enter`, `stop_begin`/`stop_end`, and `PLAYER stop`
markers bracket HOME cancellation. Cleanup logs bracket ME finish/shutdown, RGB
draining, audio thread joins and controller joins. The last begin without its end
narrows the blocked call. The existing 5 second exit limit still starts after
`psp_video_stop()` returns. This instrumentation does not change that policy.
A stalled logger, unscheduled thread or system exit can still lose the last lines;
missing lines alone do not prove which call blocked. Save `logs/wili.log`, the
current run directory and `build.json` before another launch.
