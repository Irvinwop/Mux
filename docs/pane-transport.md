# Pane transport

`mux-pane` is the thin Kitty-facing half of the profile-engine architecture. It
does not link WebKit, own browser session state, or receive pixel bytes through
its Unix socket. `mux-engine` owns WPE/WebKit and frame production; `muxd` owns
control, session metadata, and centralized clipboard history.

## Frame path

1. The custom WPE view in `mux-engine` receives a rendered `WPEBuffer`.
2. A hidden view marks the buffer rendered and releases it without importing,
   converting, or copying pixels.
3. For a visible view, the engine imports the buffer, updates its retained RGBA
   surface, and coalesces damage.
4. The outgoing region is copied into a uniquely named owner-only POSIX
   shared-memory object.
5. `FRAME` sends dimensions, damage, object name, size, and serial over the
   version 2 `SOCK_SEQPACKET` connection.
6. `mux-pane` admits a complete Kitty shared-memory graphics command to its
   bounded nonblocking PTY FIFO.
7. The pane records the engine frame serial together with the Kitty image
   generation and waits for the corresponding graphics response.
8. After a matching response, the pane sends `FRAME_ACK`. Only then may the
   engine send the next frame for that view.

If WPE renders while a frame is outstanding, the engine updates its retained
surface and coalesces later damage instead of queuing stale animation frames.
Hide, resize, overlay transitions, and reconnect retire pending presentation
entries. A bounded generation/serial response FIFO prevents a late Kitty reply
from acknowledging newer shared memory.

The frame ACK timeout is 30 seconds. Expiry fails the engine presentation
connection closed and cleans up the pending shared-memory object. It does not
continue sending frames under an ambiguous image generation.

This transport does not encode PNGs, base64-encode pixel data, poll pages, or
push unchanged frames. Base64 is used only for short textual Kitty protocol
fields such as the shared-memory object name.

## PTY output

Graphics commands, trusted terminal UI, and clipboard OSC records share one
ordered bounded nonblocking PTY queue. Partial writes resume through `POLLOUT`;
terminal backpressure does not turn an individual clipboard or frame write into
an unbounded blocking call. Capacity is reserved for complete frame commands,
and clipboard production pauses and retries when the queue cannot accept more
data.

Teardown attempts only a bounded drain before restoring terminal state and
discarding remaining output. A hard terminal failure explicitly fails pending
operations rather than silently reporting success.

## Input path

`mux-pane` enables Kitty's enhanced keyboard protocol, SGR pixel-mouse
reporting, focus reporting, and resize notifications. It translates terminal
events into version 2 keyboard, pointer, focus, visibility, and resize records.

`mux-engine` constructs WPE keyboard, pointer-button, pointer-motion, scroll,
and focus events for the authenticated pane's WPE view. Unicode input, release
and repeat events, functional keys, pixel mouse movement, buttons, wheel
scrolling, focus, and resize remain terminal-to-WPE concerns of the pane/engine
boundary, not `muxd`.

## Visibility

Focus and visibility are separate protocol state. Layer changes and trusted
overlay transitions send `SET_VISIBILITY` explicitly.

On hide, the engine retires pending frame state, releases its retained pixel
surface, and suppresses future pixel import and shared-memory frame emission.
On show, it resets the WPE viewport and produces a fresh full frame without
reloading the page. This suppresses rendering transport only; WebKit may still
run JavaScript, network requests, audio, or media for the hidden page.

## Graceful close

The first `Ctrl+Q` sends serial-bound `REQUEST_CLOSE`. `mux-engine` invokes
WebKit's graceful close path so `beforeunload` can ask whether to leave. A
matching `CLOSE_READY` permits normal pane teardown.

If the page chooses Stay or the close deadline expires, `mux-pane` sends
matching `CANCEL_CLOSE`, clears the pending request, and stays open. A second
`Ctrl+Q` while the request is pending is the explicit force-close action. Late
close responses are ignored by serial.

## Control integration

`mux-pane` also registers as a normal view client with `muxd`. URI and title
metadata update `mux-bar`; focus updates the active view and layer; `Ctrl+L`
asks the daemon to focus the layer bar; and structured control commands are
translated into engine navigation.

All daemon sockets and outbound queues are nonblocking and bounded. A stalled
bar, control client, or pane is disconnected rather than allowed to freeze
layer, session, or clipboard processing.

If the profile engine is absent, the pane starts the sibling `mux-engine` with
`--ensure` and waits for its owner-only versioned socket. Concurrent panes are
safe because the engine holds a profile lock before opening WebKit storage.

Reconnect recreates the page from its last URL. It does not preserve DOM,
forms, scroll, JavaScript, media, or in-page history. Automatic relaunch of
offline panes and reconstruction of Kitty tabs or split geometry remain
unimplemented restoration targets.
