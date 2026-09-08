#include "mux-clipboard-broker-peer.h"

#include <string.h>

struct _MuxClipboardBrokerPeer {
    gint reference_count;
    MuxClipboardBroker *broker;
    MuxClipboardBrokerPeerOutputFunc output_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gchar *profile;
    MuxClipboardHistoryMode mode;
    MuxClipboardWireAssembler *assembler;
    guint64 pending_select_request_id;
    guint64 pending_select_entry_id;
    guint64 pending_select_transaction_id;
    gint64 pending_select_deadline_us;
    gboolean closed;
};

static gboolean
output_extension(MuxClipboardBrokerPeer *peer,
                 guint16 channel,
                 GBytes *payload,
                 GError **error)
{
    MuxExtensionRecord record = {
        .channel = channel,
        .payload = payload
    };
    GBytes *packet;
    gboolean result;

    packet = mux_extension_record_encode(&record, error);
    if (packet == NULL)
        return FALSE;
    result = peer->output_func(peer, packet, peer->user_data, error);
    g_bytes_unref(packet);
    return result;
}

static gboolean
output_wire(GBytes *packet, gpointer user_data, GError **error)
{
    return output_extension(user_data,
                            MUX_EXTENSION_CHANNEL_CLIPBOARD,
                            packet,
                            error);
}

static gboolean
output_control_packet(MuxClipboardBrokerPeer *peer,
                      GBytes *packet,
                      GError **error)
{
    gboolean result = output_extension(
        peer,
        MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER,
        packet,
        error);

    g_bytes_unref(packet);
    return result;
}

static gboolean
send_control(MuxClipboardBrokerPeer *peer,
             const MuxClipboardControlRecord *record,
             GError **error)
{
    GBytes *packet = mux_clipboard_control_record_encode(record, error);

    return packet != NULL
               ? output_control_packet(peer, packet, error)
               : FALSE;
}

static gboolean
send_ok(MuxClipboardBrokerPeer *peer,
        guint64 request_id,
        guint64 entry_id,
        GError **error)
{
    MuxClipboardControlRecord record = {
        .type = MUX_CLIPBOARD_CONTROL_OK,
        .request_id = request_id,
        .entry_id = entry_id
    };

    return send_control(peer, &record, error);
}

static gboolean
send_error(MuxClipboardBrokerPeer *peer,
           guint64 request_id,
           const GError *cause,
           GError **error)
{
    MuxClipboardControlRecord record = {
        .type = MUX_CLIPBOARD_CONTROL_REMOTE_ERROR,
        .request_id = request_id
    };
    gsize length = cause != NULL && cause->message != NULL
                       ? strlen(cause->message)
                       : 0;
    GBytes *payload;

    length = MIN(length, (gsize)MUX_CLIPBOARD_CONTROL_MAX_TEXT);
    payload = length > 0
                  ? g_bytes_new(cause->message, length)
                  : g_bytes_new_static("unspecified broker error", 24);
    record.payload = payload;
    if (!send_control(peer, &record, error)) {
        g_bytes_unref(payload);
        return FALSE;
    }
    g_bytes_unref(payload);
    return TRUE;
}

static gboolean
send_wire_ack(MuxClipboardBrokerPeer *peer,
              guint64 transaction_id,
              guint32 flags,
              GError **error)
{
    MuxClipboardWireRecord record = {
        .type = MUX_CLIPBOARD_WIRE_ACK,
        .flags = flags,
        .transaction_id = transaction_id
    };
    GBytes *packet = mux_clipboard_wire_record_encode(&record, error);
    gboolean result;

    if (packet == NULL)
        return FALSE;
    result = output_extension(peer,
                              MUX_EXTENSION_CHANNEL_CLIPBOARD,
                              packet,
                              error);
    g_bytes_unref(packet);
    return result;
}

static gboolean
send_wire_error(MuxClipboardBrokerPeer *peer,
                guint64 transaction_id,
                GError **error)
{
    MuxClipboardWireRecord record = {
        .type = MUX_CLIPBOARD_WIRE_REMOTE_ERROR,
        .transaction_id = transaction_id
    };
    GBytes *packet = mux_clipboard_wire_record_encode(&record, error);
    gboolean result;

    if (packet == NULL)
        return FALSE;
    result = output_wire(packet, peer, error);
    g_bytes_unref(packet);
    return result;
}

