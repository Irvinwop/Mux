# Browser affordances across the engine boundary

Mux renders web content in Kitty, but WPE WebKit is an embedding API rather than
a complete browser UI. A useful browser must supply the UI that ordinary sites
expect: JavaScript dialogs, permission decisions, file selection, downloads,
popups, authentication, clipboard access, option menus, notifications, and text
input.

This document defines that boundary for the shared `mux-engine`, the thin
`mux-pane`, and `muxd`. It targets WPE WebKit and WPEPlatform 2.52.

## Design rules

1. `mux-engine` owns every `WebKit*Request` and `WebKit*Dialog` object. Those
   objects never cross a process boundary.
2. `mux-pane` owns terminal interaction. It renders prompts as ANSI text above a
   negative-z Kitty graphics placement; prompt UI is never converted into web
   frame images.
3. `muxd` owns actions that affect more than one pane: popup placement,
   clipboard transaction leases, notifications, and layer membership.
4. Every asynchronous interaction has a 64-bit request ID, a deadline, and an
   explicit terminal state. Late or duplicate responses are ignored.
5. Pane disconnect, view destruction, or navigation invalidation resolves every
   outstanding request with the safest result: deny, cancel, or stay on page.
6. Payloads remain bounded by the existing 256 KiB engine-protocol limit. Large
   clipboard values and path lists use chunks. Download bytes never traverse
   pane IPC.
7. Requests display their security origin. A pane must not show an untrusted
   message where an origin is expected.

## WPE hooks

WPE WebKit 2.52 exposes the required embedder signals directly on
`WebKitWebView`:

- `create` and `ready-to-show` for new browsing contexts.
- `script-dialog` for alert, confirm, prompt, and before-unload.
- `permission-request` and `query-permission-state` for powerful features.
- `run-file-chooser` for HTML file inputs.
- `authenticate` for HTTP authentication.
- `context-menu` and `show-option-menu` for browser-owned menus.
- `show-notification` for web notifications.
- `web-process-terminated` for a recoverable crash state.

Downloads are announced by `WebKitWebContext::download-started`. WPEPlatform
also lets the custom `WPEDisplay` provide both `get_clipboard` and
`create_input_method_context` virtual methods.

References:

- <https://wpewebkit.org/reference/2.52.0/wpe-webkit-2.0/class.WebView.html>
- <https://wpewebkit.org/reference/2.52.0/wpe-webkit-2.0/class.WebContext.html>
- <https://wpewebkit.org/reference/2.52.0/wpe-platform-2.0/class.Display.html>
- <https://wpewebkit.org/reference/2.52.0/wpe-platform-2.0/class.Clipboard.html>
- <https://wpewebkit.org/reference/2.52.0/wpe-platform-2.0/class.InputMethodContext.html>

## Protocol extension

The existing fixed-size, big-endian envelope remains unchanged. New message
types are append-only. Peers advertise support as capability bits in `HELLO`
and `WELCOME`; an old peer therefore continues to work with requests denied by
default.

### Generic prompt messages

`UI_REQUEST` is sent from engine to pane and contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `request_id` | `u64` | Unique for the engine lifetime |
| `kind` | `u16` | Dialog, permission, auth, chooser, menu, or crash |
| `flags` | `u32` | Kind-specific booleans |
| `deadline_ms` | `u32` | Remaining monotonic time |
| `origin` | string | Canonical security origin |
| `heading` | string | Trusted browser-generated heading |
| `message` | string | Site-provided text, length limited |
| `default_value` | string | Prompt default or suggested filename |
| `choices` | vector | Bounded menu or action entries |

`UI_RESPONSE` is sent from pane to engine and contains the request ID, one
enumerated action, an optional UTF-8 value, and an optional vector of paths.
The engine verifies that the action is valid for the original request kind.

`UI_CANCEL` is sent in either direction when the underlying object disappeared
or the pane abandoned its overlay. It is idempotent.

Unknown request kinds are answered with `UNSUPPORTED`, which the engine maps to
the fail-closed result.

### Limits

