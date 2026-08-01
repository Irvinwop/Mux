#include "mux-clipboard-broker-client.h"

#include <string.h>

typedef enum {
    PENDING_NONE,
    PENDING_HELLO,
    PENDING_LIST,
    PENDING_SELECT,
    PENDING_MUTATION,
    PENDING_BYE
} PendingKind;

struct _MuxClipboardBrokerClient {
    gint reference_count;
    gchar *profile;
    MuxClipboardHistoryMode mode;
    MuxClipboardBrokerClientState state;
    MuxClipboardBrokerClientOutputFunc output_func;
    MuxClipboardBrokerClientReadyFunc ready_func;
    MuxClipboardBrokerClientListFunc list_func;
    MuxClipboardBrokerClientSelectFunc select_func;
    MuxClipboardBrokerClientMutationFunc mutation_func;
    MuxClipboardBrokerClientFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    MuxClipboardWireAssembler *assembler;
    guint64 next_request_id;
    guint64 next_transaction_id;
    PendingKind pending_kind;
    MuxClipboardControlType pending_operation;
    guint64 pending_request_id;
    guint64 pending_entry_id;
    gboolean pending_paste;
    gint64 pending_deadline_us;
    GPtrArray *pending_summaries;
    MuxClipboardSnapshot *pending_snapshot;
};

static void
summary_free(MuxClipboardControlSummary *summary)
{
    if (summary == NULL)
        return;
    mux_clipboard_control_summary_clear(summary);
    g_free(summary);
}

static guint64
next_nonzero(guint64 *value)
{
    (*value)++;
    if (*value == 0)
        (*value)++;
    return *value;
}

static gint64
request_deadline(void)
{
    return g_get_monotonic_time() +
           ((gint64)MUX_CLIPBOARD_BROKER_CLIENT_TIMEOUT_MS * 1000);
}

static void
clear_pending(MuxClipboardBrokerClient *client)
{
    client->pending_kind = PENDING_NONE;
    client->pending_operation = 0;
    client->pending_request_id = 0;
    client->pending_entry_id = 0;
    client->pending_paste = FALSE;
    client->pending_deadline_us = 0;
    g_clear_pointer(&client->pending_summaries, g_ptr_array_unref);
    g_clear_pointer(&client->pending_snapshot,
                    mux_clipboard_snapshot_unref);
}

static void
report_failure(MuxClipboardBrokerClient *client,
               const gchar *operation,
               const GError *error)
{
    if (client->failure_func != NULL)
        client->failure_func(client,
                             operation,
                             error,
                             client->user_data);
}

static gboolean
output_extension(MuxClipboardBrokerClient *client,
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
    result = client->output_func(client,
                                 packet,
                                 client->user_data,
                                 error);
    if (!result && error != NULL && *error == NULL)
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "clipboard broker output rejected a packet");
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
send_control(MuxClipboardBrokerClient *client,
             const MuxClipboardControlRecord *record,
             GError **error)
{
    GBytes *payload = mux_clipboard_control_record_encode(record, error);
    gboolean result;

    if (payload == NULL)
        return FALSE;
    result = output_extension(client,
                              MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER,
                              payload,
                              error);
    g_bytes_unref(payload);
    return result;
}

static gboolean
begin_request(MuxClipboardBrokerClient *client,
              PendingKind kind,
              MuxClipboardControlType operation,
              guint64 entry_id,
              gboolean paste,
              GError **error)
{
    if (client->pending_kind != PENDING_NONE) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "a clipboard broker request is already pending");
        return FALSE;
    }

    client->pending_kind = kind;
    client->pending_operation = operation;
    client->pending_request_id =
        next_nonzero(&client->next_request_id);
    client->pending_entry_id = entry_id;
    client->pending_paste = paste;
    client->pending_deadline_us = request_deadline();
    return TRUE;
}