MuxClipboardBrokerPeer *
mux_clipboard_broker_peer_new(
    MuxClipboardBroker *broker,
    MuxClipboardBrokerPeerOutputFunc output_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy)
{
    MuxClipboardBrokerPeer *peer;

    g_return_val_if_fail(broker != NULL, NULL);
    g_return_val_if_fail(output_func != NULL, NULL);

    peer = g_new0(MuxClipboardBrokerPeer, 1);
    peer->reference_count = 1;
    peer->broker = broker;
    peer->output_func = output_func;
    peer->user_data = user_data;
    peer->user_data_destroy = user_data_destroy;
    peer->mode = MUX_CLIPBOARD_HISTORY_DISABLED;
    peer->assembler = mux_clipboard_wire_assembler_new(0);
    if (peer->assembler == NULL) {
        mux_clipboard_broker_peer_unref(peer);
        return NULL;
    }
    return peer;
}

MuxClipboardBrokerPeer *
mux_clipboard_broker_peer_ref(MuxClipboardBrokerPeer *peer)
{
    g_return_val_if_fail(peer != NULL, NULL);
    g_atomic_int_inc(&peer->reference_count);
    return peer;
}

void
mux_clipboard_broker_peer_unref(MuxClipboardBrokerPeer *peer)
{
    if (peer == NULL ||
        !g_atomic_int_dec_and_test(&peer->reference_count))
        return;

    mux_clipboard_wire_assembler_free(peer->assembler);
    if (peer->user_data_destroy != NULL)
        peer->user_data_destroy(peer->user_data);
    g_free(peer->profile);
    g_free(peer);
}

static gboolean
handle_hello(MuxClipboardBrokerPeer *peer,
             const MuxClipboardControlRecord *record,
             GError **error)
{
    g_autofree gchar *profile = NULL;
    g_autoptr(GError) operation_error = NULL;
    guint32 mode_flags = record->flags &
                         (MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED |
                          MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL);
    MuxClipboardHistoryMode mode;

    if (peer->profile != NULL || record->entry_id != 0 ||
        record->flags != mode_flags ||
        mode_flags == (MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED |
                       MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL) ||
        !mux_clipboard_control_payload_text(
            record,
            MUX_CLIPBOARD_HISTORY_MAX_PROFILE,
            FALSE,
            &profile,
            error))
        return FALSE;

    if (mode_flags & MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED)
        mode = MUX_CLIPBOARD_HISTORY_DISABLED;
    else if (mode_flags & MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL)
        mode = MUX_CLIPBOARD_HISTORY_EPHEMERAL;
    else
        mode = MUX_CLIPBOARD_HISTORY_MEMORY;

    if (!mux_clipboard_broker_set_profile_mode(peer->broker,
                                               profile,
                                               mode,
                                               &operation_error))
        return send_error(peer,
                          record->request_id,
                          operation_error,
                          error);

    peer->profile = g_steal_pointer(&profile);
    peer->mode = mode;
    return send_ok(peer, record->request_id, 0, error);
}

static gboolean
send_summaries(MuxClipboardBrokerPeer *peer,
               const MuxClipboardControlRecord *record,
               GError **error)
{
    g_autoptr(GError) operation_error = NULL;
    g_autoptr(GPtrArray) summaries = NULL;
    guint i;

    summaries = mux_clipboard_broker_list(peer->broker,
                                          peer->profile,
                                          &operation_error);
    if (summaries == NULL)
        return send_error(peer,
                          record->request_id,
                          operation_error,
                          error);

    for (i = 0; i < summaries->len; i++) {
        MuxClipboardBrokerSummary *source =
            g_ptr_array_index(summaries, i);
        MuxClipboardControlSummary summary = {
            .entry_id = source->id,
            .created_us = source->created_us,
            .pinned = source->pinned,
            .source_origin = source->source_origin != NULL
                                 ? source->source_origin
                                 : "external",
            .preview = source->preview,
            .source_view_id = source->source_view_id,
            .format_count = source->format_count,
            .mime_types = source->mime_types,
            .mime_type_count = source->mime_type_count,
            .total_bytes = source->total_bytes
        };
        GBytes *packet = mux_clipboard_control_summary_encode(
            record->request_id,
            &summary,
            error);

        if (packet == NULL ||
            !output_control_packet(peer, packet, error))
            return FALSE;
    }

    {
        MuxClipboardControlRecord done = {
            .type = MUX_CLIPBOARD_CONTROL_LIST_DONE,
            .request_id = record->request_id
        };

        return send_control(peer, &done, error);
    }
}

