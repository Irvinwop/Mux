# Clipboard picker controller

`mux-clipboard-picker-controller.[ch]` is the asynchronous boundary between
`mux-pane` input/rendering and `MuxClipboardBrokerClient` requests. It owns a
picker model but never owns clipboard history or a clipboard snapshot.

The controller assigns a nonzero serial to every request and accepts only the
completion matching its active serial and operation kind. Closing, reopening,
or replacing a request invalidates stale completions. Backend functions are
expected to enqueue protocol output and return immediately; the authenticated
local transport's queue callback satisfies that requirement without reentry.
Request transitions retain the controller across redraw and backend callbacks.
If a redraw closes the overlay, the pending operation is cancelled before its
backend function is invoked.
The controller tracks backend invocation separately from visible pending state,
so closing during that pre-send redraw never emits a cancellation for a request
that was not handed to the broker client.

## State flow

```text
CLOSED -> LOADING -> READY
                    |   |
                    |   +-> MUTATING -> LOADING -> READY
                    +-----> SELECTING -> CLOSED
```

Failures return to `READY` with a visible, control-character-safe status. A
failed initial list leaves an empty picker open so Escape still works and a
subsequent open can retry. Mutation success automatically requests a fresh list
instead of predicting muxd state locally.

The backend contract has six operations: list, select, set-pinned, delete,
clear, and optional cancel. A pane adapter translates those directly to broker
client calls. Successful select completion means the adapter has already
received and applied the complete multi-MIME snapshot to Kitty and requested a
paste; only then does the controller close the overlay.
