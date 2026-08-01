# Clipboard broker client

`MuxClipboardBrokerClient` is the pane-side state machine for muxd clipboard
history. It is socket agnostic and sends complete `MXEX` envelopes through one
callback.

The client serializes control requests. This is intentional: the picker cannot
meaningfully issue another list, selection, or mutation while an earlier one is
unresolved. Clipboard observations remain independent and can be streamed while
a control request is pending.

Startup sends `HELLO` with the pane's immutable profile and history mode. APIs
for list, select, pin, delete, clear, and close become available only after muxd
acknowledges the session.

List results are assembled from bounded `SUMMARY` records and delivered only
after `LIST_DONE`. A selection completes only when both the full atomic channel
2 snapshot and the matching channel 3 `OK` have arrived. The client validates
profile and paste flags before exposing that snapshot to the pane.

All control requests and selected snapshot transfers have ten-second inactivity
timeouts. Callback values are borrowed for callback duration; callers that need
to retain a snapshot must take a reference.

The pane integration maps callbacks as follows:

- List callback opens or refreshes the Kitty-native fuzzy picker.
- Select callback calls `mux_clipboard_pane_link_apply_history()`.
- Mutation callback refreshes the picker.
- Pane clipboard observation calls `mux_clipboard_broker_client_observe()`.
