# Mux control plane

`muxd` centralizes multiplexing state, session metadata, and clipboard state.
It deliberately does not own WPE/WebKit or frame production.

## Processes

- `muxd` owns the live pane registry, stable view IDs, active view, logical
  layers, state revision, persisted recovery records, and centralized clipboard
  snapshots/history.
- `mux-engine` owns WPE/WebKit views and each profile's browser session.
- `mux-pane` owns terminal input, trusted overlays, and Kitty frame
  presentation. It registers its Mux ID, process ID, Kitty window ID, Kitty
  socket, layer, URI, title, and focus state with `muxd`.
- `mux-bar` subscribes to control-plane state and sends structured navigation
  requests.
- `muxctl` is the human and scripting interface.

The launcher starts `muxd` idempotently. Its Unix socket is stored below the
owner-only Mux runtime directory, normally:

```text
$XDG_RUNTIME_DIR/mux/muxd.sock
```

If a safe XDG runtime directory is unavailable, Mux uses its validated per-user
fallback. The daemon verifies Linux peer credentials for accepted clients.

## Commands

Run commands through the repository launcher when the build directory is not on
`PATH`. The implemented command names are:

```text
status
list
active
focus
open
back
forward
reload
quit
layer
move
```

Examples:

```sh
./mux ctl status
./mux ctl list
./mux ctl active
./mux ctl open https://example.com
./mux ctl reload
./mux ctl focus VIEW-ID
./mux ctl layer research
./mux ctl move VIEW-ID research
./mux ctl quit VIEW-ID
```

Navigation targets the active view unless the selected command accepts an
explicit view ID. There is no `watch`, `prompt`, or `restore` CLI command in
v0.1; `mux-bar` uses the subscriber protocol directly.

## Wire boundary

The control protocol uses newline-terminated records. Structural fields are
tab-separated and user-controlled strings are base64 encoded. Persistent pane
and subscriber connections coexist with one-shot control clients.

Every accepted socket is nonblocking. Each client has an ordered bounded FIFO
of complete records; partial writes retain their offset and resume through
`POLLOUT`. Reads and writes are work-limited per event-loop iteration. Queue
overflow, protocol failure, or an unrecoverable send error disconnects only the
offending client. A one-shot control connection drains its complete response
before closing.

Subscribers receive a snapshot followed by revisioned events. `mux-bar` uses
that contract instead of scraping Kitty titles or inspecting WebKit processes.

## Kitty operations

The dedicated Kitty instance listens on a filesystem socket below an
owner-only runtime directory. The launcher supplies a remote-control password
scoped to `detach-window`, `focus-window`, and `launch`. It does not use a Linux
abstract socket or unrestricted `socket-only` control.

Layer focus uses a registered live Kitty window. A physical move is committed
only after Kitty successfully detaches the source window into a live target tab
on the same Kitty socket. The subprocess runs asynchronously with a bounded
timeout, so control, session, and clipboard clients remain serviceable. Empty
target layers, cross-Kitty moves, timeouts, and Kitty rejection leave logical
metadata unchanged.

## Session boundary

`muxd` persists stable view IDs, layer membership, profile identity, and last
known URLs for non-private panes. Abrupt disconnect records can be reclaimed;
an explicit clean `BYE` removes the live record.

This is recovery metadata, not automatic workspace restoration. v0.1 does not
relaunch offline panes, recreate Kitty tabs or split geometry, retain a live
WebKit view across engine replacement, or restore DOM and JavaScript state.

## Clipboard boundary

`muxd` owns profile-qualified clipboard snapshots and history so panes can
share observed formats without exposing history enumeration to websites.
`mux-pane` handles Kitty OSC transport, while `mux-engine` supplies WebKit's
clipboard-facing provider. The daemon remains the authority for cross-pane
history, limits, deduplication, and private-profile isolation.
