# `mux-engine`

`mux-engine` is the profile-owning WPE/WebKit UI process. It is intentionally
separate from `muxd`: `muxd` owns control-plane state, persisted recovery
metadata, and centralized clipboard history; `mux-engine` owns WebKit views,
browser policy, profile state, and frame production. `mux-pane` owns terminal
input and Kitty presentation.

## Running

Build it with the other WPE runtime binaries:

```sh
meson setup build/wpe spikes/wpe-kitty
meson compile -C build/wpe mux-engine
```

Run one profile engine in the foreground while developing:

```sh
build/wpe/mux-engine --profile default
```

Start it idempotently in the background:

```sh
build/wpe/mux-engine --ensure --profile default
```

Normal panes discover or start the appropriate profile engine automatically.
The current browser path is `mux-pane` plus `mux-engine`; `mux-view` is not a
current fallback architecture.

## Ownership and paths

One engine exists per profile. A process-lifetime lock prevents two UI
processes from opening the same WebKit profile. The engine socket filename is
versioned with the binary protocol:

```text
socket: $XDG_RUNTIME_DIR/mux/mux-engine-v2-PROFILE.sock
data:   $XDG_DATA_HOME/mux/profiles/PROFILE
cache:  $XDG_CACHE_HOME/mux/profiles/PROFILE
```

When a safe `XDG_RUNTIME_DIR` is unavailable, the socket uses Mux's validated
owner-only per-user fallback. Runtime, socket, profile, and cache directories
are restricted to their owner.

Supported path/profile overrides are:

```text
MUX_PROFILE
MUX_ENGINE_SOCKET
MUX_PROFILE_DATA_DIR
MUX_PROFILE_CACHE_DIR
```

Profile names are restricted to the implementation's validated ASCII subset.

## Session model

Each engine creates:

- One persistent `WebKitNetworkSession` using explicit profile data and cache
  paths.
- One ephemeral `WebKitNetworkSession` for private views.
- One headless WPE display shared by the engine's WebKit views.
- The WebKit UI-process objects and browser policy managers required by those
  views.

`CREATE_VIEW` selects the persistent session by default. The ephemeral flag
selects the private session, which does not write into persistent profile
directories. WebKit owns the associated content and network processes.

## Connection security

The listener is an owner-only `SOCK_SEQPACKET` Unix socket. Linux
`SO_PEERCRED` must report the engine UID, and the PID claimed by `HELLO` must
match the kernel-reported peer PID. Packets, payloads, dimensions, counts, and
allocations are bounded before parsing or allocation. Client, view, and
aggregate frame-memory limits prevent one pane from creating unbounded engine
state.

The socket and lock name include protocol version 2. A protocol mismatch fails
before view creation rather than reconnecting indefinitely to an incompatible
engine.

## Protocol version 2

The fixed envelope and field encoding are documented in `engine-boundary.md`.
The implemented message families include:

- Connection and view lifecycle: `HELLO`, `WELCOME`, `CREATE_VIEW`,
  `VIEW_CREATED`, destroy, acknowledgement, and error records.
- Browser commands: load, back, forward, reload, and stop actions.
- Presentation: resize, `FRAME`, `FRAME_ACK`, and metadata.
- Input: keyboard, text, pointer, buttons, wheel, and focus.
- Visibility: `SET_VISIBILITY` with a bounded `u32` value of zero or one.
- Graceful close: serial-bound `REQUEST_CLOSE`, `CANCEL_CLOSE`, and
  `CLOSE_READY`.
- Browser shell bridges for trusted UI, clipboard, downloads, file selection,
  permissions, TLS, authentication, and crash reporting.

`SET_FOCUS` and `SET_VISIBILITY` are independent. A visible unfocused pane may
continue to render. A hidden pane does not import, convert, or copy WPE frame
pixels; becoming visible resets presentation and requests a fresh full frame
without reloading the page.

## Frame lifecycle

The engine admits at most one outstanding frame per view and accounts retained
surfaces plus pending shared-memory frames against a bounded aggregate budget.
The pane acknowledges only after a response from the matching Kitty image
generation. The frame ACK deadline is 30 seconds. Timeout, disconnect, hide,
resize, or teardown retires the pending object and fails closed rather than
allowing a stale response to acknowledge newer shared memory.

## Close lifecycle

The first close request enters WebKit's graceful close path. If WebKit permits
closure, the engine emits matching `CLOSE_READY`. If the page chooses Stay, no
ready record is emitted; the pane eventually sends `CANCEL_CLOSE` and remains
alive. A second close key while the request is pending is the explicit
force-close path. Serial matching makes late cancellation or readiness harmless.

## Recovery limitations

If an engine is replaced, a pane can reconnect and recreate its view from the
last known URL. That is URL recovery, not page restoration: DOM, forms, scroll,
JavaScript, media, and in-page history are lost. v0.1 also has no command that
relaunches all persisted panes or reconstructs their Kitty tabs and split
layout.