static gboolean
require_ready(MuxClipboardBrokerClient *client, GError **error)
{
    if (client->state == MUX_CLIPBOARD_BROKER_CLIENT_READY)
        return TRUE;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_INITIALIZED,
                        "clipboard broker client is not ready");
    return FALSE;
}

MuxClipboardBrokerClient *
mux_clipboard_broker_client_new(
    const gchar *profile,
    MuxClipboardHistoryMode mode,
    MuxClipboardBrokerClientOutputFunc output_func,
    MuxClipboardBrokerClientReadyFunc ready_func,
    MuxClipboardBrokerClientListFunc list_func,
    MuxClipboardBrokerClientSelectFunc select_func,
    MuxClipboardBrokerClientMutationFunc mutation_func,
    MuxClipboardBrokerClientFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy)
{
    MuxClipboardBrokerClient *client;
    gsize profile_length;

    g_return_val_if_fail(profile != NULL, NULL);
    profile_length = strlen(profile);
    g_return_val_if_fail(profile_length > 0 &&
                         profile_length <= MUX_CLIPBOARD_HISTORY_MAX_PROFILE &&
                         g_utf8_validate(profile, profile_length, NULL),
                         NULL);
    g_return_val_if_fail(mode >= MUX_CLIPBOARD_HISTORY_DISABLED &&
                         mode <= MUX_CLIPBOARD_HISTORY_EPHEMERAL,
                         NULL);
    g_return_val_if_fail(output_func != NULL, NULL);

    client = g_new0(MuxClipboardBrokerClient, 1);
    client->reference_count = 1;
    client->profile = g_strdup(profile);
    client->mode = mode;
    client->state = MUX_CLIPBOARD_BROKER_CLIENT_NEW;
    client->output_func = output_func;
    client->ready_func = ready_func;
    client->list_func = list_func;
    client->select_func = select_func;
    client->mutation_func = mutation_func;
    client->failure_func = failure_func;
    client->user_data = user_data;
    client->user_data_destroy = user_data_destroy;
    client->assembler = mux_clipboard_wire_assembler_new(0);
    client->next_request_id =
        ((guint64)g_random_int() << 32) | g_random_int();
    client->next_transaction_id =
        ((guint64)g_random_int() << 32) | g_random_int();
    if (client->assembler == NULL) {
        mux_clipboard_broker_client_unref(client);
        return NULL;
    }
    return client;
}

MuxClipboardBrokerClient *
mux_clipboard_broker_client_ref(MuxClipboardBrokerClient *client)
{
    g_return_val_if_fail(client != NULL, NULL);
    g_atomic_int_inc(&client->reference_count);
    return client;
}

void
mux_clipboard_broker_client_unref(MuxClipboardBrokerClient *client)
{
    if (client == NULL ||
        !g_atomic_int_dec_and_test(&client->reference_count))
        return;

    clear_pending(client);
    mux_clipboard_wire_assembler_free(client->assembler);
    if (client->user_data_destroy != NULL)
        client->user_data_destroy(client->user_data);
    g_free(client->profile);
    g_free(client);
}

gboolean
mux_clipboard_broker_client_start(MuxClipboardBrokerClient *client,
                                  GError **error)
{
    MuxClipboardControlRecord record = { 0 };
    GBytes *profile;

    g_return_val_if_fail(client != NULL, FALSE);
    if (client->state != MUX_CLIPBOARD_BROKER_CLIENT_NEW ||
        !begin_request(client,
                       PENDING_HELLO,
                       MUX_CLIPBOARD_CONTROL_HELLO,
                       0,
                       FALSE,
                       error)) {
        if (error != NULL && *error == NULL)
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_PENDING,
                                "clipboard broker client already started");
        return FALSE;
    }

    record.type = MUX_CLIPBOARD_CONTROL_HELLO;
    record.request_id = client->pending_request_id;
    if (client->mode == MUX_CLIPBOARD_HISTORY_DISABLED)
        record.flags = MUX_CLIPBOARD_CONTROL_FLAG_MODE_DISABLED;
    else if (client->mode == MUX_CLIPBOARD_HISTORY_EPHEMERAL)
        record.flags = MUX_CLIPBOARD_CONTROL_FLAG_MODE_EPHEMERAL;
    profile = g_bytes_new(client->profile, strlen(client->profile));
    record.payload = profile;
    client->state = MUX_CLIPBOARD_BROKER_CLIENT_HELLO_PENDING;
    if (!send_control(client, &record, error)) {
        client->state = MUX_CLIPBOARD_BROKER_CLIENT_NEW;
        clear_pending(client);
        g_bytes_unref(profile);
        return FALSE;
    }
    g_bytes_unref(profile);
    return TRUE;
}

