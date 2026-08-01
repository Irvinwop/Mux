# Engine boundary

## Decision

Mux will use one long-lived UI/engine process for a browser profile. Kitty panes
will become lightweight clients responsible for terminal sizing, keyboard and
pointer capture, and Kitty graphics commands.

This is not merely an optimization. WebKit's browsing session lives in a
NetworkProcess shared by WebContent processes belonging to one session. Pointing
separate UI processes at the same data directory does not create that shared
session and can produce multiple processes contending for one cookie store.

The transition is intentionally additive. mux-view remains the runnable
direct-rendering implementation until mux-engine and mux-pane cover its input
and rendering behavior.

## Process model

                         owner-only Unix sockets
  +----------------+      control       +----------------+
  | mux-bar        | <----------------> | muxd           |
  +----------------+                    | Kitty registry |
                                        | layers/focus   |
                                        +----------------+
                                                 ^
                                                 |
                                                 v
  +----------------+      engine        +----------------+
  | mux-pane       | <----------------> | mux-engine     |
  | Kitty input    |                    | UI process     |
  | Kitty graphics|                    | WebViews       |
  +----------------+                    | NetworkSession |
          ^                             +----------------+
          |                                      |
          v                                      v
       Kitty PTY                         WebContent processes
                                         one NetworkProcess

muxd continues to own user-visible multiplexing state. mux-engine owns all
objects that must truly be shared by browser tabs: WebKitNetworkSession,
WebKitWebsiteDataManager, cookie manager, cache, storage, proxy configuration,
content filters, and WebKit view creation.

## Transport invariants

- The engine socket is SOCK_SEQPACKET; one packet is one protocol message.
- Every integer is unsigned and encoded in network byte order.
- The fixed header is 40 bytes and begins with MUX1 plus protocol version 1.
- Payloads are capped at 256 KiB. Pixel bytes never travel through the socket.
- Connections are accepted only when Linux peer credentials match the daemon UID.
- A pane may have at most one unacknowledged frame. New damage is unioned while
  that frame is pending.
- Every view has a 64-bit engine ID. Every frame has a monotonically increasing
  64-bit serial.
- Unknown message types are protocol errors for version 1; flags unknown to a
  receiver are ignored.

The implementation lives in spikes/wpe-kitty/src/mux-engine-protocol.[ch].

## Header

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | magic, 0x4d555831 (MUX1) |
| 4 | 2 | protocol version |
| 6 | 2 | message type |
| 8 | 4 | flags |
| 12 | 4 | payload size |
| 16 | 8 | view ID, or zero for connection messages |
| 24 | 8 | request/frame serial |
| 32 | 8 | reserved, zero in version 1 |

Strings are a 32-bit byte length followed by UTF-8 bytes without a trailing NUL.
Rectangles are four 32-bit values in x, y, width, height order.

## Message flow

1. A pane connects and sends HELLO with its PID, Kitty window ID, layer, pixel
   dimensions, scale, and initial URL.
2. The engine validates peer credentials and replies with WELCOME.
3. The pane sends CREATE_VIEW; the engine creates a WebKitWebView using the
   process-global persistent WebKitNetworkSession and replies VIEW_CREATED.
4. The pane sends RESIZE, INPUT_KEY, INPUT_POINTER, SET_FOCUS, and NAVIGATE
   messages. The engine sends METADATA whenever URI, title, loading, or security
   state changes.
5. WPE's buffer callback causes the engine to copy only the coalesced damage into
   a short-lived POSIX shared-memory object and send FRAME.
6. The pane submits that object to Kitty using raw RGBA shared-memory transport.
   It requests a Kitty graphics response and sends FRAME_ACK only after Kitty
   reports that it consumed the object.
7. The engine unlinks the object on acknowledgement. It also unlinks stale
   objects on disconnect or timeout.

## Frame payload

    u32 width
    u32 height
    u32 stride
    u32 pixel_format       # RGBA8888 in version 1
    u32 damage_count       # 1..64
    damage_count * rect
    u64 shm_size
    string shm_name

The first frame and every size change set FULL_DAMAGE. If a newer render
supersedes damage waiting behind an unacknowledged frame, the next message sets
REPLACES_PENDING. This preserves latency under load instead of queuing obsolete
frames. The adaptive ceiling remains a policy above this mechanism: idle pages
produce no frames, ordinary motion targets 60 Hz, and the ceiling can rise on a
high-refresh terminal without polling or PNG encoding.

## Profile ownership

mux-engine will create one explicit persistent session:

    webkit_network_session_new(profile_data_directory,
                               profile_cache_directory);

Every WebKitWebView created by that engine receives the same object through its
network-session construction property. Separate profiles require separate engine
processes and separate socket namespaces. Private layers use a separate
ephemeral session; they never reuse the persistent profile directories.

This gives Mux real cross-tab cookies, HTTP cache, credentials, local storage,
IndexedDB, service workers, proxy policy, and tracking-prevention state while
retaining WebKit's process isolation for page content.
