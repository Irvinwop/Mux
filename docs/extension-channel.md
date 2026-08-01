# Engine extension channel

Engine protocol message type `64` carries one `MXEX` envelope. The envelope
prevents UI, clipboard, broker, and diagnostic payloads from competing for a
single untyped byte stream.

The envelope is 24 bytes, versioned, big-endian, and bounded to the engine
protocol's 256 KiB packet limit. V1 defines these channels:

- `1`: browser UI request, response, and cancellation records.
- `2`: pane/engine clipboard snapshot transactions.
- `3`: pane/muxd clipboard broker requests and responses.
- `4`: bounded diagnostics.

Unknown channels and nonzero v1 flags fail closed. Channel handlers receive a
borrowed immutable `GBytes`; they cannot retain it without taking a reference.

`MuxExtensionRouter` wraps outgoing payloads and dispatches incoming envelopes.
Handlers are independently reference counted, so replacing a handler during a
callback cannot free the callback's state before it returns. The router itself
also holds a guard reference across output and dispatch callbacks.

The live executable hooks are intentionally small:

1. Engine and pane register their UI adapter on channel 1.
2. Engine and pane register their clipboard link on channel 2.
3. Their router output callback sends engine message type `64` with the encoded
   envelope as its payload.
4. Receiving message type `64` calls `mux_extension_router_dispatch()`.

Clipboard wire data chunks are 192 KiB, leaving room for both the `MXCB` and
`MXEX` headers below the outer engine packet limit.
