# Mux control plane

The project now has a small central process instead of relying on Kitty panes
to discover one another.

## Processes

- muxd owns the live view registry, active view, current logical layer, and a
  monotonically increasing state revision.
- mux-view registers its UUID, process ID, Kitty window ID, layer, URI, title,
  and focus state. It also accepts navigation commands from muxd.
- muxctl is the human and scripting interface.

The launcher starts muxd automatically. Its Unix socket is stored at
XDG_RUNTIME_DIR/mux/muxd.sock, or a per-user directory under the system
temporary directory when XDG_RUNTIME_DIR is unavailable. The socket and its
directory are owner-only.

## Commands

Run control commands through the repository launcher so the incremental build
directory does not need to be added to the login PATH:

    ./mux ctl list
    ./mux ctl status
    ./mux ctl open https://example.com
    ./mux ctl reload
    ./mux ctl focus VIEW-ID
    ./mux ctl layer research
    ./mux ctl move VIEW-ID research
    ./mux ctl watch

Navigation commands target the active view by default. Supplying a view UUID
targets that view directly.

## Wire boundary

The protocol is newline-delimited and versioned. Structural fields are
tab-separated; user-controlled strings are base64 encoded. Persistent view and
subscriber connections coexist with one-shot control connections.

Subscribers receive an initial BEGIN, VIEW, END snapshot followed by revisioned
EVENT records. This is the input contract for the dedicated global URL-bar
process, so the bar will not need to scrape Kitty titles or inspect browser
processes.

Logical layer state is centralized now, but this slice does not yet hide and
show Kitty panes. The next layer adapter will translate muxd layer changes into
Kitty remote-control operations while keeping browser processes alive.
