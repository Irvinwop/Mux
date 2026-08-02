#include "mux-wpe-clipboard.h"

#include <string.h>

struct _MuxWpeClipboard {
    WPEClipboard parent_instance;
    MuxClipboardSnapshot *external;
    MuxWpeClipboardPublishBeginFunc publish_begin_func;
    MuxWpeClipboardPublishFunc publish_func;
    GDestroyNotify publication_data_destroy;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    guint64 next_serial;
};

G_DEFINE_TYPE(MuxWpeClipboard, mux_wpe_clipboard, WPE_TYPE_CLIPBOARD)

static gpointer
bounded_realloc(gpointer data, gsize size)
{
    if (size > MUX_CLIPBOARD_MAX_ITEM_BYTES)
        return NULL;
    return g_realloc(data, size);
}

static GBytes *
serialize_content(WPEClipboardContent *content,
                  const gchar *format,
                  GError **error)
{
    GBytes *bytes;
    const gchar *text;
    GOutputStream *stream;
    gsize length;

    bytes = wpe_clipboard_content_get_bytes(content, format);
    if (bytes != NULL) {
        if (g_bytes_get_size(bytes) > MUX_CLIPBOARD_MAX_ITEM_BYTES) {
            g_set_error_literal(error,
                                MUX_CLIPBOARD_ERROR,
                                MUX_CLIPBOARD_ERROR_LIMIT,
                                "WebKit clipboard format exceeds 16 MiB");
            return NULL;
        }
        return g_bytes_ref(bytes);
    }

    text = wpe_clipboard_content_get_text(content);
    if (text != NULL && g_str_has_prefix(format, "text/plain")) {
        length = strlen(text);
        if (length > MUX_CLIPBOARD_MAX_ITEM_BYTES) {
            g_set_error_literal(error,
                                MUX_CLIPBOARD_ERROR,
                                MUX_CLIPBOARD_ERROR_LIMIT,
                                "WebKit clipboard text exceeds 16 MiB");
            return NULL;
        }
        return g_bytes_new(text, length);
    }

    stream = g_memory_output_stream_new(NULL, 0, bounded_realloc, g_free);
    if (!wpe_clipboard_content_serialize(content, format, stream) ||
        !g_output_stream_close(stream, NULL, error)) {
        if (error != NULL && *error == NULL)
            g_set_error_literal(
                error,
                MUX_CLIPBOARD_ERROR,
                MUX_CLIPBOARD_ERROR_LIMIT,
                "WebKit clipboard format could not be serialized within 16 MiB");
        g_object_unref(stream);
        return NULL;
    }

    bytes = g_memory_output_stream_steal_as_bytes(
        G_MEMORY_OUTPUT_STREAM(stream));
    g_object_unref(stream);
    return bytes;
}