- At most one blocking prompt is visible per view.
- At most eight requests may be queued per view.
- Site text is capped at 8 KiB and sanitized as UTF-8 before display.
- A menu carries at most 256 entries and 64 KiB of labels.
- A chooser returns at most 128 paths.
- Permission requests expire after 30 seconds.
- Dialog and authentication requests expire after 120 seconds.
- File choosers expire after 300 seconds.
- Menus expire after 60 seconds.

## Pane overlay compositor

The web frame placement uses Kitty graphics z-index `-1`. Normal browsing keeps
the terminal text plane blank. When a request arrives, `mux-pane` paints a
compact ANSI panel over the bottom rows and intercepts input before it reaches
the engine.

The panel always includes:

- Browser-owned request type.
- Canonical origin in a visually separate row.
- Site-provided text with control characters removed.
- Explicit key labels, such as `[a] allow` and `[d] deny`.
- Remaining timeout for security-sensitive requests.

The overlay uses synchronized terminal updates so panel changes and web frame
updates do not tear. Dismissing it clears only its text rows and redraws the
latest frame damage. No prompt causes a new PNG, full-frame screenshot, or
additional WebKit render.

## JavaScript dialogs

The engine translates `WebKitScriptDialog` to these prompt kinds:

| WebKit type | Actions | Disconnect/timeout result |
| --- | --- | --- |
| Alert | acknowledge | acknowledge |
| Confirm | accept, cancel | cancel |
| Prompt | submit text, cancel | cancel |
| Before unload | leave, stay | stay |

Only one JavaScript dialog is active per view. Repeated dialogs are queued up to
the global per-view limit and then dismissed safely to prevent dialog spam from
making the pane unusable.

## Permissions

Permission prompts are categorized by concrete GObject type rather than by
site-supplied strings. Initial categories are geolocation, notifications,
camera, microphone, display capture, pointer lock, clipboard, third-party
storage, encrypted media, device enumeration, and XR.

The default is deny. Decisions can be:

- Allow once.
- Deny once.
- Remember allow for this origin and permission kind.
- Remember deny for this origin and permission kind.

Remembered policy is stored by `mux-engine` in the selected profile with owner-
only permissions and atomic replacement. Private profiles never persist it.
`query-permission-state` consults the same store, so JavaScript observes a state
consistent with prior decisions.

Camera, microphone, display capture, encrypted media, and XR are initially
compiled in but policy-disabled until their Linux device and sandbox behavior
is exercised. The pane explains that distinction instead of silently claiming
the feature is unavailable.

## File chooser

For `run-file-chooser`, the engine sends accepted MIME types, multi-select state,
and the current origin. The pane temporarily suspends raw browsing input and
uses `kitten choose-files` when Kitty 0.45 or newer is available. Multiple mode
maps to `--mode=files`; otherwise it uses `--mode=file`.

On older Kitty versions, the overlay accepts newline-separated absolute paths.
Before answering WebKit, the pane canonicalizes paths and the engine validates
that each path is absolute, exists, is a regular readable file, and was returned
for the current request. No directory is recursively exposed.

Kitty chooser reference:
<https://sw.kovidgoyal.net/kitty/kittens/choose-files/>

## Downloads

Download data stays inside the shared engine process. On
`download-started`, the engine associates the `WebKitDownload` with its source
view and sends a destination request containing the suggested filename, MIME
type, source URI, and expected size when known.

Default behavior:

1. Suggest `$XDG_DOWNLOAD_DIR`, falling back to `~/Downloads`.
2. Sanitize the server-provided basename and reject path separators.
3. Never overwrite. Add ` (1)`, ` (2)`, and so on using an exclusive create.
4. Write a partial file with owner-only permissions.
5. Atomically rename on completion.
6. Remove the partial file on cancellation or failure unless explicitly kept.

`DOWNLOAD_EVENT` reports started, progress, finished, failed, and cancelled
states. Progress updates are coalesced to at most four per second. The global bar
may summarize active downloads, but it never blocks frame delivery.

## Popups and new browsing contexts

`WebKitWebView::create` is synchronous, so the engine immediately creates a
related child view and returns it to WebKit. It also generates a random,
single-use 128-bit attachment token and sends `VIEW_OFFER` to the parent pane.