static gboolean
handle_select(MuxClipboardBrokerPeer *peer,
              const MuxClipboardControlRecord *record,
              GError **error)
{
    g_autoptr(GError) operation_error = NULL;
    g_autoptr(MuxClipboardSnapshot) snapshot = NULL;
    guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT;

    if (record->entry_id == 0 ||
        record->flags & ~MUX_CLIPBOARD_CONTROL_FLAG_PASTE ||
        g_bytes_get_size(record->payload) != 0)
        return FALSE;
    if (peer->pending_select_request_id != 0) {
        operation_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_BUSY,
            "another clipboard selection is awaiting acknowledgement");
        return send_error(peer,
                          record->request_id,
                          operation_error,
                          error);
    }

    snapshot = mux_clipboard_broker_select(peer->broker,
                                           peer->profile,
                                           record->entry_id,
                                           &operation_error);
    if (snapshot == NULL)
        return send_error(peer,
                          record->request_id,
                          operation_error,
                          error);

    if (record->flags & MUX_CLIPBOARD_CONTROL_FLAG_PASTE)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PASTE;
    if (peer->mode == MUX_CLIPBOARD_HISTORY_EPHEMERAL)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL;
    peer->pending_select_request_id = record->request_id;
    peer->pending_select_entry_id = record->entry_id;
    peer->pending_select_transaction_id = record->request_id;
    peer->pending_select_deadline_us =
        g_get_monotonic_time() +
        ((gint64)MUX_CLIPBOARD_WIRE_TIMEOUT_MS * 1000);
    if (!mux_clipboard_wire_send_snapshot(record->request_id,
                                          flags,
                                          peer->profile,
                                          "mux-history",
                                          0,
                                          g_get_monotonic_time(),
                                          snapshot,
                                          output_wire,
                                          peer,
                                          error)) {
        peer->pending_select_request_id = 0;
        peer->pending_select_entry_id = 0;
        peer->pending_select_transaction_id = 0;
        peer->pending_select_deadline_us = 0;
        return FALSE;
    }
    return TRUE;
}

static gboolean
handle_mutation(MuxClipboardBrokerPeer *peer,
                const MuxClipboardControlRecord *record,
                GError **error)
{
    g_autoptr(GError) operation_error = NULL;
    gboolean result = FALSE;
    guint removed = 0;

    if (g_bytes_get_size(record->payload) != 0)
        return FALSE;
    switch (record->type) {
    case MUX_CLIPBOARD_CONTROL_DELETE:
        if (record->entry_id != 0 && record->flags == 0)
            result = mux_clipboard_broker_delete(peer->broker,
                                                 peer->profile,
                                                 record->entry_id,
                                                 &operation_error);
        break;
    case MUX_CLIPBOARD_CONTROL_PIN:
        if (record->entry_id != 0 &&
            !(record->flags & ~MUX_CLIPBOARD_CONTROL_FLAG_PINNED))
            result = mux_clipboard_broker_set_pinned(
                peer->broker,
                peer->profile,
                record->entry_id,
                record->flags & MUX_CLIPBOARD_CONTROL_FLAG_PINNED,
                &operation_error);
        break;
    case MUX_CLIPBOARD_CONTROL_CLEAR:
        if (record->entry_id == 0 &&
            !(record->flags &
              ~MUX_CLIPBOARD_CONTROL_FLAG_INCLUDE_PINNED)) {
            removed = mux_clipboard_broker_clear(
                peer->broker,
                peer->profile,
                record->flags &
                    MUX_CLIPBOARD_CONTROL_FLAG_INCLUDE_PINNED,
                &operation_error);
            result = operation_error == NULL;
        }
        break;
    default:
        break;
    }

    if (!result) {
        if (operation_error == NULL)
            operation_error = g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "invalid clipboard history mutation");
        return send_error(peer,
                          record->request_id,
                          operation_error,
                          error);
    }
    return send_ok(peer,
                   record->request_id,
                   record->type == MUX_CLIPBOARD_CONTROL_CLEAR
                       ? removed
                       : record->entry_id,
                   error);
}

