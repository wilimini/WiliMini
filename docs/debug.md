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
- Every channel defaults to `INFO`. `wili_log_configure("info,net=debug")` changes
  levels at runtime; a settings.json hook for it is not wired up yet.
- `WILI_LOG_MAX_LEVEL` (default `DEBUG`) removes higher levels at compile time.
- The last 64 lines are kept in RAM and `wili_log_dump()` writes them to a file.
  Nothing calls it yet, so crash and error files do not include them today.
- Only `main.cpp` and the loader use it so far. Other modules, including the
  decoder, still print with `printf` and have no timestamps or levels.

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
