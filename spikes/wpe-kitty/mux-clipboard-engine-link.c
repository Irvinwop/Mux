#include "mux-clipboard-engine-link.h"

#include <string.h>

struct _MuxClipboardEngineLink {
    gchar *profile;
    gboolean ephemeral;
    guint64 active_view_id;
    gchar *active_origin;
    guint64 next_transaction_id;
    MuxWpeClipboard *clipboard;
    MuxClipboardWireAssembler *assembler;
    MuxClipboardEngineOutputFunc output_func;
    MuxClipboardEnginePasteFunc paste_func;
    MuxClipboardEngineFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

struct _MuxClipboardEngineWrite {
    MuxClipboardEngineLink *owner;
    gchar *profile;
    gchar *source_origin;
    guint64 source_view_id;
    guint64 transaction_id;
    gint64 created_us;
    guint32 flags;
    gboolean completed;
};

static gboolean
valid_text(const gchar *text, gsize limit, gboolean required)
{
    gsize length;

    if (text == NULL)
        return !required;
    length = strlen(text);
    return (!required || length > 0) && length <= limit &&
           g_utf8_validate(text, length, NULL);
}

static guint64
next_transaction(MuxClipboardEngineLink *link)
{
    link->next_transaction_id++;
    if (link->next_transaction_id == 0)
        link->next_transaction_id++;
    return link->next_transaction_id;
}

static void
report_failure(MuxClipboardEngineLink *link,
               const gchar *operation,
               const GError *error)
{
    if (link->failure_func != NULL)
        link->failure_func(link, operation, error, link->user_data);
}

static gboolean
wire_output(GBytes *packet, gpointer user_data, GError **error)
{
    MuxClipboardEngineLink *link = user_data;

    return link->output_func(link, packet, link->user_data, error);
}

static gboolean
send_ack(MuxClipboardEngineLink *link,
         guint64 transaction_id,
         GError **error)
{
    MuxClipboardWireRecord record = {
        .type = MUX_CLIPBOARD_WIRE_ACK,
        .transaction_id = transaction_id
    };
    GBytes *packet = mux_clipboard_wire_record_encode(&record, error);
    gboolean result;

    if (packet == NULL)
        return FALSE;
    result = link->output_func(link, packet, link->user_data, error);
    g_bytes_unref(packet);
    return result;
}

MuxClipboardEngineWrite *
mux_clipboard_engine_link_begin_write(MuxClipboardEngineLink *link)
{
    MuxClipboardEngineWrite *write;

    g_return_val_if_fail(link != NULL, NULL);
    write = g_new0(MuxClipboardEngineWrite, 1);
    write->owner = link;
    write->profile = g_strdup(link->profile);
    write->source_origin = g_strdup(link->active_origin);
    write->source_view_id = link->active_view_id;
    write->transaction_id = next_transaction(link);
    write->created_us = g_get_monotonic_time();
    write->flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT |
                   MUX_CLIPBOARD_WIRE_FLAG_HISTORY;
    if (link->ephemeral)
        write->flags |= MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL;
    return write;
}

gboolean
mux_clipboard_engine_link_complete_write(
    MuxClipboardEngineLink *link,
    MuxClipboardEngineWrite *write,
    const MuxClipboardSnapshot *snapshot,
    GError **error)
{
    g_return_val_if_fail(link != NULL, FALSE);
    if (!write || write->owner != link || write->completed) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard write transaction is invalid or complete");
        return FALSE;
    }
    if (!snapshot || !mux_clipboard_snapshot_is_sealed(snapshot)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard write snapshot must be sealed");
        return FALSE;
    }

    write->completed = TRUE;
    return mux_clipboard_wire_send_snapshot(write->transaction_id,
                                            write->flags,
                                            write->profile,
                                            write->source_origin,
                                            write->source_view_id,
                                            write->created_us,
                                            snapshot,
                                            wire_output,
                                            link,
                                            error);
}

void
mux_clipboard_engine_write_free(MuxClipboardEngineWrite *write)
{
    if (!write)
        return;
    g_free(write->profile);
    g_free(write->source_origin);
    g_free(write);
}

static gpointer
on_webkit_publish_begin(MuxWpeClipboard *clipboard, gpointer user_data)
{
    (void)clipboard;
    return mux_clipboard_engine_link_begin_write(user_data);
}

