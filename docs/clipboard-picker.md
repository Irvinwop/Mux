# Kitty clipboard history picker

`mux-clipboard-picker.[ch]` is the terminal-facing model for clipboard history.
It renders ANSI text directly in a Kitty pane; it does not submit graphics or
periodic image frames.

The picker receives immutable summaries from the muxd broker. Summaries include
the entry ID, age, source origin and view, pin state, total byte size, safe text
preview, and every advertised MIME type. Arbitrary binary formats remain fully
selectable even when their visible preview is only `[binary data]`.

## Search and ordering

Typing performs case-folded fuzzy subsequence search across the preview, source
origin, and MIME names. Consecutive characters and token boundaries receive a
score bonus. Equal matches prefer pinned entries, then newer entries. The
selected immutable entry ID survives list refreshes whenever that entry still
exists.

## Pane key mapping

The pane adapter will open the picker with `Ctrl+Shift+V` and map Kitty keyboard
events as follows:

| Key | Picker command |
| --- | --- |
| Printable text | Extend query |
| Backspace | Delete character |
| `Ctrl+W` | Delete query word |
| `Ctrl+U` | Clear query |
| Up/Down | Move selection |
| Page Up/Page Down | Move one visible page |
| Home/End | First/last match |
| Enter | Select and paste entry |
| `Alt+P` | Toggle pin on selected entry |
| `Alt+D` | Delete selected entry |
| `Alt+C` | Clear unpinned history |
| Escape | Close without changing clipboard |

The model emits actions rather than mutating history itself. `mux-pane` sends
those actions through `MuxClipboardBrokerClient`; muxd remains the only owner of
history state. A successful selection returns the complete multi-MIME snapshot,
makes it current through the Kitty clipboard link, and requests paste into the
focused page.
