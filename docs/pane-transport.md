# Pane transport

mux-pane is the thin Kitty-facing half of the central engine architecture.
It does not link WebKit and never receives page pixels through its Unix socket.

## Frame path

1. The custom WPEView in mux-engine imports the rendered WPEBuffer.
2. Only the union of WebKit damage rectangles is converted from ARGB to RGBA.
3. The engine keeps one current RGBA surface per view.
4. One compact damaged rectangle is copied into a uniquely named owner-only
   POSIX shared-memory object.
5. FRAME sends dimensions, damage, object name, and byte size over the engine
   SOCK_SEQPACKET connection.
6. mux-pane submits the object to Kitty with t=s and f=32.
7. The first frame uses transmit-and-display. Later frames edit root frame 1
   with a=f, r=1, and replacement composition X=1.
8. Kitty unlinks the object after reading it and replies with the image ID.
9. mux-pane sends FRAME_ACK. Only then may the engine send another frame.

If WPE renders while Kitty has not acknowledged the previous upload, the engine
updates its in-memory surface and coalesces damage rather than queueing stale
frames. A five-second timeout unlinks abandoned shared memory and releases the
frame gate if a pane or terminal fails to answer.

This transport does not encode PNGs, base64-encode pixels, poll the page, or push
unchanged frames. Base64 is used only for the short shared-memory object name
required by Kitty's control protocol.

## Input path

mux-pane enables Kitty's enhanced keyboard protocol, SGR pixel mouse reporting,
focus reporting, and terminal resize notifications. It translates terminal
events into versioned INPUT_KEY, INPUT_POINTER, SET_FOCUS, and RESIZE packets.

The engine constructs WPE keyboard, pointer-button, pointer-motion, scroll, and
focus events on the WPEView that belongs to the authenticated pane. The initial
implementation handles Unicode input, release and repeat events, common function
keys, pixel mouse movement, buttons, wheel scrolling, focus, and resize.

## Control integration

mux-pane registers as a normal VIEW client with muxd. URI and title metadata
update the global bar, focus events update the active view and layer, Ctrl+L
focuses the layer bar, and DO commands from muxctl or the bar are translated to
engine navigation. The Kitty configuration and layer launcher now start
mux-pane; mux-view remains available only as the direct-rendering fallback.

If the profile engine is not running, mux-pane starts the sibling mux-engine
binary with --ensure and waits for its owner-only socket. Concurrent panes are
safe because mux-engine holds a profile lock before opening WebKit storage.

Frame timeout recovery is deliberately fail-closed. A terminal that does not
acknowledge an upload within thirty seconds loses its engine connection. The
engine never sends another frame under the same image ID while a stale Kitty
response could be outstanding, so an old response cannot acknowledge newer
shared memory.
