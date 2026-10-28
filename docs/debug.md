# PSPLink device checks

Start `usbhostfs_pc -vv -b 11000 "$PWD"` from the repository root, then
connect with `pspsh -p 11000`. The PSP must already be running PSPLink.

Before each experiment, save `thlist` and `meminfo`. Copy the packaged
`me_bridge.prx` to `ms0:/PSP/GAME/WiliMini/me_bridge.prx` and launch
`./build/client/loader/bootstrap.prx` from `host0:/`. The loader uses the
packaged client on the memory stick, so copy that client before launching too.

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
