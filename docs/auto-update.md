# Auto update and Codeberg mirror (issue #26)

## Status

| Milestone | State |
| --- | --- |
| 1. Release workflow publishes `WiliMini-<tag>.zip` + `SHA256SUMS` | done (`.github/workflows/build.yml`) |
| 2. Platform-independent core | partly done: version compare, release JSON parsing, checksum lookup, archive path check, swap journal (`client/services/update_core.*`, `tests/update_core_test.cpp`). Zip extraction is not written yet. |
| 3. Network and download | not started |
| 4. Loader journaled swap + rollback | not started (needs hardware testing) |
| 5. Settings UI | not started |
| 6. Release checklist | below |
| 7. Signed releases | later |

Nothing in the client calls the core yet, so the shipped app behaves as before.
Only host checks (`sh tools/test-host.sh`) have been run; no PSP, PPSSPP or
hardware test of updating exists.

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
