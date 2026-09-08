# Clipboard history

Mux clipboard history is a control-plane feature, not a Web API. Web content
can observe only the current `WPEClipboard` through WebKit's normal permission
and user-gesture rules. It cannot list, select, pin, or delete history entries.

## Ownership

`muxd` owns one `MuxClipboardHistory` per profile. The history constructor takes
the profile once, and additions cannot provide a different profile. This makes
cross-profile insertion impossible through the data-model API.

Modes are:

- `MUX_CLIPBOARD_HISTORY_DISABLED` for private panes or profiles that opt out.
- `MUX_CLIPBOARD_HISTORY_MEMORY` for normal profiles.
- `MUX_CLIPBOARD_HISTORY_EPHEMERAL` for isolated in-memory profiles.

No mode persists entries to disk. A future persistence feature must be explicit,
encrypted, bounded, and separate from the v1 API.

## Limits and retention

Each profile keeps at most 25 entries and 16 MiB. Insertion evicts the oldest
unpinned entries. If pinned entries consume the limits, insertion fails rather
than silently deleting pinned data.

V1 stores every MIME variant in an observed clipboard snapshot, including
images and arbitrary binary formats. An entry is atomic: Mux stores all of its
variants or none of them. A snapshot larger than the profile's 16 MiB history
budget is rejected rather than partially retained under a misleading MIME list.

Picker previews are deliberately narrower than storage. Valid `text/plain` or
`text/uri-list` data is normalized into terminal-safe text with control bytes
removed. Other content is represented by MIME type, total size, and format
count; binary payloads are never printed into the terminal.

Identical snapshots are deduplicated. The existing entry keeps its ID
and pin state, receives the latest source metadata, and moves to the front.

## Provenance

History metadata wraps, rather than modifies, the transport snapshot:

```c
typedef struct {
    guint64 id;
    gint64 created_us;
    gchar *profile;
    gchar *source_origin;
    guint64 source_view_id;
    MuxClipboardSnapshot *snapshot;
    gboolean pinned;
} MuxClipboardHistoryEntry;
```

The source origin is display metadata only. It does not grant that origin
clipboard permission.

## Picker flow

The `Super+Shift+V` flow (`Ctrl+Shift+V` as a Linux compatibility alias) is:

1. The focused pane asks `muxd` for summaries from its profile history.
2. The pane renders a Kitty-native fuzzy picker over the negative-z web frame.
3. Selecting an entry asks `muxd` to make its snapshot current.
4. `muxd` sends the snapshot to the focused pane's Kitty clipboard adapter and
   the engine-side WPE clipboard cache.
5. The pane synthesizes the paste only after both updates have been accepted.

The picker will expose explicit pin, unpin, delete, and clear actions. Clearing
can either preserve pinned entries or remove everything after confirmation.

History records only clipboard traffic observed by Mux. It is not promised to
mirror every external operating-system clipboard change.