static gboolean
handle_control(MuxClipboardBrokerPeer *peer,
               GBytes *payload,
               GError **error)
{
    MuxClipboardControlRecord record = { 0 };
    const guint8 *data;
    gsize length;
    gboolean result = FALSE;

    data = g_bytes_get_data(payload, &length);
    if (!mux_clipboard_control_record_decode(data,
                                             length,
                                             &record,
                                             error))
        return FALSE;

    if (record.type == MUX_CLIPBOARD_CONTROL_HELLO) {
        result = handle_hello(peer, &record, error);
        goto out;
    }
    if (peer->profile == NULL) {
        g_autoptr(GError) cause = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "clipboard broker HELLO is required");

        result = send_error(peer, record.request_id, cause, error);
        goto out;
    }

    switch (record.type) {
    case MUX_CLIPBOARD_CONTROL_LIST:
        if (record.entry_id == 0 && record.flags == 0 &&
            g_bytes_get_size(record.payload) == 0)
            result = send_summaries(peer, &record, error);
        break;
    case MUX_CLIPBOARD_CONTROL_SELECT:
        result = handle_select(peer, &record, error);
        break;
    case MUX_CLIPBOARD_CONTROL_DELETE:
    case MUX_CLIPBOARD_CONTROL_PIN:
    case MUX_CLIPBOARD_CONTROL_CLEAR:
        result = handle_mutation(peer, &record, error);
        break;
    case MUX_CLIPBOARD_CONTROL_BYE:
        if (record.entry_id == 0 && record.flags == 0 &&
            g_bytes_get_size(record.payload) == 0) {
            result = send_ok(peer, record.request_id, 0, error);
            if (result)
                peer->closed = TRUE;
        }
        break;
    case MUX_CLIPBOARD_CONTROL_HELLO:
    case MUX_CLIPBOARD_CONTROL_SUMMARY:
    case MUX_CLIPBOARD_CONTROL_LIST_DONE:
    case MUX_CLIPBOARD_CONTROL_OK:
    case MUX_CLIPBOARD_CONTROL_REMOTE_ERROR:
    default:
        break;
    }

    if (!result && (error == NULL || *error == NULL)) {
        g_autoptr(GError) cause = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "invalid clipboard broker request");

        result = send_error(peer, record.request_id, cause, error);
    }

out:
    mux_clipboard_control_record_clear(&record);
    return result;
}

static gboolean
handle_snapshot(MuxClipboardBrokerPeer *peer,
                GBytes *payload,
                GError **error)
{
    const guint8 *data;
    gsize length;
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    g_autoptr(GError) operation_error = NULL;
    g_autoptr(GError) feed_error = NULL;
    guint64 transaction_id;
    MuxClipboardHistoryAddResult observe_result;

    if (peer->profile == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "clipboard broker HELLO is required");
        return FALSE;
    }

    data = g_bytes_get_data(payload, &length);
    if (!mux_clipboard_wire_record_decode(data, length, &record, error))
        return FALSE;
    if (record.type == MUX_CLIPBOARD_WIRE_ACK) {
        guint64 request_id = peer->pending_select_request_id;
        guint64 entry_id = peer->pending_select_entry_id;

        if (record.flags != 0 || request_id == 0 ||
            record.transaction_id !=
                peer->pending_select_transaction_id) {
            mux_clipboard_wire_record_clear(&record);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "clipboard selection ACK has no matching request");
            return FALSE;
        }
        peer->pending_select_request_id = 0;
        peer->pending_select_entry_id = 0;
        peer->pending_select_transaction_id = 0;
        peer->pending_select_deadline_us = 0;
        mux_clipboard_wire_record_clear(&record);
        return send_ok(peer, request_id, entry_id, error);
    }
    if (record.type == MUX_CLIPBOARD_WIRE_REMOTE_ERROR) {
        guint64 request_id = peer->pending_select_request_id;
        g_autoptr(GError) rejected = NULL;

        if (request_id == 0 ||
            record.transaction_id !=
                peer->pending_select_transaction_id) {
            mux_clipboard_wire_record_clear(&record);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "clipboard selection rejection has no matching request");
            return FALSE;
        }
        rejected = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "clipboard client rejected broker selection");
        peer->pending_select_request_id = 0;
        peer->pending_select_entry_id = 0;
        peer->pending_select_transaction_id = 0;
        peer->pending_select_deadline_us = 0;
        mux_clipboard_wire_record_clear(&record);
        return send_error(peer, request_id, rejected, error);
    }
    feed_result = mux_clipboard_wire_assembler_feed(peer->assembler,
                                                    data,
                                                    length,
                                                    g_get_monotonic_time(),
                                                    &transfer,
                                                    &feed_error);
    mux_clipboard_wire_record_clear(&record);
    if (feed_error != NULL) {
        g_propagate_error(error, g_steal_pointer(&feed_error));
        return FALSE;
    }
    if (feed_result == MUX_CLIPBOARD_WIRE_FEED_ACCEPTED ||
        feed_result == MUX_CLIPBOARD_WIRE_FEED_CANCELLED)
        return TRUE;
    if (feed_result == MUX_CLIPBOARD_WIRE_FEED_REJECTED)
        return FALSE;

    if (mux_clipboard_wire_transfer_get_profile(transfer) == NULL ||
        !g_str_equal(mux_clipboard_wire_transfer_get_profile(transfer),
                     peer->profile)) {
        mux_clipboard_wire_transfer_free(transfer);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "clipboard snapshot crossed profile boundary");
        return FALSE;
    }

    transaction_id =
        mux_clipboard_wire_transfer_get_transaction_id(transfer);
    observe_result = mux_clipboard_broker_observe(
        peer->broker,
        peer->profile,
        mux_clipboard_wire_transfer_get_snapshot(transfer),
        mux_clipboard_wire_transfer_get_created_us(transfer),
        mux_clipboard_wire_transfer_get_source_origin(transfer),
        mux_clipboard_wire_transfer_get_source_view_id(transfer),
        NULL,
        &operation_error);
    mux_clipboard_wire_transfer_free(transfer);

    if (operation_error != NULL)
        return send_wire_error(peer, transaction_id, error);
    return send_wire_ack(
        peer,
        transaction_id,
        observe_result == MUX_CLIPBOARD_HISTORY_DEGRADED
            ? MUX_CLIPBOARD_WIRE_FLAG_HISTORY_DEGRADED
            : 0,
        error);
}