static void
on_webkit_publish(MuxWpeClipboard *clipboard,
                  MuxClipboardSnapshot *snapshot,
                  gpointer publication_data,
                  gpointer user_data)
{
    MuxClipboardEngineLink *link = user_data;
    MuxClipboardEngineWrite *write = publication_data;
    g_autoptr(GError) error = NULL;

    (void)clipboard;
    mux_clipboard_smoke_trace(
        MUX_CLIPBOARD_TRACE_WPE_LOCAL,
        &(MuxClipboardTraceFields) {
            .transaction_id = write->transaction_id,
            .view_id = write->source_view_id,
            .snapshot = snapshot
        });
    if (!mux_clipboard_engine_link_complete_write(link,
                                                  write,
                                                  snapshot,
                                                  &error))
        report_failure(link, "webkit-copy", error);
}

MuxClipboardEngineLink *
mux_clipboard_engine_link_new(WPEDisplay *display,
                              const gchar *profile,
                              gboolean ephemeral,
                              MuxClipboardEngineOutputFunc output_func,
                              MuxClipboardEnginePasteFunc paste_func,
                              MuxClipboardEngineFailureFunc failure_func,
                              gpointer user_data,
                              GDestroyNotify user_data_destroy)
{
    MuxClipboardEngineLink *link;

    g_return_val_if_fail(WPE_IS_DISPLAY(display), NULL);
    g_return_val_if_fail(valid_text(profile,
                                    MUX_CLIPBOARD_WIRE_MAX_PROFILE,
                                    TRUE),
                         NULL);
    g_return_val_if_fail(output_func != NULL, NULL);

    link = g_new0(MuxClipboardEngineLink, 1);
    link->profile = g_strdup(profile);
    link->ephemeral = ephemeral;
    link->next_transaction_id =
        ((guint64)g_random_int() << 32) | g_random_int();
    link->output_func = output_func;
    link->paste_func = paste_func;
    link->failure_func = failure_func;
    link->user_data = user_data;
    link->user_data_destroy = user_data_destroy;
    link->assembler = mux_clipboard_wire_assembler_new(0);
    link->clipboard = mux_wpe_clipboard_new(display,
                                            on_webkit_publish_begin,
                                            on_webkit_publish,
                                            (GDestroyNotify)
                                                mux_clipboard_engine_write_free,
                                            link,
                                            NULL);
    if (link->assembler == NULL || link->clipboard == NULL) {
        mux_clipboard_engine_link_free(link);
        return NULL;
    }
    return link;
}

void
mux_clipboard_engine_link_free(MuxClipboardEngineLink *link)
{
    if (link == NULL)
        return;

    g_clear_object(&link->clipboard);
    mux_clipboard_wire_assembler_free(link->assembler);
    if (link->user_data_destroy != NULL)
        link->user_data_destroy(link->user_data);
    g_free(link->active_origin);
    g_free(link->profile);
    g_free(link);
}

WPEClipboard *
mux_clipboard_engine_link_get_clipboard(MuxClipboardEngineLink *link)
{
    g_return_val_if_fail(link != NULL, NULL);
    return WPE_CLIPBOARD(link->clipboard);
}

gboolean
mux_clipboard_engine_link_set_active_source(MuxClipboardEngineLink *link,
                                            guint64 view_id,
                                            const gchar *origin,
                                            gboolean ephemeral,
                                            GError **error)
{
    g_return_val_if_fail(link != NULL, FALSE);
    if (!valid_text(origin, MUX_CLIPBOARD_WIRE_MAX_ORIGIN, FALSE)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard source origin is invalid");
        return FALSE;
    }

    link->active_view_id = view_id;
    link->ephemeral = ephemeral;
    g_free(link->active_origin);
    link->active_origin = g_strdup(origin);
    return TRUE;
}

