# Clipboard wire and broker

Clipboard payloads are not encoded into muxd's line-oriented control commands.
They use the binary `MXCB` protocol over authenticated Unix-domain packet
channels and over the engine/pane extension channel.

## Snapshot transaction

Every transaction is ordered and atomic:

1. `SNAPSHOT_BEGIN` declares profile, origin, source view, flags, MIME count,
   serial, timestamp, and total bytes.
2. `ITEM_BEGIN` declares one MIME type and its exact byte count.
3. One or more `ITEM_DATA` records carry at most 192 KiB each.
4. `SNAPSHOT_COMMIT` publishes the snapshot only after every declared byte has
   arrived.
5. `CANCEL` discards partial state.

The fixed 64-byte header and all integers are big-endian. Packets are bounded
at 256 KiB, snapshots at 32 MiB, items at 16 MiB, and MIME count at 32. The
assembler has one in-flight transaction and a ten-second inactivity timeout.
Malformed, reordered, duplicated, oversized, and incomplete transactions fail
closed without exposing a partial clipboard.

The protocol transfers raw bytes. It does not base64 clipboard data and does
not interpret a payload according to its MIME type.

## muxd broker

`MuxClipboardBroker` is the in-process ownership boundary intended for muxd.
Profiles must be registered before use and are exact hash-map keys. There is no
API that accepts one profile while returning another profile's current value or
history.

The broker maintains two related states per profile:

- Current clipboard: a complete immutable snapshot used by pane and engine
  synchronization even when history is disabled.
- History: a bounded `MuxClipboardHistory` controlled by disabled, memory, or
  ephemeral mode.

Observing a snapshot updates current state first. A history-limit failure does
not break the live clipboard. Selecting a history entry atomically makes it
current and returns a referenced snapshot for distribution to the focused pane
and its engine view.

Picker list responses contain only IDs, timestamps, source metadata, pin state,
format count, total size, and terminal-safe previews. Full bytes are transferred
only after selection.