gboolean
mux_clipboard_broker_peer_handle_packet(
    MuxClipboardBrokerPeer *peer,
    const guint8 *packet,
    gsize packet_length,
    GError **error)
{
    MuxClipboardBrokerPeer *guard;
    MuxExtensionRecord record = { 0 };
    gboolean result;

    g_return_val_if_fail(peer != NULL, FALSE);
    guard = mux_clipboard_broker_peer_ref(peer);
    if (peer->closed) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "clipboard broker peer is closed");
        mux_clipboard_broker_peer_unref(guard);
        return FALSE;
    }
    if (!mux_extension_record_decode(packet,
                                     packet_length,
                                     &record,
                                     error)) {
        mux_clipboard_broker_peer_unref(guard);
        return FALSE;
    }

    if (record.channel == MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER)
        result = handle_control(peer, record.payload, error);
    else if (record.channel == MUX_EXTENSION_CHANNEL_CLIPBOARD)
        result = handle_snapshot(peer, record.payload, error);
    else {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "clipboard broker channel is unsupported");
        result = FALSE;
    }

    mux_extension_record_clear(&record);
    mux_clipboard_broker_peer_unref(guard);
    return result;
}

gboolean
mux_clipboard_broker_peer_tick(MuxClipboardBrokerPeer *peer,
                               gint64 monotonic_us)
{
    g_return_val_if_fail(peer != NULL, FALSE);
    return mux_clipboard_wire_assembler_tick(peer->assembler,
                                             monotonic_us) ||
           (peer->pending_select_request_id != 0 &&
            peer->pending_select_deadline_us <= monotonic_us);
}

const gchar *
mux_clipboard_broker_peer_get_profile(
    const MuxClipboardBrokerPeer *peer)
{
    g_return_val_if_fail(peer != NULL, NULL);
    return peer->profile;
}

MuxClipboardHistoryMode
mux_clipboard_broker_peer_get_mode(
    const MuxClipboardBrokerPeer *peer)
{
    g_return_val_if_fail(peer != NULL,
                         MUX_CLIPBOARD_HISTORY_DISABLED);
    return peer->mode;
}

gboolean
mux_clipboard_broker_peer_is_closed(
    const MuxClipboardBrokerPeer *peer)
{
    g_return_val_if_fail(peer != NULL, TRUE);
    return peer->closed;
}