gboolean
mux_clipboard_broker_client_observe(
    MuxClipboardBrokerClient *client,
    guint32 flags,
    const gchar *source_origin,
    guint64 source_view_id,
    const MuxClipboardSnapshot *snapshot,
    GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    g_return_val_if_fail(snapshot != NULL, FALSE);
    if (!require_ready(client, error))
        return FALSE;
    if (client->mode == MUX_CLIPBOARD_HISTORY_EPHEMERAL)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL;
    return mux_clipboard_wire_send_snapshot(
        next_nonzero(&client->next_transaction_id),
        flags | MUX_CLIPBOARD_WIRE_FLAG_CURRENT,
        client->profile,
        source_origin,
        source_view_id,
        g_get_monotonic_time(),
        snapshot,
        output_wire,
        client,
        error);
}

static gboolean
send_simple_request(MuxClipboardBrokerClient *client,
                    PendingKind kind,
                    MuxClipboardControlType operation,
                    guint64 entry_id,
                    guint32 flags,
                    gboolean paste,
                    GError **error)
{
    MuxClipboardControlRecord record = { 0 };

    if (!require_ready(client, error) ||
        !begin_request(client,
                       kind,
                       operation,
                       entry_id,
                       paste,
                       error))
        return FALSE;

    if (kind == PENDING_LIST)
        client->pending_summaries = g_ptr_array_new_with_free_func(
            (GDestroyNotify)summary_free);

    record.type = operation;
    record.flags = flags;
    record.request_id = client->pending_request_id;
    record.entry_id = entry_id;
    if (!send_control(client, &record, error)) {
        clear_pending(client);
        return FALSE;
    }
    return TRUE;
}

gboolean
mux_clipboard_broker_client_list(MuxClipboardBrokerClient *client,
                                 GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    if (!send_simple_request(client,
                             PENDING_LIST,
                             MUX_CLIPBOARD_CONTROL_LIST,
                             0,
                             0,
                             FALSE,
                             error))
        return FALSE;
    return TRUE;
}

gboolean
mux_clipboard_broker_client_select(MuxClipboardBrokerClient *client,
                                   guint64 entry_id,
                                   gboolean paste,
                                   GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    if (entry_id == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard history entry id is required");
        return FALSE;
    }
    return send_simple_request(
        client,
        PENDING_SELECT,
        MUX_CLIPBOARD_CONTROL_SELECT,
        entry_id,
        paste ? MUX_CLIPBOARD_CONTROL_FLAG_PASTE : 0,
        paste,
        error);
}

gboolean
mux_clipboard_broker_client_set_pinned(
    MuxClipboardBrokerClient *client,
    guint64 entry_id,
    gboolean pinned,
    GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    if (entry_id == 0)
        return FALSE;
    return send_simple_request(
        client,
        PENDING_MUTATION,
        MUX_CLIPBOARD_CONTROL_PIN,
        entry_id,
        pinned ? MUX_CLIPBOARD_CONTROL_FLAG_PINNED : 0,
        FALSE,
        error);
}

