# Clipboard broker transport binding

`mux-clipboard-broker-transport.[ch]` binds one clipboard broker protocol object
to an authenticated local endpoint. It supports both client-side connections and
server-side connections already accepted by muxd.

Construction order is intentional:

1. Create and attach the local endpoint.
2. Mark output available.
3. Invoke the protocol factory.
4. Permit the factory to queue its initial HELLO without input reentry.
5. Start the optional monotonic timeout tick source.

The protocol operation table contains packet handling, timeout ticking,
disconnect notification, and destruction. Thin factory wrappers adapt the
existing `MuxClipboardBrokerClient` and `MuxClipboardBrokerPeer` APIs to this
table without adding another wire format.

Incoming socket packets are passed directly to the protocol object as complete
extension envelopes. Protocol output calls
`mux_clipboard_broker_transport_send`, which only queues the packet. A packet
parse failure, EOF, transport failure, or timeout closes the endpoint and emits
one disconnect notification. Explicit owner shutdown is silent.

Factory-data ownership transfers only after both endpoint and protocol creation
succeed. This lets muxd reject a peer or a pane report connection failure
without ambiguous cleanup responsibility.
