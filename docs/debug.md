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
- The last 64 lines are kept in RAM and written to files at fixed points. Nothing is
  written as it happens, so a crash or hang loses the lines not yet saved:
  - `results/run-NNN/log.txt`: the lines logged during one playback, written when it
    ends, on both the native and sceMpeg backends. If more than 64 lines were logged it
    starts with `log_lines_overwritten=N`.
  - `logs/last-run.log`: the newest lines of the whole session, written on a normal quit
    (START). It starts at the client's first line; the loader keeps no ring buffer.
  - `last-error.txt`: the newest lines between `recent_log_begin` and `recent_log_end`,
    written when an error is recorded.
  Lines longer than 384 bytes are truncated.
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

Each playback creates `results/run-NNN/` on the Memory Stick (`summary.json`, `log.txt`,
and for native playback `diagnostics.txt` and CSV files).

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