gboolean
mux_clipboard_broker_client_delete(MuxClipboardBrokerClient *client,
                                   guint64 entry_id,
                                   GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    if (entry_id == 0)
        return FALSE;
    return send_simple_request(client,
                               PENDING_MUTATION,
                               MUX_CLIPBOARD_CONTROL_DELETE,
                               entry_id,
                               0,
                               FALSE,
                               error);
}

gboolean
mux_clipboard_broker_client_clear(MuxClipboardBrokerClient *client,
                                  gboolean include_pinned,
                                  GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    return send_simple_request(
        client,
        PENDING_MUTATION,
        MUX_CLIPBOARD_CONTROL_CLEAR,
        0,
        include_pinned
            ? MUX_CLIPBOARD_CONTROL_FLAG_INCLUDE_PINNED
            : 0,
        FALSE,
        error);
}

gboolean
mux_clipboard_broker_client_close(MuxClipboardBrokerClient *client,
                                  GError **error)
{
    g_return_val_if_fail(client != NULL, FALSE);
    return send_simple_request(client,
                               PENDING_BYE,
                               MUX_CLIPBOARD_CONTROL_BYE,
                               0,
                               0,
                               FALSE,
                               error);
}

static const gchar *
pending_name(const MuxClipboardBrokerClient *client)
{
    switch (client->pending_kind) {
    case PENDING_HELLO:
        return "clipboard-hello";
    case PENDING_LIST:
        return "clipboard-list";
    case PENDING_SELECT:
        return "clipboard-select";
    case PENDING_MUTATION:
        return "clipboard-mutation";
    case PENDING_BYE:
        return "clipboard-bye";
    case PENDING_NONE:
    default:
        return "clipboard-broker";
    }
}

static gboolean
handle_ok(MuxClipboardBrokerClient *client,
          const MuxClipboardControlRecord *record,
          GError **error)
{
    PendingKind kind = client->pending_kind;
    MuxClipboardControlType operation = client->pending_operation;
    guint64 entry_id = client->pending_entry_id;
    gboolean paste = client->pending_paste;

    if (record->flags != 0 || g_bytes_get_size(record->payload) != 0)
        return FALSE;
    if (kind == PENDING_SELECT &&
        (client->pending_snapshot == NULL ||
         record->entry_id != entry_id)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard selection completed without its snapshot");
        return FALSE;
    }

    if (kind == PENDING_HELLO) {
        clear_pending(client);
        client->state = MUX_CLIPBOARD_BROKER_CLIENT_READY;
        if (client->ready_func != NULL)
            client->ready_func(client, client->user_data);
    } else if (kind == PENDING_SELECT) {
        MuxClipboardSnapshot *snapshot = client->pending_snapshot;

        client->pending_snapshot = NULL;
        clear_pending(client);
        if (client->select_func != NULL)
            client->select_func(client,
                                entry_id,
                                snapshot,
                                paste,
                                client->user_data);
        mux_clipboard_snapshot_unref(snapshot);
    } else if (kind == PENDING_MUTATION) {
        guint64 value = record->entry_id;

        clear_pending(client);
        if (client->mutation_func != NULL)
            client->mutation_func(client,
                                  operation,
                                  value,
                                  client->user_data);
    } else if (kind == PENDING_BYE) {
        clear_pending(client);
        client->state = MUX_CLIPBOARD_BROKER_CLIENT_CLOSED;
    } else {
        return FALSE;
    }
    return TRUE;
}