static MuxClipboardSnapshot *
snapshot_from_content(MuxWpeClipboard *clipboard,
                      GPtrArray *formats,
                      WPEClipboardContent *content,
                      GError **error)
{
    MuxClipboardSnapshotItem items[MUX_CLIPBOARD_MAX_ITEMS] = { 0 };
    g_autoptr(GHashTable) seen = NULL;
    g_autoptr(GPtrArray) selected = NULL;
    MuxClipboardSnapshot *snapshot = NULL;
    gsize total = 0;
    guint item_count = 0;
    guint pass;
    guint i;

    if (formats == NULL || content == NULL) {
        snapshot = mux_clipboard_snapshot_new_sealed_from_items(
            clipboard->next_serial + 1,
            items,
            0,
            error);
        if (snapshot != NULL)
            clipboard->next_serial++;
        return snapshot;
    }

    seen = g_hash_table_new(g_str_hash, g_str_equal);
    selected = g_ptr_array_new();

    for (i = 0; i < formats->len; i++) {
        const gchar *format = g_ptr_array_index(formats, i);

        if (format == NULL)
            break;
        if (!mux_clipboard_mime_is_valid(format)) {
            g_warning("WebKit clipboard omitted an invalid MIME type");
            continue;
        }
        if (g_hash_table_contains(seen, format))
            continue;
        if (selected->len >= MUX_CLIPBOARD_MAX_ITEMS) {
            g_warning("WebKit clipboard omitted excess MIME variants");
            break;
        }
        g_hash_table_add(seen, (gpointer)format);
        g_ptr_array_add(selected, (gpointer)format);
    }

    /* Prefer text when bounded policy requires omitting MIME variants. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < selected->len; i++) {
            const gchar *format = g_ptr_array_index(selected, i);
            g_autoptr(GError) item_error = NULL;
            gboolean is_text =
                g_ascii_strncasecmp(format, "text/", 5) == 0;
            GBytes *bytes;
            gsize length;

            if (is_text != (pass == 0))
                continue;
            bytes = serialize_content(content, format, &item_error);
            if (bytes == NULL) {
                g_warning("WebKit clipboard MIME variant omitted: %s",
                          item_error != NULL ? item_error->message
                                             : "serialization failed");
                continue;
            }
            length = g_bytes_get_size(bytes);
            if (length > MUX_CLIPBOARD_MAX_TOTAL_BYTES - total) {
                g_warning("WebKit clipboard MIME variant omitted: total exceeds 32 MiB");
                g_bytes_unref(bytes);
                continue;
            }
            items[item_count].mime = format;
            items[item_count].bytes = bytes;
            item_count++;
            total += length;
        }
    }

    snapshot = mux_clipboard_snapshot_new_sealed_from_items(
        clipboard->next_serial + 1,
        items,
        item_count,
        error);
    if (snapshot != NULL)
        clipboard->next_serial++;

    for (i = 0; i < item_count; i++)
        g_bytes_unref(items[i].bytes);
    return snapshot;
}

static GBytes *
mux_wpe_clipboard_read(WPEClipboard *base, const gchar *format)
{
    MuxWpeClipboard *clipboard = MUX_WPE_CLIPBOARD(base);
    GBytes *bytes;

    if (clipboard->external == NULL)
        return NULL;

    bytes = mux_clipboard_snapshot_find(clipboard->external, format);
    mux_clipboard_smoke_trace(
        bytes != NULL ? MUX_CLIPBOARD_TRACE_WPE_READ_HIT
                      : MUX_CLIPBOARD_TRACE_WPE_READ_MISS,
        &(MuxClipboardTraceFields) {
            .snapshot = clipboard->external,
            .has_text_plain = g_str_has_prefix(format, "text/plain")
        });
    return bytes != NULL ? g_bytes_ref(bytes) : NULL;
}

static void
mux_wpe_clipboard_changed(WPEClipboard *base,
                          GPtrArray *formats,
                          gboolean is_local,
                          WPEClipboardContent *content)
{
    MuxWpeClipboard *clipboard = MUX_WPE_CLIPBOARD(base);
    WPEClipboardClass *parent_class =
        WPE_CLIPBOARD_CLASS(mux_wpe_clipboard_parent_class);
    g_autoptr(MuxClipboardSnapshot) snapshot = NULL;
    g_autoptr(GError) error = NULL;
    MuxWpeClipboardPublishFunc publish_func = clipboard->publish_func;
    GDestroyNotify publication_data_destroy =
        clipboard->publication_data_destroy;
    gpointer user_data = clipboard->user_data;
    gpointer publication_data = NULL;
    gboolean should_publish = is_local && publish_func != NULL;

    if (should_publish && clipboard->publish_begin_func != NULL)
        publication_data = clipboard->publish_begin_func(clipboard,
                                                         user_data);

    parent_class->changed(base, formats, is_local, content);
    if (!should_publish)
        return;

    snapshot = snapshot_from_content(clipboard, formats, content, &error);
    if (snapshot == NULL) {
        if (error != NULL)
            g_warning("WebKit clipboard publication rejected: %s",
                      error->message);
        goto out;
    }
    publish_func(clipboard, snapshot, publication_data, user_data);

out:
    if (publication_data_destroy != NULL)
        publication_data_destroy(publication_data);
}

static void
mux_wpe_clipboard_finalize(GObject *object)
{
    MuxWpeClipboard *clipboard = MUX_WPE_CLIPBOARD(object);

    g_clear_pointer(&clipboard->external, mux_clipboard_snapshot_unref);
    if (clipboard->user_data_destroy != NULL)
        clipboard->user_data_destroy(clipboard->user_data);

    G_OBJECT_CLASS(mux_wpe_clipboard_parent_class)->finalize(object);
}

static void
mux_wpe_clipboard_class_init(MuxWpeClipboardClass *clipboard_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(clipboard_class);
    WPEClipboardClass *wpe_class = WPE_CLIPBOARD_CLASS(clipboard_class);

    object_class->finalize = mux_wpe_clipboard_finalize;
    wpe_class->read = mux_wpe_clipboard_read;
    wpe_class->changed = mux_wpe_clipboard_changed;
}

static void
mux_wpe_clipboard_init(MuxWpeClipboard *clipboard)
{
    (void)clipboard;
}

MuxWpeClipboard *
mux_wpe_clipboard_new(WPEDisplay *display,
                      MuxWpeClipboardPublishBeginFunc publish_begin_func,
                      MuxWpeClipboardPublishFunc publish_func,
                      GDestroyNotify publication_data_destroy,
                      gpointer user_data,
                      GDestroyNotify user_data_destroy)
{
    MuxWpeClipboard *clipboard;

    g_return_val_if_fail(WPE_IS_DISPLAY(display), NULL);

    clipboard = g_object_new(MUX_TYPE_WPE_CLIPBOARD,
                             "display",
                             display,
                             NULL);
    clipboard->publish_begin_func = publish_begin_func;
    clipboard->publish_func = publish_func;
    clipboard->publication_data_destroy = publication_data_destroy;
    clipboard->user_data = user_data;
    clipboard->user_data_destroy = user_data_destroy;
    return clipboard;
}

void
mux_wpe_clipboard_set_external(MuxWpeClipboard *clipboard,
                               const MuxClipboardSnapshot *snapshot)
{
    WPEClipboardClass *parent_class;
    MuxClipboardSnapshot *copy;
    GPtrArray *formats;
    guint i;

    g_return_if_fail(MUX_IS_WPE_CLIPBOARD(clipboard));
    g_return_if_fail(snapshot != NULL);

    copy = mux_clipboard_snapshot_dup_sealed(snapshot);
    g_return_if_fail(copy != NULL);

    g_clear_pointer(&clipboard->external, mux_clipboard_snapshot_unref);
    clipboard->external = copy;

    formats = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < mux_clipboard_snapshot_get_count(copy); i++) {
        const gchar *mime = NULL;

        mux_clipboard_snapshot_get_item(copy, i, &mime, NULL);
        g_ptr_array_add(formats, g_strdup(mime));
    }
    g_ptr_array_add(formats, NULL);

    parent_class = WPE_CLIPBOARD_CLASS(mux_wpe_clipboard_parent_class);
    parent_class->changed(WPE_CLIPBOARD(clipboard),
                          formats,
                          FALSE,
                          NULL);
    g_ptr_array_unref(formats);
}

void
mux_wpe_clipboard_clear_external(MuxWpeClipboard *clipboard, guint64 serial)
{
    g_autoptr(MuxClipboardSnapshot) snapshot = NULL;

    g_return_if_fail(MUX_IS_WPE_CLIPBOARD(clipboard));

    snapshot = mux_clipboard_snapshot_new(serial);
    mux_clipboard_snapshot_seal(snapshot);
    mux_wpe_clipboard_set_external(clipboard, snapshot);
}

const MuxClipboardSnapshot *
mux_wpe_clipboard_get_external(MuxWpeClipboard *clipboard)
{
    g_return_val_if_fail(MUX_IS_WPE_CLIPBOARD(clipboard), NULL);
    return clipboard->external;
}
