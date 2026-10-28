# Repository Guidelines

## Project Context

WiliMini is a native Bilibili client for the Sony PlayStation Portable (PSP).
Networking, UI rendering, media downloads, and audio/video decoding run on the
device; this is not a streaming frontend for a separate computer. The client
uses C++17, with C and assembly in the decoder and platform integration.

Read `README.md` before changing build or runtime behavior. Consult
`decoder/README.md` for playback work, `tools/README.md` for validation tools,
and the relevant documents under `docs/` for subsystem context. Check that
referenced files exist rather than assuming every documentation link is current.

## Repository Layout

- `client/`: application, UI, Bilibili services, networking, and playback control.
- `decoder/`: FFmpeg player, audio processing, VFPU code, and Media Engine code.
- `cmake/` and root `CMakeLists.txt`: dependency builds, client targets, and packaging.
- `scripts/`: asset generators and build-manifest generation.
- `tools/`: PPSSPP replay, screenshots, device utilities, and host-side checks.
- `tests/`: test support; inspect the checkout for available test sources.
- `patches/`: patches applied to external dependencies.
- `third_party/`: external dependencies, including pinned Media Engine submodules.
- `docs/`, `experiments/`, and `results/`: design notes, experiments, and evidence.
- `build/` and `dist/`: generated build and package output, not source directories.

Some dependency, experiment, and result directories may be absent or ignored in
a particular checkout. Do not treat their presence as guaranteed.

## Build Workflow

Run commands from the repository root. A PSP toolchain and PSPSDK are required.
Set `PSPDEV` to the local installation and add `$PSPDEV/bin` to `PATH`; do not
introduce new machine-specific absolute paths.

Initialize required submodules with `git submodule update --init --recursive`
when needed. Dependency builds download pinned FFmpeg and libcurl archives,
verify their checksums, and apply the repository patches.

Use the root Makefile as the normal build entry point:

```sh
make dependencies
make build
make package
```

`make` defaults to packaging. `make curl` and `make ffmpeg` build individual
dependencies; `make me` builds the client and Media Engine components;
`make assets` generates configured assets. Configure builds with `BUILD_DIR`,
`BUILD_TYPE`, `PARALLEL`, and `CMAKE_ARGS`.

Packages are written to `dist/PSP/GAME/WiliMini/` by default. Launch through
`EBOOT.PBP`, not directly through `wili_mini.prx`: the loader initializes required
network modules and the Media Engine bridge.

## Change Guidelines

- Keep patches focused and preserve unrelated local changes.
- Follow the naming, indentation, and error-handling patterns of adjacent code.
- Keep platform-independent service logic separate from PSP-specific integration.
- Respect PSP memory limits, thread behavior, and resource lifetimes. Avoid
  unnecessary allocations or blocking work in rendering and playback paths.
- Treat Media Engine synchronization, cache coherency, DMA, VFPU state, and
  buffer ownership as correctness concerns, not just performance details.
- Preserve cancellation, cleanup, and recovery behavior when changing network
  requests or playback.
- Keep experimental decoder settings and diagnostic build flags separate from
  production defaults. Do not change tuning constants without relevant evidence.
- Do not hand-edit generated assets or build manifests when a generator exists.
  Change the generator or source input and regenerate through the supported flow.
- Avoid modifying vendored code or updating submodule revisions unless required
  by the task. Preserve dependency checksums, patch application, and licenses.
- Never commit account cookies, QR-login sessions, tokens, or private device logs.

## PSP Hardware Debugging

Read `docs/debug.md` before running physical-device experiments. The device must
already be running PSPLink, and the host needs `usbhostfs_pc` and `pspsh`.
Build the client and deploy `me_bridge.prx` to
`ms0:/PSP/GAME/WiliMini/me_bridge.prx` before launching.

From the repository root, start USBHostFS and leave it running:

```sh
usbhostfs_pc -vv -b 11000 "$PWD"
```

Connect from a second host terminal:

```sh
pspsh -p 11000
```

Run these commands inside `pspsh`, not in the host shell:

```text
thlist
cd host0:/
./build/client/loader/bootstrap.prx
```

Use the loader rather than loading the client PRX directly. Adjust the loader
path if using a different build directory. Even when launching from `host0:`,
the loader loads the ME bridge from the memory-stick path listed above.

- Save the initial `thlist` output and run only one experiment at a time.
- Wait for results to be written, then exit the interactive client normally
  with START. Run `thlist` again and compare against the baseline: all application
  worker threads must exit, not just `user_main`, and PSPLink must remain responsive.
- Save logs, result files, and exit-check evidence before starting another run.
  A host query timeout alone does not prove a device crash; reconnect, inspect
  threads, and recover existing results before attempting another launch.
- Use `reset` inside `pspsh` only for fault recovery, after saving available
  evidence. It interrupts the experiment and may lose unwritten results; it is
  neither a normal exit nor evidence of a successful test. Reconnect with
  `pspsh -p 11000`, inspect `thlist`, and restore `host0:/` before relaunching.
- If PSPLink is unresponsive, manually reboot the PSP and re-enter PSPLink.
  Persistent ME failures also require a manual reboot; a responsive shell alone
  does not establish that ME state has recovered.
- Do not use PSPLink `scrshot` with the current ME decoder: its partition-4
  scratch allocation overlaps the ME firmware image. Do not use VRAM `savemem`
  as a routine screenshot method; it has stalled USB transfers. Emulator
  screenshots are a separate workflow and do not replace hardware validation.

## Validation and Reporting

Choose checks that exercise the changed subsystem; do not assume a universal
test command exists. Build the relevant target when the PSP toolchain and
dependencies are available. Use the PPSSPP workflows in `tools/README.md` for
UI replay, screenshots, and playback checks when their prerequisites exist.

Distinguish host checks, emulator results, and physical-device results. Passing
PPSSPP playback does not establish real PSP Media Engine behavior or hardware
performance. The README records testing on PSP-3000 with ARK-5; do not infer
PSP-1000 or other firmware compatibility from that coverage.

For performance changes, record the media sample, build configuration, execution
environment, and baseline before claiming an improvement. For UI changes,
inspect the affected screens and navigation in applicable themes and states.

Report the changes, checks actually performed, and any missing prerequisites or
remaining risks. Never claim a build, emulator run, or device test passed unless
it was performed. Update relevant documentation when workflows or behavior change.