static gboolean
handle_control(MuxClipboardBrokerClient *client,
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
    if (client->pending_kind == PENDING_NONE ||
        record.request_id != client->pending_request_id) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard broker response has no matching request");
        goto out;
    }

    switch (record.type) {
    case MUX_CLIPBOARD_CONTROL_SUMMARY:
        if (client->pending_kind == PENDING_LIST &&
            client->pending_summaries->len <
                MUX_CLIPBOARD_HISTORY_MAX_ENTRIES) {
            MuxClipboardControlSummary *summary =
                g_new0(MuxClipboardControlSummary, 1);

            if (mux_clipboard_control_summary_decode(&record,
                                                     summary,
                                                     error)) {
                g_ptr_array_add(client->pending_summaries, summary);
                client->pending_deadline_us = request_deadline();
                result = TRUE;
            } else {
                summary_free(summary);
            }
        }
        break;
    case MUX_CLIPBOARD_CONTROL_LIST_DONE:
        if (client->pending_kind == PENDING_LIST && record.flags == 0 &&
            record.entry_id == 0 &&
            g_bytes_get_size(record.payload) == 0) {
            GPtrArray *summaries = client->pending_summaries;

            client->pending_summaries = NULL;
            clear_pending(client);
            if (client->list_func != NULL)
                client->list_func(client,
                                  summaries,
                                  client->user_data);
            g_ptr_array_unref(summaries);
            result = TRUE;
        }
        break;
    case MUX_CLIPBOARD_CONTROL_OK:
        result = handle_ok(client, &record, error);
        break;
    case MUX_CLIPBOARD_CONTROL_REMOTE_ERROR: {
        g_autofree gchar *message = NULL;
        g_autoptr(GError) remote_error = NULL;
        const gchar *operation = pending_name(client);

        if (!mux_clipboard_control_payload_text(
                &record,
                MUX_CLIPBOARD_CONTROL_MAX_TEXT,
                FALSE,
                &message,
                error))
            break;
        remote_error = g_error_new_literal(G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           message);
        if (client->pending_kind == PENDING_HELLO)
            client->state = MUX_CLIPBOARD_BROKER_CLIENT_NEW;
        clear_pending(client);
        report_failure(client, operation, remote_error);
        result = TRUE;
        break;
    }
    case MUX_CLIPBOARD_CONTROL_HELLO:
    case MUX_CLIPBOARD_CONTROL_LIST:
    case MUX_CLIPBOARD_CONTROL_SELECT:
    case MUX_CLIPBOARD_CONTROL_DELETE:
    case MUX_CLIPBOARD_CONTROL_PIN:
    case MUX_CLIPBOARD_CONTROL_CLEAR:
    case MUX_CLIPBOARD_CONTROL_BYE:
    default:
        break;
    }

    if (!result && (error == NULL || *error == NULL))
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "invalid clipboard broker response");

out:
    mux_clipboard_control_record_clear(&record);
    return result;
}

static gboolean
send_wire_ack(MuxClipboardBrokerClient *client,
              guint64 transaction_id,
              GError **error)
{
    MuxClipboardWireRecord record = {
        .type = MUX_CLIPBOARD_WIRE_ACK,
        .transaction_id = transaction_id
    };
    GBytes *payload = mux_clipboard_wire_record_encode(&record, error);
    gboolean result;

    if (payload == NULL)
        return FALSE;
    result = output_extension(client,
                              MUX_EXTENSION_CHANNEL_CLIPBOARD,
                              payload,
                              error);
    g_bytes_unref(payload);
    return result;
}