static gboolean
handle_remote_error(MuxClipboardEngineLink *link,
                    const MuxClipboardWireRecord *record,
                    GError **error)
{
    g_autoptr(GError) remote_error = NULL;
    const gchar *data;
    gsize length;

    data = g_bytes_get_data(record->payload, &length);
    if (length > 4096 ||
        (length > 0 &&
         (memchr(data, '\0', length) != NULL ||
          !g_utf8_validate(data, length, NULL)))) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "invalid clipboard remote error");
        return FALSE;
    }

    if (length > 0)
        remote_error = g_error_new(G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "pane rejected clipboard transaction: %.*s",
                                   (gint)length,
                                   data);
    else
        remote_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "pane rejected clipboard transaction: unspecified error");
    report_failure(link, "clipboard-wire", remote_error);
    g_propagate_error(error, g_steal_pointer(&remote_error));
    return FALSE;
}

gboolean
mux_clipboard_engine_link_handle_packet(MuxClipboardEngineLink *link,
                                        const guint8 *packet,
                                        gsize packet_length,
                                        GError **error)
{
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    const gchar *profile;
    const MuxClipboardSnapshot *snapshot;
    guint64 transaction_id;
    g_autoptr(GError) feed_error = NULL;
    gboolean result = FALSE;

    g_return_val_if_fail(link != NULL, FALSE);
    if (!mux_clipboard_wire_record_decode(packet,
                                          packet_length,
                                          &record,
                                          error))
        return FALSE;
    if (record.type == MUX_CLIPBOARD_WIRE_ACK) {
        mux_clipboard_wire_record_clear(&record);
        return TRUE;
    }
    if (record.type == MUX_CLIPBOARD_WIRE_REMOTE_ERROR) {
        result = handle_remote_error(link, &record, error);
        mux_clipboard_wire_record_clear(&record);
        return result;
    }
    mux_clipboard_wire_record_clear(&record);

    feed_result = mux_clipboard_wire_assembler_feed(link->assembler,
                                                    packet,
                                                    packet_length,
                                                    g_get_monotonic_time(),
                                                    &transfer,
                                                    &feed_error);
    if (feed_error != NULL) {
        report_failure(link, "clipboard-wire", feed_error);
        g_propagate_error(error, g_steal_pointer(&feed_error));
        return FALSE;
    }
    if (feed_result == MUX_CLIPBOARD_WIRE_FEED_ACCEPTED ||
        feed_result == MUX_CLIPBOARD_WIRE_FEED_CANCELLED)
        return TRUE;
    if (feed_result == MUX_CLIPBOARD_WIRE_FEED_REJECTED)
        return FALSE;

    profile = mux_clipboard_wire_transfer_get_profile(transfer);
    if (profile == NULL || !g_str_equal(profile, link->profile)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "clipboard transfer crossed profile boundary");
        goto out;
    }
    if (!(mux_clipboard_wire_transfer_get_flags(transfer) &
          MUX_CLIPBOARD_WIRE_FLAG_CURRENT)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard transfer does not update current state");
        goto out;
    }

    transaction_id =
        mux_clipboard_wire_transfer_get_transaction_id(transfer);
    snapshot = mux_clipboard_wire_transfer_get_snapshot(transfer);
    mux_wpe_clipboard_set_external(link->clipboard, snapshot);
    mux_clipboard_smoke_trace(
        MUX_CLIPBOARD_TRACE_ENGINE_EXTERNAL,
        &(MuxClipboardTraceFields) {
            .transaction_id = transaction_id,
            .view_id =
                mux_clipboard_wire_transfer_get_source_view_id(transfer),
            .snapshot = snapshot
        });
    if ((mux_clipboard_wire_transfer_get_flags(transfer) &
         MUX_CLIPBOARD_WIRE_FLAG_PASTE) &&
        link->paste_func != NULL) {
        guint64 view_id =
            mux_clipboard_wire_transfer_get_source_view_id(transfer);

        link->paste_func(link,
                         view_id != 0 ? view_id : link->active_view_id,
                         snapshot,
                         link->user_data);
    }
    result = send_ack(link, transaction_id, error);

out:
    mux_clipboard_wire_transfer_free(transfer);
    return result;
}

gboolean
mux_clipboard_engine_link_tick(MuxClipboardEngineLink *link,
                               gint64 monotonic_us)
{
    g_autoptr(GError) error = NULL;

    g_return_val_if_fail(link != NULL, FALSE);
    if (!mux_clipboard_wire_assembler_tick(link->assembler,
                                           monotonic_us))
        return FALSE;

    error = g_error_new_literal(G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "pane clipboard transfer timed out");
    report_failure(link, "clipboard-wire", error);
    return TRUE;
}
