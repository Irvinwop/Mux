# Picker broker adapter

`mux-clipboard-picker-broker.[ch]` binds the asynchronous picker controller to
one serialized clipboard broker client without exposing that client's callback
shape to the terminal UI.

The adapter owns the client, controller, and streamed list accumulator. Its
small operation table maps list, select, pin, delete, clear, and cancel directly
onto the concrete `MuxClipboardBrokerClient` API. The final pane integration is
therefore limited to constructing this table and forwarding decoded client
callbacks into the adapter completion functions.

## Request rules

- Exactly one broker request may be active.
- Every active request is paired with the controller's nonzero serial.
- Cancellation clears adapter state before touching the client, so a reentrant
  or late callback is ignored.
- List summaries are converted immediately into immutable picker items and are
  published atomically only on list completion.
- Mutation completion triggers the controller's automatic list refresh.
- Selection is not successful merely because the broker delivered bytes. The
  pane's apply callback must first install the complete multi-MIME snapshot in
  the Kitty clipboard link and request paste into the focused page.
- Closing the picker invalidates any later result from the cancelled request.

The controller itself remains private to the adapter. Pane code opens, closes,
renders, and sends keys through the adapter API, which prevents independent
controller references from outliving its broker backend.
