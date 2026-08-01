#ifndef MUX_CLIPBOARD_H
#define MUX_CLIPBOARD_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define MUX_CLIPBOARD_MAX_ITEMS 32U
#define MUX_CLIPBOARD_MAX_MIME 512U
#define MUX_CLIPBOARD_MAX_ITEM_BYTES (16U * 1024U * 1024U)
#define MUX_CLIPBOARD_MAX_TOTAL_BYTES (32U * 1024U * 1024U)

typedef enum {
    MUX_CLIPBOARD_ERROR_INVALID,
    MUX_CLIPBOARD_ERROR_LIMIT,
    MUX_CLIPBOARD_ERROR_SEALED
} MuxClipboardError;

#define MUX_CLIPBOARD_ERROR (mux_clipboard_error_quark())
GQuark mux_clipboard_error_quark(void);

typedef struct _MuxClipboardSnapshot MuxClipboardSnapshot;

typedef struct {
    const gchar *mime;
    GBytes *bytes;
} MuxClipboardSnapshotItem;

MuxClipboardSnapshot *mux_clipboard_snapshot_new(guint64 serial);
MuxClipboardSnapshot *mux_clipboard_snapshot_ref(MuxClipboardSnapshot *snapshot);
void mux_clipboard_snapshot_unref(MuxClipboardSnapshot *snapshot);

gboolean mux_clipboard_snapshot_add(MuxClipboardSnapshot *snapshot,
                                    const gchar *mime,
                                    GBytes *bytes,
                                    GError **error);

/* Builds and seals the snapshot atomically, returning NULL on any bad item. */
MuxClipboardSnapshot *mux_clipboard_snapshot_new_sealed_from_items(
    guint64 serial,
    const MuxClipboardSnapshotItem *items,
    guint item_count,
    GError **error);

void mux_clipboard_snapshot_seal(MuxClipboardSnapshot *snapshot);
gboolean mux_clipboard_snapshot_is_sealed(const MuxClipboardSnapshot *snapshot);

MuxClipboardSnapshot *mux_clipboard_snapshot_dup_sealed(
    const MuxClipboardSnapshot *snapshot);

guint64 mux_clipboard_snapshot_get_serial(const MuxClipboardSnapshot *snapshot);
guint mux_clipboard_snapshot_get_count(const MuxClipboardSnapshot *snapshot);
gsize mux_clipboard_snapshot_get_total_bytes(const MuxClipboardSnapshot *snapshot);

gboolean mux_clipboard_snapshot_get_item(const MuxClipboardSnapshot *snapshot,
                                         guint index,
                                         const gchar **mime,
                                         GBytes **bytes);

GBytes *mux_clipboard_snapshot_find(const MuxClipboardSnapshot *snapshot,
                                    const gchar *mime);

gboolean mux_clipboard_mime_is_valid(const gchar *mime);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardSnapshot,
                              mux_clipboard_snapshot_unref)

G_END_DECLS

#endif
