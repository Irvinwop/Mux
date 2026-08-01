#include "mux-clipboard.h"

#include <string.h>

typedef struct {
    gchar *mime;
    GBytes *bytes;
} ClipboardItem;

struct _MuxClipboardSnapshot {
    gint reference_count;
    guint64 serial;
    GPtrArray *items;
    gsize total_bytes;
    gboolean sealed;
};

G_DEFINE_QUARK(mux-clipboard-error-quark, mux_clipboard_error)

static void
clipboard_item_free(ClipboardItem *item)
{
    if (item == NULL)
        return;

    g_free(item->mime);
    g_clear_pointer(&item->bytes, g_bytes_unref);
    g_free(item);
}

gboolean
mux_clipboard_mime_is_valid(const gchar *mime)
{
    gsize length;
    gsize i;

    if (mime == NULL || mime[0] == '\0')
        return FALSE;

    length = strlen(mime);
    if (length > MUX_CLIPBOARD_MAX_MIME)
        return FALSE;

    for (i = 0; i < length; i++) {
        if ((guchar)mime[i] < 0x21 || (guchar)mime[i] > 0x7e)
            return FALSE;
    }

    return TRUE;
}

MuxClipboardSnapshot *
mux_clipboard_snapshot_new(guint64 serial)
{
    MuxClipboardSnapshot *snapshot = g_new0(MuxClipboardSnapshot, 1);

    snapshot->reference_count = 1;
    snapshot->serial = serial;
    snapshot->items =
        g_ptr_array_new_with_free_func((GDestroyNotify)clipboard_item_free);
    return snapshot;
}

MuxClipboardSnapshot *
mux_clipboard_snapshot_ref(MuxClipboardSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, NULL);
    g_atomic_int_inc(&snapshot->reference_count);
    return snapshot;
}

void
mux_clipboard_snapshot_unref(MuxClipboardSnapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    if (!g_atomic_int_dec_and_test(&snapshot->reference_count))
        return;

    g_ptr_array_unref(snapshot->items);
    g_free(snapshot);
}

GBytes *
mux_clipboard_snapshot_find(const MuxClipboardSnapshot *snapshot,
                            const gchar *mime)
{
    guint i;

    g_return_val_if_fail(snapshot != NULL, NULL);
    if (mime == NULL)
        return NULL;

    for (i = 0; i < snapshot->items->len; i++) {
        ClipboardItem *item = g_ptr_array_index(snapshot->items, i);

        if (g_str_equal(item->mime, mime))
            return item->bytes;
    }

    return NULL;
}

gboolean
mux_clipboard_snapshot_add(MuxClipboardSnapshot *snapshot,
                           const gchar *mime,
                           GBytes *bytes,
                           GError **error)
{
    ClipboardItem *item;
    gsize length;

    g_return_val_if_fail(snapshot != NULL, FALSE);

    if (snapshot->sealed) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_SEALED,
                            "clipboard snapshot is sealed");
        return FALSE;
    }
    if (!mux_clipboard_mime_is_valid(mime) || bytes == NULL) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_INVALID,
                            "clipboard item is invalid");
        return FALSE;
    }
    if (mux_clipboard_snapshot_find(snapshot, mime) != NULL) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_INVALID,
                            "clipboard MIME type is duplicated");
        return FALSE;
    }
    if (snapshot->items->len >= MUX_CLIPBOARD_MAX_ITEMS) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_LIMIT,
                            "clipboard snapshot has too many items");
        return FALSE;
    }

    length = g_bytes_get_size(bytes);
    if (length > MUX_CLIPBOARD_MAX_ITEM_BYTES ||
        length > MUX_CLIPBOARD_MAX_TOTAL_BYTES - snapshot->total_bytes) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_LIMIT,
                            "clipboard snapshot exceeds its byte limit");
        return FALSE;
    }

    item = g_new0(ClipboardItem, 1);
    item->mime = g_strdup(mime);
    item->bytes = g_bytes_ref(bytes);
    g_ptr_array_add(snapshot->items, item);
    snapshot->total_bytes += length;
    return TRUE;
}

MuxClipboardSnapshot *
mux_clipboard_snapshot_new_sealed_from_items(
    guint64 serial,
    const MuxClipboardSnapshotItem *items,
    guint item_count,
    GError **error)
{
    MuxClipboardSnapshot *snapshot;
    guint i;

    if (item_count > MUX_CLIPBOARD_MAX_ITEMS ||
        (item_count > 0 && items == NULL)) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_ERROR,
                            MUX_CLIPBOARD_ERROR_LIMIT,
                            "clipboard snapshot has too many items");
        return NULL;
    }

    snapshot = mux_clipboard_snapshot_new(serial);
    for (i = 0; i < item_count; i++) {
        if (!mux_clipboard_snapshot_add(snapshot,
                                        items[i].mime,
                                        items[i].bytes,
                                        error)) {
            mux_clipboard_snapshot_unref(snapshot);
            return NULL;
        }
    }
    mux_clipboard_snapshot_seal(snapshot);
    return snapshot;
}

void
mux_clipboard_snapshot_seal(MuxClipboardSnapshot *snapshot)
{
    g_return_if_fail(snapshot != NULL);
    snapshot->sealed = TRUE;
}

gboolean
mux_clipboard_snapshot_is_sealed(const MuxClipboardSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, FALSE);
    return snapshot->sealed;
}

MuxClipboardSnapshot *
mux_clipboard_snapshot_dup_sealed(const MuxClipboardSnapshot *snapshot)
{
    MuxClipboardSnapshot *copy;
    guint i;

    g_return_val_if_fail(snapshot != NULL, NULL);

    copy = mux_clipboard_snapshot_new(snapshot->serial);
    for (i = 0; i < snapshot->items->len; i++) {
        ClipboardItem *item = g_ptr_array_index(snapshot->items, i);

        if (!mux_clipboard_snapshot_add(copy,
                                        item->mime,
                                        item->bytes,
                                        NULL)) {
            mux_clipboard_snapshot_unref(copy);
            return NULL;
        }
    }
    mux_clipboard_snapshot_seal(copy);
    return copy;
}

guint64
mux_clipboard_snapshot_get_serial(const MuxClipboardSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, 0);
    return snapshot->serial;
}

guint
mux_clipboard_snapshot_get_count(const MuxClipboardSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, 0);
    return snapshot->items->len;
}

gsize
mux_clipboard_snapshot_get_total_bytes(const MuxClipboardSnapshot *snapshot)
{
    g_return_val_if_fail(snapshot != NULL, 0);
    return snapshot->total_bytes;
}

gboolean
mux_clipboard_snapshot_get_item(const MuxClipboardSnapshot *snapshot,
                                guint index,
                                const gchar **mime,
                                GBytes **bytes)
{
    ClipboardItem *item;

    g_return_val_if_fail(snapshot != NULL, FALSE);
    if (index >= snapshot->items->len)
        return FALSE;

    item = g_ptr_array_index(snapshot->items, index);
    if (mime != NULL)
        *mime = item->mime;
    if (bytes != NULL)
        *bytes = item->bytes;
    return TRUE;
}
