# Custom Core backend validation

This opt-in backend uses mcidclan's pinned Custom Core bootstrap and cache helpers
under the upstream MIT license. The existing kernel bridge hosts the task loop;
it does not install a separate kcall plugin or patch the firmware syscall table.
FFmpeg decoding, rendering kernels, media settings and RGB queues stay shared
with the MIST build. This is not a decoder performance experiment.

Build the diagnostic client through the normal entry point. Every package carries
both the MIST and Custom Core bridge PRXs; the backend is chosen at launch:

```sh
make package BUILD_DIR=build/custom-core-test \
  CMAKE_ARGS='-DBILI_DEVICE_PLAYBACK_TEST=ON -DBILI_DIST_DIR=build/custom-core-test-dist'
sh tools/test-me-bridge.sh
```

Select Custom Core by placing `"me_backend":"custom-core"` in the package's
`settings.json` before launch (the Settings screen writes the same field in the
normal client; changing it there needs a restart). Confirm `LOADER me_backend=custom-core`
in the startup log and `me_backend=custom-core` in `diagnostics.txt`.

Place a local 640x360 H.264/stereo AAC MP4 named `me-test.mp4` in the generated
`build/custom-core-test-dist/PSP/GAME/WiliMini/` directory, with this
`playback-test.json`:

```json
{
  "video": "me-test.mp4",
  "mode": "file",
  "repeat": 3,
  "buffers": [2, 4, 8]
}
```

Follow [the PSPLink procedure](debug.md). Save the thread and memory baseline,
copy the entire generated WiliMini directory to `ms0:/PSP/GAME/WiliMini/`, then
launch `./build/custom-core-test/loader/bootstrap.prx` from `host0:/` in pspsh.
Confirm the selected `LOADER root=` and extended client profile. Do not launch
the client PRX directly. Save existing device results before another experiment.

For each playback require `core_ready result=0`, `core_probe result=0` with
`a=1 b=4d450001`, completed video tasks, nonzero presented frames, successful EOF,
and successful `core_stop`/`restored` events. `wait_done` and `wait_timeout` use
`a` for the core state (1 ready, 2 running), `b` for job done and `c` for progress.
The probe is an operation-zero task and does not touch video buffers or LUTs.
The mailbox completion flag is published only after the C worker returns.

Save `playback-test-result.json`, all `results/run-XXX/` files, `build.json`,
the media hash and host/device logs. Exit the diagnostic client normally with
START, compare `thlist` with the baseline and confirm every application worker
has gone. A responsive PSPLink shell alone does not verify ME recovery.
Repeat a launch only after evidence from the previous run has been saved.

The backend defers power-switch suspend while it owns ME, then unlocks after
restoring the saved firmware bootstrap. Suspend context save/restore is not
implemented. A timeout retains possibly live buffers and poisons initialization;
fully reboot into PSPLink before further hardware experiments.

Host tests simulate the mailbox to check repeated playback, probe completion,
boot/task/stop failures and retained buffer ownership. They do not run the ME,
validate SRAM/cache/reset instructions, or prove PSP-1000 compatibility.

## Recorded checks (2026-10-31)

- Custom Core diagnostic/normal packages and the MIST normal package built with
  `make package`. Export symbols and Custom Core bootstrap section were checked
  in the generated bridge ELF; the package manifest hashes were verified.
- Both bridge host tests passed with ASan/UBSan. The RGB queue test also passed
  with ASan/UBSan. The broader memory-profile script could not run because its
  existing loader test references an absent `loader_profile.h`.
- The existing ME-enabled PPSSPPHeadless ran the Slim profile selected by
  `BILI_HEADLESS_MODEL=3000` (SDK model ID 1), with the extracted ME firmware.
  Three consecutive 8-second 640x360 H.264/stereo AAC plays using 2/4/8 RGB slots
  each completed 120 ME tasks and presented 120 frames, with EOF and error code
  zero. Every play recorded successful boot, probe, stop and restoration.
  Summary `frames=90` excludes the 30 warmup frames; the test result and
  `rgb_presented` include all 120. An injected START at 36 seconds exited the
  diagnostic client normally, without the runner timeout.
- First-play heap occupancy grew by 208 bytes; the second and third plays had
  identical heap occupancy before/after. This is not evidence of complete
  absence of leaks. The sample SHA-256 was
  `ebfc5ca66438b0a1bc965aa7d9880b21240fc514dcc0e1eb2aa9de407838243c`.
- The first emulator attempt exposed an invalid mailbox alias when the bridge
  was loaded at a physical kernel address. The runtime now normalizes physical,
  cached and uncached kernel pointers to KSEG1; the host test covers all three.

Local emulator evidence is under `results/custom-core-psp3000/` and
`build/custom-core-emulator-03/memstick/PSP/GAME/WiliMini/`.

## PSP-3000 hardware run (2026-10-31)

The connected device reported model ID 2 and devkit `06060010` (6.60), running
at CPU/bus 333/166 MHz under PSPLink. The baseline contained 18 threads.
At the user's request, the test ran entirely over USBHostFS without copying
files to the memory stick. A scratch loader under `build/custom-core-device/`
uses the regular loader's network/bridge/client initialization but selects
`host0:/dist/PSP/GAME/WiliMini` explicitly. A temporary host filesystem symlink
points that directory at the complete diagnostic package. No host preference
macro or change to the normal client's storage selection is required.

The same sample and Release configuration as above completed three consecutive
plays with 2/4/8 RGB slots. Each returned code 0, EOF, 120 completed ME tasks and
120 presented frames. All three boot probes, stops and firmware restoration
steps succeeded. First-play heap occupancy grew by 208 bytes; the second and
third plays had equal before/after heap occupancy. The test exercises local
media, not Bilibili network playback or long-duration performance.

Logs, manifests, media hash, configuration and per-play results are saved under
`results/custom-core-psp3000/device-run-001/`. Playback workers exited after the
third play; the test client's `user_main` and callback thread remain while it
waits for START. Normal exit/thread comparison is still pending.
PSP-1000 compatibility and sleep/resume remain unverified.
