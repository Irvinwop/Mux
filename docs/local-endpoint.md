# Local endpoint ownership

`mux-local-endpoint.[ch]` is the executable-facing owner for one authenticated
local transport connection. It combines a `MuxLocalConnection`, its custom GLib
source, packet callbacks, output queue wakeups, and teardown into one refcounted
object.

Protocol output calls `mux_local_endpoint_send`. That function only appends one
complete packet to the bounded transport queue and wakes the main context. It
never dispatches input, which is the non-reentry guarantee required by the
clipboard broker client and picker controller.

Incoming packets are delivered in socket order. Returning false from the packet
callback closes the connection as a protocol failure. EOF and transport errors
mark the endpoint closed and invoke the optional end callback exactly once.
Explicit local close is silent.

The endpoint uses a source-owned weak hook rather than making the source and
endpoint reference each other. This permits the final external reference to be
released from inside a packet or end callback without leaving a cycle or giving
the source a dangling endpoint pointer.

Callback-data ownership transfers only after source construction succeeds. A
failed connection or source setup leaves that data with the caller while still
releasing every internal descriptor and connection reference.

## Clipboard use

A pane or engine creates a client endpoint for the muxd clipboard service. Its
broker-client output callback queues the encoded extension envelope through the
endpoint, and its endpoint packet callback passes the complete envelope into the
broker client. On disconnect, the owner fails the active picker operation and
recreates both endpoint and protocol client before accepting another request.

Muxd adopts each connection accepted by its listener source into the same
endpoint type. The endpoint packet callback feeds one `MuxClipboardBrokerPeer`;
the peer's output callback queues responses back through that endpoint.
