# mux-engine

mux-engine is the profile-owning WPE UI process. It is intentionally separate
from muxd: muxd owns Kitty windows, focus, the global bar, and logical layers;
mux-engine owns WebKit views and browser-session state.

## Running the transition target

The daemon is built with the other WPE spike binaries:

    meson setup build/wpe spikes/wpe-kitty
    meson compile -C build/wpe mux-engine

Run it in the foreground while developing:

    build/wpe/mux-engine --profile default

Start it idempotently in the background:

    build/wpe/mux-engine --ensure --profile default

The final launcher will start it automatically before creating Kitty panes.
Until mux-pane implements input and frame consumption, the existing mux-view
binary remains the active browser path.

## Ownership and paths

One daemon exists per profile. A flock held for the process lifetime prevents
two UI processes from opening the same WebKit profile.

Default paths are:

    socket: $XDG_RUNTIME_DIR/mux/mux-engine-PROFILE.sock
    data:   $XDG_DATA_HOME/mux/profiles/PROFILE
    cache:  $XDG_CACHE_HOME/mux/profiles/PROFILE

When XDG_RUNTIME_DIR is unavailable, the socket uses
/tmp/mux-UID/mux-engine-PROFILE.sock. Runtime, profile, and socket directories
are restricted to their owner.

Overrides:

    MUX_PROFILE
    MUX_ENGINE_SOCKET
    MUX_PROFILE_DATA_DIR
    MUX_PROFILE_CACHE_DIR

Profile names may contain ASCII letters, digits, period, underscore, and hyphen.

## Session model

The process creates:

- One persistent WebKitNetworkSession using the explicit data and cache paths.
- One WebKitNetworkSession created with webkit_network_session_new_ephemeral().
- One headless WPEDisplay shared by all WebKitWebViews.
- One WebKitWebContext shared by all WebKitWebViews.

CREATE_VIEW chooses the persistent session by default. The EPHEMERAL header flag
chooses the private session. Tracking prevention is enabled for both.

## Connection security

The listener is an owner-only SOCK_SEQPACKET Unix socket. Linux SO_PEERCRED must
report the daemon UID, and the PID claimed by HELLO must match the kernel-reported
peer PID. Packets and payloads are bounded by the engine protocol before parsing.
The current limits are 128 pane clients, 32 views per pane, and 256 views total.

## Version 1 payloads implemented by the daemon

HELLO:

    u32 pid
    string kitty_window_id
    string layer
    u32 width
    u32 height
    u32 scale_milli
    string initial_uri

WELCOME:

    u32 engine_pid
    string profile
    string socket_path

CREATE_VIEW may have an empty payload to use HELLO defaults, or:

    u32 width
    u32 height
    u32 scale_milli
    string layer
    string initial_uri

VIEW_CREATED:

    u32 width
    u32 height
    u32 scale_milli
    string layer

RESIZE:

    u32 width
    u32 height
    u32 scale_milli

NAVIGATE:

    u16 action
    string uri  # LOAD only

SET_FOCUS:

    u32 focused

METADATA:

    string uri
    string title
    string layer
    u32 is_loading
    u32 can_go_back
    u32 can_go_forward
    u32 progress_milli
    u32 width
    u32 height
    u32 scale_milli

ACK echoes the request view ID and serial with an empty payload. ERROR echoes the
same identifiers and carries a u32 remote error code followed by a string.

Lifecycle, resize, focus bookkeeping, metadata, URI loading, back, forward,
reload, and stop are implemented. Input and frame acknowledgements currently
return NOT_IMPLEMENTED; their implementation belongs to mux-pane and the
damage-driven shared-memory renderer.
