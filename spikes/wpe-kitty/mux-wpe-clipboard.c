#include "mux-wpe-clipboard.h"

#include <string.h>

struct _MuxWpeClipboard {
    WPEClipboard parent_instance;
    MuxClipboardSnapshot *external;
    MuxWpeClipboardPublishFunc publish_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    guint64 next_serial;
};

G_DEFINE_TYPE(MuxWpeClipboard, mux_wpe_clipboard, WPE_TYPE_CLIPBOARD)

static GBytes *
serialize_content(WPEClipboardContent *content, const gchar *format)
{
    GBytes *bytes;
    const gchar *text;
    GOutputStream *stream;

    bytes = wpe_clipboard_content_get_bytes(content, format);
    if (bytes != NULL)
        return g_bytes_ref(bytes);

    text = wpe_clipboard_content_get_text(content);
    if (text != NULL && g_str_has_prefix(format, "text/plain"))
        return g_bytes_new(text, strlen(text));

    stream = g_memory_output_stream_new_resizable();
    if (!wpe_clipboard_content_serialize(content, format, stream) ||
        !g_output_stream_close(stream, NULL, NULL)) {
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
                      WPEClipboardContent *content)
{
    MuxClipboardSnapshot *snapshot;
    g_autoptr(GError) error = NULL;
    guint i;

    snapshot = mux_clipboard_snapshot_new(++clipboard->next_serial);
    if (formats == NULL || content == NULL) {
        mux_clipboard_snapshot_seal(snapshot);
        return snapshot;
    }

    for (i = 0; i < formats->len; i++) {
        const gchar *format = g_ptr_array_index(formats, i);
        g_autoptr(GBytes) bytes = NULL;

        if (format == NULL)
            break;
        if (!mux_clipboard_mime_is_valid(format))
            continue;
        if (mux_clipboard_snapshot_find(snapshot, format) != NULL)
            continue;

        bytes = serialize_content(content, format);
        if (bytes == NULL)
            continue;
        if (!mux_clipboard_snapshot_add(snapshot, format, bytes, &error)) {
            g_warning("could not export WebKit clipboard: %s", error->message);
            mux_clipboard_snapshot_unref(snapshot);
            snapshot = mux_clipboard_snapshot_new(++clipboard->next_serial);
            break;
        }
    }

    mux_clipboard_snapshot_seal(snapshot);
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

    parent_class->changed(base, formats, is_local, content);
    if (!is_local || clipboard->publish_func == NULL)
        return;

    snapshot = snapshot_from_content(clipboard, formats, content);
    clipboard->publish_func(clipboard, snapshot, clipboard->user_data);
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
                      MuxWpeClipboardPublishFunc publish_func,
                      gpointer user_data,
                      GDestroyNotify user_data_destroy)
{
    MuxWpeClipboard *clipboard;

    g_return_val_if_fail(WPE_IS_DISPLAY(display), NULL);

    clipboard = g_object_new(MUX_TYPE_WPE_CLIPBOARD,
                             "display",
                             display,
                             NULL);
    clipboard->publish_func = publish_func;
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
