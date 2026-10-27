# Client services

Network, search and authentication use typed results. The existing Wiliwili API
entry points remain as an adapter for recommendations, details, comments and
playback URL preparation. Stream workers and decoding retain their existing
lifecycle and are not run through the JSON request transport.

```mermaid
flowchart TD
    UI[UI and controller] --> App[App: jobs and main-thread state]
    App --> Search[SearchService]
    App --> Auth[AuthService]
    Search --> API[BilibiliApi]
    Auth --> API
    API --> HTTP[HttpClient]
    HTTP --> Wifi[WifiManager / PSP network]
    Auth --> Session[SessionStore]
    Search --> Error[AppError]
    Auth --> Error
    HTTP --> Error
    Error --> App
    App --> Presentation[Error messages and diagnostic log]
```

## Ownership and jobs

App owns the API, search and authentication services and the session store.
A single worker runs one job at a time. Its input is captured before starting;
its results are staged until the main thread joins the worker. Renderers read
only main-thread state. Session credentials are applied between requests on this
worker; the synchronous player finishes before another UI job can start.

Search requests carry a keyword, page and generation. App rejects results from a
different generation. A failed appended page leaves existing cards and the page
to retry intact. Empty results and cancellation are not errors. Search history
continues using the existing local history file.

## Errors and retries

AppError contains independent domain, kind, operation, raw code and HTTP status.
Result<T> contains a value or an error. UI text is produced by errorMessage;
logs omit raw response bodies, URLs, cookies and QR keys. Playback also uses this
error presentation, while retaining its original stage and cleanup code log.

HttpClient permits one additional attempt for safe GETs on a timeout or HTTP
502/503/504. QR creation and polling do not use automatic transport retries.
WBI keys are cached for one hour. A signature rejection (-403) invalidates the
key and permits one fresh signature attempt. An actual WBI transport failure is
not relabeled as API -412. No operation retries a login-required or risk-control
response indefinitely.

The legacy C network_error buffer remains for stream workers and existing C
callers. New search/auth/API JSON requests return their own response and error.
The old BilibiliClient error callback necessarily flattens these errors when
serving unmigrated UI jobs.

## Login

Open My, select the account row and press Cross. The login page creates a real
Bilibili web QR code and polls every two seconds after the previous poll ends.
States are waiting for scan, waiting for phone confirmation, expired and signed
in. A transport error pauses polling and offers refresh. Circle cancels an
active request and leaves the page once the worker has joined. Leaving the page
stops polling. Triangle refreshes the QR or checks an existing session.

Successful polling reads Set-Cookie from the passport HTTPS response. The store
accepts SESSDATA, bili_jct, DedeUserID and DedeUserID__ckMd5, and requires a
session token and numeric user ID. It persists session.json, then activates the
credentials. The JSON API receives credentials only at exact HTTPS API and
passport hosts, with redirects disabled while credentials are attached. CDN
streams do not receive account cookies. Login response headers are captured
only at the exact passport host, without redirects.

On startup a stored session is restored and checked using nav. A network failure
keeps it intact. Only an explicit logged-out response expires it. Square on the
account page clears local credentials and files; this is local sign-out, not
revocation of all Bilibili sessions. The account's name and UID are shown after
validation. Cloud history/favorites synchronization and automatic cookie-token
refresh are not implemented.

Writes use a temporary file and rename. A backup/rollback path supports memory
sticks that cannot rename over an existing file. Logout also removes temporary
and backup credentials. Errors leave the live session unchanged where possible.
The credentials file is local plaintext; it is not included in build packages.

## Verification

- `sh scripts/test-services.sh`: production service code with injected transport;
  retry bounds, cancellation, WBI failure/refresh, schema errors, pagination,
  QR states, persistence, offline validation, expiry, logout, write failures,
  credential host checks and header bounds, error-log redaction.
- Build an offline client into build/client-services-offline and package it into
  build/services-offline-dist, then run `python3 scripts/check-services-ui.py`.
  It drives the real App through PPSSPP Headless with synthetic API fixtures.
  Screenshots and assertions are written under build/services-ui-check.
- Real account confirmation requires scanning the real PSP login screen with
  the account owner's phone. A fixture test is not evidence of a live login.
- Do not use PSPLink scrshot with the current ME decoder: its partition-4
  scratch allocation overlaps the decoder's ME firmware image. Direct VRAM
  savemem also caused USB transfer stalls. Headless captures do not use either.
