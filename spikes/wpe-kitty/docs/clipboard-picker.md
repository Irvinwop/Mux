# Clipboard picker presentation

The picker is a modal terminal panel, not a replacement browser screen.
It uses at most 88 columns and 14 rows, with up to six visible history items.
Empty history uses only the rows needed for its explanation. Small terminals
drop decorative borders and secondary information before exceeding their bounds.

The current profile is named in the panel. An empty profile history and a search
with no matches have different messages. Empty history explains that copying in
this profile adds an item and that other profiles' histories remain separate.
This does not change explicit system-clipboard paste behavior.

Action hints describe the current state:

- Escape closes the picker, including during an asynchronous request.
- Paste, pin/unpin, and delete are offered only with a selection.
- Clear unpinned history is offered only when an unpinned item exists.
- Clear search is offered only when a query is present.
- Busy requests show their status and do not advertise unavailable mutations.

The model still preserves source metadata, previews, full format counts, fuzzy
MIME search, pinned ordering, and entry identity. Controller serial matching,
broker authorization, profile isolation, and complete multi-MIME selection are
unchanged.

## Terminal integration

The renderer emits opaque panel cells and CRLF only between rows. It never emits
a final newline, so filling the final terminal row cannot scroll away the header.
The presentation helper positions each row explicitly and preserves the cursor.
It clears only the panel's old row footprint, not the entire screen, and remembers
damage hidden by a smaller viewport until a later resize can clear it.

While the picker is open, the pane keeps its existing browser image placement
behind the panel and continues to suspend page input and engine visibility.
This preserves the last available page as context; it does not request a new
background frame or reconstruct an image removed while the pane was hidden.
Closing retains the existing engine visibility and resize/resynchronization path.

The portable test-clipboard-picker target covers model states, controller hints,
bounded opaque rendering, cursor/scroll safety, and resize/close footprint cleanup.
Real Kitty/Weston screenshots remain necessary for visual confirmation.
