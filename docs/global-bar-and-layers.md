# Global bar and Kitty layers

Each logical layer is represented by a Kitty tab. `mux-layer` creates one
persistent `mux-bar` split and an initial `mux-pane`; additional browser splits
run additional panes in the same layer. WPE/WebKit remains in `mux-engine`, not
in the bar, layer process, pane, or `muxd`.

## URL bar

`Ctrl+L` in a browser pane sends `PROMPT` to `muxd`. The daemon identifies the
registered bar on the same Kitty control socket and layer, focuses it, and asks
it to enter edit mode. Enter sends the location as a structured request to the
active view. Escape cancels and returns focus without navigation.

The bar subscribes to `muxd` for active URI and page state. It does not scrape
terminal output or Kitty titles, and location text is never interpolated into a
shell command.

## Kitty control boundary

Each launched Kitty instance receives a distinct filesystem socket below an
owner-only runtime directory. The launcher supplies a scoped remote-control
password permitting only the actions Mux needs:

- `focus-window` for URL-bar and layer focus.
- `launch` for bars, panes, and new splits or tabs.
- `detach-window` for physical moves between live tabs.

Mux does not use a Linux abstract socket, unrestricted remote control, or the
user's ordinary Kitty instance. Panes and bars register the socket identity and
their `KITTY_WINDOW_ID` with `muxd`.

## Layer operations

The configured Kitty mappings can create a new tab/layer or another browser
split in the current layer. The control plane can focus an existing layer with:

```sh
./mux ctl layer LAYER-NAME
```

`muxd` chooses a live registered view in that layer and invokes Kitty's
documented `focus-window` action. Kitty activates the containing tab while the
profile engine and WebKit view remain alive.

A live pane can be moved with:

```sh
./mux ctl move VIEW-ID LAYER-NAME
```

Mux invokes `detach-window` only when the requested layer already contains a
live target window on the same Kitty socket. Metadata changes only after Kitty
reports success. An empty target layer, a cross-Kitty request, a timeout, or a
rejected move returns an error and leaves the source unchanged.

Logical layer membership belongs to `muxd`; it is not inferred from Kitty tab
titles. v0.1 assumes one dedicated Kitty UI and does not automatically recreate
missing tabs, bars, panes, split geometry, or layouts from persisted records.
That restoration workflow remains a target, not an implemented command.
