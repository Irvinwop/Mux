# Engine boundary

## Decision

Mux uses one long-lived WPE/WebKit UI process per browser profile. Kitty panes
are lightweight clients responsible for terminal sizing and input, trusted UI,
and Kitty graphics presentation.

This is a correctness boundary as well as an optimization. WebKit's browsing
session lives in the network and content processes associated with one UI
process and `WebKitNetworkSession`. Separate UI processes pointed at the same
data directory would not create one shared live browser session and could
contend for profile storage.

`mux-engine` is therefore the WebKit owner. `mux-pane` is the terminal owner.
`muxd` is the control, session-metadata, and clipboard authority; it never
imports WPE buffers or owns WebKit views. The former `mux-view` transition path
is historical and is not the current architecture.

## Process model

```text
                         owner-only Unix sockets
  +----------------+      control       +----------------+
  | mux-bar        | <----------------> | muxd           |
  +----------------+                    | registry       |
                                        | session state  |
                                        | clipboard      |
                                        +----------------+
                                                 ^
                                                 |
                                                 v
  +----------------+      engine v2     +----------------+
  | mux-pane       | <----------------> | mux-engine     |
  | terminal input |                    | WPE/WebKit UI  |
  | trusted UI     |                    | profile/views  |
  | Kitty graphics |                    | frame producer |
  +----------------+                    +----------------+
          ^                                      |
          |                                      v
       Kitty PTY                         WebKit content and
                                         network processes
```

## Transport invariants

- The engine socket is owner-only `SOCK_SEQPACKET`; one packet is one message.
- Its filename includes protocol version 2, preventing silent reuse of a v1
  daemon socket.
- Every integer is unsigned and encoded in network byte order.
- The fixed header is 40 bytes and begins with `MUX1` plus protocol version 2.
- Payloads are capped at 256 KiB. Pixel bytes never travel through the socket.
- Linux peer credentials must match the engine UID, and the PID claimed in
  `HELLO` must match the kernel peer PID.
- A pane has at most one unacknowledged engine frame per view. Later damage is
  coalesced rather than queued as stale frames.
- Every view has a 64-bit engine ID. Requests and frames use monotonically
  increasing 64-bit serials.
- Kitty presentation responses are tracked by image generation and frame
  serial, so a retired response cannot acknowledge a newer frame.
- A frame ACK deadline is 30 seconds. Expiry fails the presentation connection
  closed rather than reusing an ambiguous image generation.
- Unknown message types are protocol errors for version 2; unknown flags that a
  receiver is allowed to ignore remain forward-compatible.

The implementation lives in `spikes/wpe-kitty/src/mux-engine-protocol.[ch]`.

## Header

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | magic, `0x4d555831` (`MUX1`) |
| 4 | 2 | protocol version, currently 2 |
| 6 | 2 | message type |
| 8 | 4 | flags |
| 12 | 4 | payload size |
| 16 | 8 | view ID, or zero for connection messages |
| 24 | 8 | request or frame serial |
| 32 | 8 | reserved, zero in version 2 |

Strings are a 32-bit byte length followed by UTF-8 bytes without a trailing
NUL. Rectangles are four 32-bit values in `x`, `y`, `width`, `height` order.

## Core message flow

1. A pane connects to the versioned profile socket and sends `HELLO` with its
   PID, Kitty identity, layer, viewport, and initial URL.
2. The engine validates the packet and peer credentials, then replies
   `WELCOME`.
3. The pane sends `CREATE_VIEW`; the engine creates a `WebKitWebView` in the
   selected persistent or ephemeral session and replies `VIEW_CREATED`.
4. The pane sends resize, navigation, input, focus, and explicit visibility
   records. The engine emits metadata as browser state changes.
5. For a visible view, a WPE buffer callback updates the engine surface and
   creates a short-lived owner-only shared-memory frame.
6. The engine sends `FRAME`; the pane queues the corresponding Kitty command on
   its bounded nonblocking PTY transport.
7. After a correlated Kitty response, the pane sends `FRAME_ACK`. The engine
   then releases the gate and cleans up the shared-memory object.
8. Hiding a view sends `SET_VISIBILITY 0`, retires pending presentation, and
   suppresses buffer import and copying. Showing it sends `SET_VISIBILITY 1`
   and causes a fresh full frame without reloading the page.

## Graceful close

Close is serial-bound in protocol v2:

1. The first `Ctrl+Q` causes the pane to send `REQUEST_CLOSE`.
2. The engine invokes WebKit's graceful close path, including `beforeunload`.
3. If WebKit accepts closure, the engine returns matching `CLOSE_READY` and the
   pane performs normal teardown.
4. If the page chooses Stay or the request deadline expires, the pane sends
   matching `CANCEL_CLOSE`, clears the pending request, and remains open.
5. A second `Ctrl+Q` while a close request is pending is the explicit
   force-close operation.

Late close records are ignored by serial and cannot close a later page state.

## Frame payload

```text
u32 width
u32 height
u32 stride
u32 pixel_format       # RGBA8888 in version 2
u32 damage_count
damage_count * rect
u64 shm_size
string shm_name
```

The first frame after attach, resize, show, or transport recovery is full
damage. The engine has bounded per-view dimensions and an aggregate frame-memory
budget. Hidden views release WPE buffers without importing pixels.

## Profile ownership

Each persistent engine creates one explicit persistent network session using
its profile data and cache directories. Every persistent `WebKitWebView` in
that process shares it. An ephemeral view uses the engine's separate ephemeral
session and never reuses persistent profile directories.

Separate profiles require separate engine processes, profile directories, and
versioned socket namespaces. This provides shared cookies, HTTP cache,
credentials, local storage, IndexedDB, service workers, proxy policy, and
tracking-prevention state while preserving WebKit's content-process isolation.

## Recovery boundary

Pane and engine reconnection can recreate a WebKit view at the last known URL.
It does not retain or restore DOM, form, scroll, JavaScript, media, or in-page
history state. Automatic workspace and Kitty-layout reconstruction is also not
implemented in v0.1.
