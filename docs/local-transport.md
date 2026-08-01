# Authenticated local transport

Mux's control and browser-affordance protocols use a shared local transport
implemented by `mux-local-transport.[ch]`. The transport deliberately does not
know about clipboard records, UI prompts, browser views, or muxd commands.

## Socket model

- Linux `AF_UNIX` plus `SOCK_SEQPACKET` preserves one protocol envelope per
  packet.
- Services live below `$XDG_RUNTIME_DIR/mux`, with `/tmp/mux-UID` as the
  fallback.
- Both the runtime directory and service socket are owner-only.
- A listener verifies every accepted peer with `SO_PEERCRED` before exposing
  the connection to a protocol handler.
- A client verifies the server uid in the same way after connecting.
- Stale sockets are removed only when the path is an owner-owned socket and a
  connection probe reports that no service is listening.
- Listener teardown compares the socket inode before unlinking, so it cannot
  remove a replacement listener's path.

On BSD-family systems `getpeereid` supplies uid/gid authentication, without a
peer pid. Linux remains the supported runtime target.

## Flow control

Each connection has independent maximum-packet and maximum-queued-byte limits.
Queueing a packet never calls a protocol callback. A GLib event source asks the
connection for its desired `GIOCondition`, calls dispatch when ready, then
updates the source condition. Dispatch drains complete input packets and flushes
whole output packets until the descriptor would block.

`mux-local-source.[ch]` supplies the corresponding custom `GSource` adapters.
The connection source dynamically enables writable polling only while output is
queued. Its queue helper also wakes the owning main context, so packets queued
outside a socket callback are not stranded. The listener source drains accepts,
authenticates each descriptor before invoking application code, and keeps the
listener alive for exactly the source lifetime.

The default packet ceiling accommodates a 256 KiB extension envelope plus its
header. The default four-MiB output limit bounds a stalled peer independently
of the clipboard history's own storage limits.

An oversized incoming packet is consumed by the kernel, rejected, and closes
the connection. Partial seqpacket writes are treated as transport corruption.

## Intended wiring

`muxd` owns the clipboard listener. One `MuxClipboardBrokerPeer` is attached to
each authenticated pane or engine connection. Its output callback only queues
an encoded extension packet, preventing synchronous response reentry into a
client request.

`mux-pane` and `mux-engine` each attach one `MuxClipboardBrokerClient` to their
connection. Their existing clipboard wire records remain unchanged: the local
transport carries extension envelopes, the extension router selects the
clipboard or broker channel, and the clipboard assembler reconstructs complete
multi-MIME snapshots.
