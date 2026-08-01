# Clipboard broker protocol

The muxd clipboard endpoint receives complete `MXEX` packets from authenticated
local peers. Channel 2 carries atomic `MXCB` snapshots and channel 3 carries
`MXCC` history commands.

## Session

The first command must be `HELLO`. It fixes one profile and one history mode for
the peer. Every later snapshot must declare exactly that profile. A profile
mismatch closes the logical session rather than looking up the declared target.

History modes are normal memory, ephemeral memory, and disabled. Disabled mode
still updates the profile's current clipboard because live copy and paste must
not depend on history retention.

## Commands

- `LIST` streams zero or more `SUMMARY` records followed by `LIST_DONE`.
- `SELECT` streams the selected full snapshot on channel 2, then returns `OK`.
- `DELETE` removes one explicit entry, pinned or not.
- `PIN` sets or clears pin state.
- `CLEAR` preserves pinned entries unless `INCLUDE_PINNED` is set.
- `BYE` acknowledges and closes the logical peer.

Every command has a nonzero request ID. Errors echo that ID with bounded UTF-8
text. Summary records contain no clipboard bytes, only provenance, age, pin
state, format count, total size, and a terminal-safe preview.

## Snapshot observation

An observed channel-2 snapshot updates current state before history insertion.
The broker acknowledges the atomic transfer even if history rejects it because
of retention limits, then emits a control error describing the history failure.
This keeps the live clipboard functional under pinned-history pressure.

`MuxClipboardBrokerPeer` implements all protocol state without owning a socket.
The muxd event loop remains responsible for `SO_PEERCRED`, owner-only socket
permissions, packet reads/writes, bounded output queues, and peer lifetime.