The parent pane asks `muxd` to place the child in the current layer. `muxd`
launches `mux-pane --attach TOKEN`; that pane sends `ATTACH_VIEW` to the engine.
The engine consumes the token and binds the existing child view to the new pane.

Properties:

- Tokens expire after 30 seconds and are consumed exactly once.
- A popup has no terminal process until `ready-to-show`.
- Closing the parent does not implicitly destroy an attached child.
- An unattached or denied popup is destroyed after expiry.
- User-gesture metadata is preserved so unsolicited popup policy can differ
  from link-open behavior.
- The default placement is a new Kitty pane on the current logical layer, not a
  new engine process.

## Clipboard

The headless WPE display otherwise receives a local-only clipboard. Mux provides
a `WPEClipboard` subclass backed by an engine-side MIME cache and bridges that
cache through the pane to Kitty's OSC 5522 clipboard protocol.

Kitty OSC 5522 supports arbitrary MIME types, chunked reads and writes,
permission results, request IDs, and paste events. The pane enables paste-event
mode with `CSI ? 5522 h`. A user paste causes the pane to request only advertised
types, update the engine cache, and then send the editing action or committed
text to WebKit. Web-originated copy updates the WPE cache first; the engine then
sends bounded `CLIPBOARD_WRITE` chunks to the focused pane.

Rules:

- `muxd` grants a single global clipboard transaction lease; contention returns
  `EBUSY`, as the Kitty protocol recommends for multiplexers.
- Each transfer has an ID and no more than one transfer is in flight per pane.
- Chunks are at most 4096 unencoded bytes.
- Text is capped at 8 MiB; non-text data is capped at 32 MiB per MIME type.
- Clipboard reads caused by `navigator.clipboard` still require WebKit's
  permission request. A paste gesture is not reusable ambient permission.
- OSC 52 plain-text fallback is write-only unless the user enables terminal
  clipboard reads.
- Pane disconnect clears its lease and fails pending reads.

Kitty clipboard protocol reference:
<https://sw.kovidgoyal.net/kitty/clipboard/>

## Text input and IME

Normal Kitty keyboard events remain appropriate for shortcuts and physical key
semantics. Committed Unicode text gets a separate `TEXT_COMMIT` message so it is
not reconstructed from layout-dependent key codes.

Each WebView owns a `WebKitInputMethodContext` subclass. `TEXT_COMMIT` emits its
`committed` signal when editable content is focused; the matching physical text
key is suppressed to prevent duplicate insertion. Focus and cursor-area changes
follow the active view. This supports terminal-committed desktop IME input
without pretending that Kitty exposes composition preedit state that it does
not provide.

Full preedit styling is therefore a known limitation. It can be added later if
Kitty gains a composition protocol; committed CJK text, emoji, dead-key output,
and compose-key output do not wait for that extension.

## Menus, notifications, and crash recovery

Context menus and HTML option menus use the generic bounded choice vector. The
engine retains the WebKit menu object while the pane displays keyboard-searchable
text choices. Disabled and separator entries are preserved but cannot be
selected.

Notifications remain permission-gated in WebKit and are presented by the
originating pane through Kitty OSC 99. Title and body are Base64 chunked, page
text never enters terminal metadata, and tag replacements retain a stable
identifier. Activation focuses the originating Kitty pane and layer and reports
the click to WebKit; terminal close events close the WebKit notification.

On `web-process-terminated`, the engine sends a non-spoofable crash overlay with
reload and close actions. It does not reuse the last rendered page as if the
view were still live.

## Delivery order

Implementation proceeds in vertical slices:

1. Generic request IDs, prompt overlay, JavaScript dialogs, and disconnect
   cleanup.
2. Permission classification, profile policy persistence, and query state.
3. Downloads and file chooser integration.
4. Popup attachment tokens and `muxd` placement.
5. OSC 5522 clipboard bridge and committed-text input context.
6. Menus, notifications, authentication, and crash recovery.

Each slice must preserve compatibility with peers that lack its capability bit.
Linux build/runtime exercise remains the gate before any of these features can
be called complete.
