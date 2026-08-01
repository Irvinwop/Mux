# Concrete clipboard broker bindings

`mux-clipboard-broker-bindings.[ch]` adapts the existing broker client and peer
objects to `MuxClipboardBrokerTransport`. It introduces no messages and performs
no additional serialization.

## Client binding

The client connector opens the owner-authenticated `clipboard.sock`, constructs
`MuxClipboardBrokerClient`, and starts its HELLO handshake while factory-time
output is already available. Incoming endpoint packets are passed to
`mux_clipboard_broker_client_handle_packet`. A 250 ms monotonic tick expires
requests through the client's existing timeout machinery.

Ready, list, selection, mutation, and failure callbacks preserve their original
borrowed-lifetime contracts. Transport disconnect is surfaced through the same
failure callback with operation `transport`. The transport owns callback data
only when complete construction succeeds.

## Peer binding

Muxd passes each accepted connection and its process-wide clipboard broker to
the peer factory. Incoming packets feed `MuxClipboardBrokerPeer`; peer output is
queued through the endpoint. The broker is borrowed because muxd owns it for
longer than every accepted peer. Peer timeout failure closes only that peer.

The typed getters expose the concrete client or peer when pane and muxd code
need their operation APIs. They validate a private protocol-kind marker so a
client transport cannot accidentally be treated as a peer transport.
