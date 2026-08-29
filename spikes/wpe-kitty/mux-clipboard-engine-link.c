#include "mux-clipboard-engine-link.h"
#include "mux-clipboard-lifetime.h"

#include <string.h>

struct _MuxClipboardEngineLink {
    MuxClipboardLifetime lifetime;
    gboolean disposing;
    gchar *profile;
    gboolean ephemeral;
    guint64 active_view_id;
    gchar *active_origin;
    guint64 next_transaction_id;
    MuxWpeClipboard *clipboard;
    MuxClipboardWireAssembler *assembler;
    GHashTable *pending_writes;
    MuxClipboardEngineOutputFunc output_func;
    MuxClipboardEnginePasteFunc paste_func;
    MuxClipboardEngineFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

struct _MuxClipboardEngineWrite {
    gint reference_count;
    MuxClipboardEngineLink *owner;
    gchar *profile;
    gchar *source_origin;
    guint64 source_view_id;
    guint64 transaction_id;
    gint64 created_us;
    guint32 flags;
    gint64 deadline_us;
    gboolean completed;
};

static void engine_link_destroy(MuxClipboardEngineLink *link);

static MuxClipboardEngineLink *
engine_link_acquire(MuxClipboardEngineLink *link)
{
    mux_clipboard_lifetime_acquire(&link->lifetime);
    return link;
}

static void
engine_link_release(MuxClipboardEngineLink *link)
{
    if (mux_clipboard_lifetime_release(&link->lifetime))
        engine_link_destroy(link);
}

typedef MuxClipboardEngineLink MuxClipboardEngineLinkOperation;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardEngineLinkOperation,
                              engine_link_release)

static MuxClipboardEngineWrite *
engine_write_ref(MuxClipboardEngineWrite *write)
{
    g_atomic_int_inc(&write->reference_count);
    return write;
}

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

    if (link->disposing) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "clipboard engine link is closing");
        return FALSE;
    }
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
    if (link->disposing)
        return NULL;
    write = g_new0(MuxClipboardEngineWrite, 1);
    write->reference_count = 1;
    write->owner = engine_link_acquire(link);
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

    if (link->disposing) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "clipboard engine link is closing");
        return FALSE;
    }
    if (!mux_clipboard_wire_send_snapshot(write->transaction_id,
                                          write->flags,
                                          write->profile,
                                          write->source_origin,
                                          write->source_view_id,
                                          write->created_us,
                                          snapshot,
                                          wire_output,
                                          link,
                                          error))
        return FALSE;
    write->completed = TRUE;
    write->deadline_us = g_get_monotonic_time() +
        ((gint64)MUX_CLIPBOARD_WIRE_TIMEOUT_MS * 1000);
    {
        guint64 *key = g_new(guint64, 1);

        *key = write->transaction_id;
        g_hash_table_insert(link->pending_writes,
                            key,
                            engine_write_ref(write));
    }
    return TRUE;
}

void
mux_clipboard_engine_write_free(MuxClipboardEngineWrite *write)
{
    if (!write)
        return;
    if (!g_atomic_int_dec_and_test(&write->reference_count))
        return;
    g_free(write->profile);
    g_free(write->source_origin);
    engine_link_release(write->owner);
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
    if (write == NULL)
        return;
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
    mux_clipboard_lifetime_init(&link->lifetime);
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
    link->pending_writes = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)mux_clipboard_engine_write_free);
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
    if (link->disposing)
        return;
    link->disposing = TRUE;
    g_hash_table_remove_all(link->pending_writes);
    if (mux_clipboard_lifetime_release_owner(&link->lifetime))
        engine_link_destroy(link);
}

static void
engine_link_destroy(MuxClipboardEngineLink *link)
{
    g_clear_object(&link->clipboard);
    mux_clipboard_wire_assembler_free(link->assembler);
    g_hash_table_unref(link->pending_writes);
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
    g_hash_table_remove(link->pending_writes,
                        &record->transaction_id);
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
    g_autoptr(MuxClipboardEngineLinkOperation) operation = NULL;
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    const gchar *profile;
    const MuxClipboardSnapshot *snapshot;
    guint64 transaction_id;
    g_autoptr(GError) feed_error = NULL;
    gboolean result = FALSE;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = engine_link_acquire(link);
    (void)operation;
    if (!mux_clipboard_wire_record_decode(packet,
                                          packet_length,
                                          &record,
                                          error))
        return FALSE;
    if (record.type == MUX_CLIPBOARD_WIRE_ACK) {
        g_hash_table_remove(link->pending_writes,
                            &record.transaction_id);
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
        link->paste_func != NULL && link->active_view_id != 0) {
        link->paste_func(link,
                         link->active_view_id,
                         snapshot,
                         link->user_data);
    } else if (mux_clipboard_wire_transfer_get_flags(transfer) &
               MUX_CLIPBOARD_WIRE_FLAG_PASTE) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "clipboard paste has no active target view");
        goto out;
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
    g_autoptr(MuxClipboardEngineLinkOperation) operation = NULL;
    g_autoptr(GArray) expired_ids = NULL;
    g_autoptr(GError) error = NULL;
    GHashTableIter iterator;
    gpointer value;
    gboolean expired = FALSE;
    guint i;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = engine_link_acquire(link);
    (void)operation;
    expired_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    g_hash_table_iter_init(&iterator, link->pending_writes);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        MuxClipboardEngineWrite *write = value;

        if (write->deadline_us <= monotonic_us)
            g_array_append_val(expired_ids, write->transaction_id);
    }
    for (i = 0; i < expired_ids->len; i++) {
        guint64 id = g_array_index(expired_ids, guint64, i);

        g_hash_table_remove(link->pending_writes, &id);
    }
    if (expired_ids->len > 0) {
        error = g_error_new_literal(G_IO_ERROR,
                                    G_IO_ERROR_TIMED_OUT,
                                    "pane clipboard acknowledgement timed out");
        report_failure(link, "clipboard-write", error);
        g_clear_error(&error);
        expired = TRUE;
    }
    if (!mux_clipboard_wire_assembler_tick(link->assembler,
                                           monotonic_us))
        return expired;

    error = g_error_new_literal(G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "pane clipboard transfer timed out");
    report_failure(link, "clipboard-wire", error);
    return TRUE;
}