static gboolean
handle_snapshot(MuxClipboardBrokerClient *client,
                GBytes *payload,
                GError **error)
{
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    g_autoptr(GError) feed_error = NULL;
    const guint8 *data;
    gsize length;
    guint32 flags;
    guint64 transaction_id;

    data = g_bytes_get_data(payload, &length);
    if (!mux_clipboard_wire_record_decode(data, length, &record, error))
        return FALSE;
    if (record.type == MUX_CLIPBOARD_WIRE_ACK) {
        mux_clipboard_wire_record_clear(&record);
        return TRUE;
    }
    if (record.type == MUX_CLIPBOARD_WIRE_REMOTE_ERROR) {
        mux_clipboard_wire_record_clear(&record);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "clipboard broker rejected an observed snapshot");
        return FALSE;
    }

    feed_result = mux_clipboard_wire_assembler_feed(client->assembler,
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

    if (client->pending_kind != PENDING_SELECT ||
        client->pending_snapshot != NULL ||
        mux_clipboard_wire_transfer_get_profile(transfer) == NULL ||
        !g_str_equal(mux_clipboard_wire_transfer_get_profile(transfer),
                     client->profile)) {
        mux_clipboard_wire_transfer_free(transfer);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "unexpected clipboard selection snapshot");
        return FALSE;
    }

    flags = mux_clipboard_wire_transfer_get_flags(transfer);
    if (!(flags & MUX_CLIPBOARD_WIRE_FLAG_CURRENT) ||
        !!(flags & MUX_CLIPBOARD_WIRE_FLAG_PASTE) !=
            client->pending_paste) {
        mux_clipboard_wire_transfer_free(transfer);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard selection flags do not match the request");
        return FALSE;
    }

    client->pending_snapshot = mux_clipboard_snapshot_ref(
        (MuxClipboardSnapshot *)
            mux_clipboard_wire_transfer_get_snapshot(transfer));
    client->pending_deadline_us = request_deadline();
    transaction_id =
        mux_clipboard_wire_transfer_get_transaction_id(transfer);
    mux_clipboard_wire_transfer_free(transfer);
    return send_wire_ack(client, transaction_id, error);
}

gboolean
mux_clipboard_broker_client_handle_packet(
    MuxClipboardBrokerClient *client,
    const guint8 *packet,
    gsize packet_length,
    GError **error)
{
    MuxClipboardBrokerClient *guard;
    MuxExtensionRecord record = { 0 };
    gboolean result;

    g_return_val_if_fail(client != NULL, FALSE);
    guard = mux_clipboard_broker_client_ref(client);
    if (!mux_extension_record_decode(packet,
                                     packet_length,
                                     &record,
                                     error)) {
        mux_clipboard_broker_client_unref(guard);
        return FALSE;
    }

    if (record.channel == MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER)
        result = handle_control(client, record.payload, error);
    else if (record.channel == MUX_EXTENSION_CHANNEL_CLIPBOARD)
        result = handle_snapshot(client, record.payload, error);
    else {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "clipboard broker response channel is unsupported");
        result = FALSE;
    }

    mux_extension_record_clear(&record);
    mux_clipboard_broker_client_unref(guard);
    return result;
}

guint
mux_clipboard_broker_client_tick(MuxClipboardBrokerClient *client,
                                 gint64 monotonic_us)
{
    MuxClipboardBrokerClient *guard;
    guint expired = 0;

    g_return_val_if_fail(client != NULL, 0);
    guard = mux_clipboard_broker_client_ref(client);
    if (client->pending_kind != PENDING_NONE &&
        client->pending_deadline_us <= monotonic_us) {
        g_autoptr(GError) timeout = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_TIMED_OUT,
            "clipboard broker request timed out");
        const gchar *operation = pending_name(client);

        if (client->pending_kind == PENDING_HELLO)
            client->state = MUX_CLIPBOARD_BROKER_CLIENT_NEW;
        clear_pending(client);
        report_failure(client, operation, timeout);
        expired++;
    }
    if (mux_clipboard_wire_assembler_tick(client->assembler,
                                          monotonic_us)) {
        g_autoptr(GError) timeout = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_TIMED_OUT,
            "clipboard selection snapshot timed out");

        clear_pending(client);
        report_failure(client, "clipboard-select", timeout);
        expired++;
    }
    mux_clipboard_broker_client_unref(guard);
    return expired;
}

MuxClipboardBrokerClientState
mux_clipboard_broker_client_get_state(
    const MuxClipboardBrokerClient *client)
{
    g_return_val_if_fail(client != NULL,
                         MUX_CLIPBOARD_BROKER_CLIENT_CLOSED);
    return client->state;
}

gboolean
mux_clipboard_broker_client_request_pending(
    const MuxClipboardBrokerClient *client)
{
    g_return_val_if_fail(client != NULL, FALSE);
    return client->pending_kind != PENDING_NONE;
}
