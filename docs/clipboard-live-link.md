# Clipboard live-link adapters

The clipboard implementation is split into profile-wide engine state and
pane-local terminal state.

## Engine link

`MuxClipboardEngineLink` owns the `MuxWpeClipboard` returned by the custom
display's `get_clipboard` virtual method. WebKit writes are serialized as atomic
`MXCB` transactions with active view and origin provenance. Incoming pane
snapshots replace the synchronous WPE read cache before an optional paste
callback runs, so WebKit never receives a paste event before its clipboard data
is available.

The engine daemon must create one link per profile before creating web views.
Its custom `WPEDisplayClass.get_clipboard` implementation returns
`mux_clipboard_engine_link_get_clipboard()`.

## Pane link

`MuxClipboardPaneLink` owns `MuxKittyClipboard`. It routes OSC 5522 capability,
read, paste-offer, and write traffic to the terminal and routes complete binary
snapshots to the engine extension channel.

Snapshots copied by WebKit travel engine to pane, are written to Kitty, and are
offered to muxd for history. Snapshots read from Kitty travel pane to engine,
are cached by WPE, and are also offered to muxd. Primary-selection provenance
is retained as a wire flag.

History selection uses `mux_clipboard_pane_link_apply_history()`. It writes the
selected content to Kitty and updates the engine cache without recording a new
history entry. If paste was requested, the engine link invokes its paste
callback only after replacing the WPE cache.

## Remaining event-loop hooks

The existing loops need only provide callbacks for:

- Sending one raw `MXCB` packet through the engine extension message.
- Writing one OSC/CSI packet to the pane TTY.
- Forwarding observed snapshots to muxd's clipboard packet endpoint.
- Synthesizing paste into the selected engine view.

Both links enforce exact profile equality before applying a transfer. Partial
or timed-out snapshots are discarded and never enter WPE, Kitty, or history.
