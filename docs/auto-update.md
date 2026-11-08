# Auto update and Codeberg mirror (issue #26)

## Status

| Milestone | State |
| --- | --- |
| 1. Release workflow publishes `WiliMini-<tag>.zip` + `SHA256SUMS` | done (`.github/workflows/build.yml`) |
| 2. Platform-independent core | done, host-tested: `update_core.*` (versions, release JSON, swap planning), `update_package.*` (SHA-256, streaming zip, Range resume rules) |
| 3. Network and download | logic done and host-tested with a fake transport (`update_install.*`: check against the chosen source, resumable download, SHA-256 verify, staging into `pending/`). The libcurl transport (`update_curl.*`) is written but only syntax-checked against a stub header |
| 4. Loader journaled swap + rollback | done and host-tested with power loss injected at every file operation (`platform/src/wili_update_swap.c`, 144 crash points); wired into `client/bootstrap.c`, committed from `client/main.cpp` after the UI is up. **Never compiled for PSP, never run on a device** |
| 5. Settings UI | not started. Needs a Settings row, a page (release notes, progress, confirm), localization keys, battery and free-space checks, and a worker thread |
| 6. Release checklist | below |
| 7. Signed releases | later |

## What is and is not in the PSP build

Only the swap (`wili_update_swap.c`, loader + client commit) is in `CMakeLists.txt`.
`update_core`, `update_package`, `update_install` and `update_curl` are not
added yet because nothing calls them and they have never been built with the
PSP toolchain (zlib linking included). Add them together with the UI.
No PSP toolchain was available while writing this, so the loader/client edits
and the CMake change are unverified; build with `make package` before merging
anything that relies on them.

Host checks: `sh tools/test-host.sh` (needs libcurl headers for the existing
transport test) or `sh tools/test-update-package.sh` for the updater alone.

## How an update works

1. Check: `GET /releases/latest` on the release source chosen on the update page
   (Codeberg by default; GitHub or Codeberg can be selected with ←→ and is saved
   in `settings.json` as `update_channel`). Existing saved selections are preserved.
   There is no automatic fallback: if a source is unreachable the page says so and
   the user can switch. Prereleases, drafts and downgrades are ignored.
2. Download `WiliMini-<tag>.zip` to `.part` with Range resume; verify SHA-256
   (API digest or `SHA256SUMS`); delete on mismatch.
3. Stage: verify again, extract to `<root>/pending/`, write `pending/MANIFEST` last.
   Packages without `EBOOT.PBP` and `wili_mini.prx`, or containing `settings.json`,
   unsafe paths or `.bak` names, are refused.
4. Next launch, the loader runs the swap: back up old files as `*.bak`, move new
   files in (`EBOOT.PBP` last), then 3 trial boots. The client calls
   `wili_swap_commit()` once its UI is up; if it never does, the 4th launch
   restores the `.bak` files and writes `update.failed`. Files not in the
   manifest (settings, themes) are never touched.

Decisions taken for the open questions in the issue: manifest-based (not
whole-folder) replacement; `SHA256SUMS` from the release is enough for a first
version, signing stays milestone 7.

## Release assets

A `v*` tag publishes `WiliMini-<tag>.zip`, `WiliMini-<tag>.zip.sha256` and
`SHA256SUMS`. The updater looks for the asset named exactly `WiliMini-<tag>.zip`
and verifies it against the API `digest` field or the `SHA256SUMS` entry.
`parseRelease` rejects drafts, prereleases, non-HTTPS asset URLs and releases
with nothing to verify against. The same payload shape is used by GitHub and
Codeberg (Forgejo), so the client can later fall back to
`https://codeberg.org/api/v1/repos/<owner>/<repo>/releases/latest`.

## Codeberg mirror

`.github/workflows/mirror-codeberg.yml` pushes `main` and `v*` tags to Codeberg
(fast-forward only, never forced) and, when a GitHub release is published,
copies its notes and assets through `scripts/codeberg-release-sync.sh`
(idempotent). Users can then clone or download from Codeberg when GitHub is
unreachable.

One-time setup:

1. Create an empty repository on Codeberg (no README/licence commit).
2. Create a Codeberg access token with `write:repository`.
3. In the GitHub repository settings add the variable `CODEBERG_REPO`
   (for example `wilimini/WiliMini`) and the secret `CODEBERG_TOKEN`.

Without both, the workflow skips with a notice. The workflow is untested
against a live Codeberg repository; run it once via *Actions → Run workflow*
and check the result before relying on it. Submodules are stored as pointers,
so they still resolve to their original hosts.

## Release checklist

1. Merge to `main`; tag `vX.Y.Z` on a commit already in `main`.
2. Confirm the release has the zip, `.sha256` and `SHA256SUMS`.
3. Confirm the mirror workflow ran and the Codeberg release lists the same assets.
