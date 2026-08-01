# Global bar and Kitty layers

Each logical layer is represented by one Kitty tab. The first process in a
layer is mux-layer, which creates a small persistent mux-bar split and then
becomes mux-view. Additional browser splits inherit the layer identity and use
the same bar.

Ctrl+L in any browser pane sends PROMPT to muxd. The daemon identifies the bar
with the same Kitty remote-control socket and layer, focuses it, and starts edit
mode. Enter sends the entered location to the active view and returns focus.
Escape cancels and returns focus without navigation.

The launcher gives every Kitty instance a distinct Linux abstract socket and
enables socket-only remote control. Views and bars register both that socket and
their KITTY_WINDOW_ID with muxd. No terminal escape scraping is used for focus.

## Layer operations

Ctrl+Shift+T creates a new tab with a generated layer name, global bar, and
browser view. Ctrl+Shift+Enter creates another browser split in the current
layer.

The control plane can switch to any existing layer:

    ./mux ctl layer layer-name

muxd selects a view in that layer and asks the correct Kitty instance to focus
its registered window. Kitty consequently activates the containing tab while
the WebKit process remains alive.

Views can be reassigned without restarting:

    ./mux ctl move VIEW-ID layer-name

The visible layer switch is implemented with the official Kitty focus-window
remote-control command. Logical layer membership remains owned by muxd rather
than inferred from Kitty tab titles.
