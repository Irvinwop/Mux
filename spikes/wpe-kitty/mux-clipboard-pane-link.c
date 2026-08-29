#include "mux-clipboard-pane-link.h"
#include "mux-clipboard-lifetime.h"

#include <string.h>

#define MUX_CLIPBOARD_REMOTE_ERROR_MAX_BYTES 512U

typedef struct {
    guint64 transaction_id;
    guint32 flags;
    gchar *source_origin;
    guint64 source_view_id;
    MuxClipboardSnapshot *snapshot;
    gchar *rejection_reason;
    gboolean observed;
    gboolean rejected;
} PendingEngineWrite;

typedef struct {
    gchar *source_origin;
    guint64 source_view_id;
    MuxClipboardSnapshot *snapshot;
    gboolean paste;
    gchar *rejection_reason;
    gboolean rejected;
} PendingHistoryApply;

struct _MuxClipboardPaneLink {
    MuxClipboardLifetime lifetime;
    gchar *profile;
    gboolean ephemeral;
    guint64 view_id;
    guint64 next_transaction_id;
    MuxKittyClipboard *kitty;
    MuxClipboardWireAssembler *assembler;
    PendingEngineWrite *pending_engine_write;
    PendingHistoryApply *pending_history_apply;
    MuxClipboardPaneTerminalOutputFunc terminal_output_func;
    MuxClipboardPaneWireOutputFunc wire_output_func;
    MuxClipboardPaneObserveFunc observe_func;
    MuxClipboardPaneFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

static void pane_link_destroy(MuxClipboardPaneLink *link);

static MuxClipboardPaneLink *
pane_link_acquire(MuxClipboardPaneLink *link)
{
    mux_clipboard_lifetime_acquire(&link->lifetime);
    return link;
}

static void
pane_link_release(MuxClipboardPaneLink *link)
{
    if (mux_clipboard_lifetime_release(&link->lifetime))
        pane_link_destroy(link);
}

typedef MuxClipboardPaneLink MuxClipboardPaneLinkOperation;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardPaneLinkOperation,
                              pane_link_release)

static void report_failure(MuxClipboardPaneLink *link,
                           const gchar *operation,
                           const GError *error);
static gboolean send_snapshot(MuxClipboardPaneLink *link,
                              guint32 flags,
                              const gchar *source_origin,
                              guint64 source_view_id,
                              const MuxClipboardSnapshot *snapshot,
                              GError **error);

static void
pending_engine_write_free(PendingEngineWrite *pending)
{
    if (pending == NULL)
        return;
    g_free(pending->source_origin);
    g_free(pending->rejection_reason);
    mux_clipboard_snapshot_unref(pending->snapshot);
    g_free(pending);
}

static void
pending_history_apply_free(PendingHistoryApply *pending)
{
    if (pending == NULL)
        return;
    g_free(pending->source_origin);
    g_free(pending->rejection_reason);
    mux_clipboard_snapshot_unref(pending->snapshot);
    g_free(pending);
}

static gchar *
bounded_failure_reason(const gchar *message)
{
    g_autofree gchar *valid = NULL;
    gsize length;
    gsize i;

    if (message == NULL || message[0] == '\0')
        return NULL;
    valid = g_utf8_make_valid(message, -1);
    length = MIN(strlen(valid),
                 (gsize)MUX_CLIPBOARD_REMOTE_ERROR_MAX_BYTES);
    while (length > 0 && !g_utf8_validate(valid, length, NULL))
        length--;
    for (i = 0; i < length; i++) {
        if ((guchar)valid[i] < 0x20 || (guchar)valid[i] == 0x7f)
            valid[i] = ' ';
    }
    return g_strndup(valid, length);
}

static void
set_pending_rejection(PendingEngineWrite *pending, const gchar *reason)
{
    gchar *bounded;

    if (pending == NULL)
        return;
    bounded = bounded_failure_reason(reason);
    g_free(pending->rejection_reason);
    pending->rejection_reason = bounded;
    pending->rejected = TRUE;
}

static gboolean
send_engine_result(MuxClipboardPaneLink *link,
                   MuxClipboardWireType type,
                   guint64 transaction_id,
                   const gchar *reason,
                   GError **error)
{
    g_autoptr(GBytes) payload = NULL;
    MuxClipboardWireRecord record = {
        .type = type,
        .transaction_id = transaction_id
    };
    GBytes *packet;
    gboolean result;

    if (reason != NULL && reason[0] != '\0') {
        payload = g_bytes_new(reason, strlen(reason));
        record.payload = payload;
    }
    packet = mux_clipboard_wire_record_encode(&record, error);
    if (packet == NULL)
        return FALSE;
    result = link->wire_output_func(link,
                                    packet,
                                    link->user_data,
                                    error);
    g_bytes_unref(packet);
    return result;
}

static gboolean
reject_pending_engine_write(MuxClipboardPaneLink *link,
                            const gchar *fallback_reason,
                            GError **error)
{
    PendingEngineWrite *pending = link->pending_engine_write;
    const gchar *reason;
    gboolean result;

    if (pending == NULL)
        return TRUE;
    link->pending_engine_write = NULL;
    reason = pending->rejection_reason != NULL
                 ? pending->rejection_reason
                 : fallback_reason;
    result = send_engine_result(link,
                                MUX_CLIPBOARD_WIRE_REMOTE_ERROR,
                                pending->transaction_id,
                                reason,
                                error);
    pending_engine_write_free(pending);
    return result;
}

static gboolean
complete_pending_engine_write(MuxClipboardPaneLink *link, GError **error)
{
    PendingEngineWrite *pending = link->pending_engine_write;
    guint64 transaction_id;
    gboolean result;

    if (pending == NULL || mux_kitty_clipboard_write_pending(link->kitty))
        return TRUE;
    if (pending->rejected)
        return reject_pending_engine_write(
            link, "Kitty rejected clipboard transaction", error);
    transaction_id = pending->transaction_id;
    mux_clipboard_smoke_trace(
        MUX_CLIPBOARD_TRACE_KITTY_WRITE_DONE,
        &(MuxClipboardTraceFields) {
            .transaction_id = transaction_id,
            .view_id = pending->source_view_id,
            .snapshot = pending->snapshot
        });
    if (!pending->observed &&
        (pending->flags & MUX_CLIPBOARD_WIRE_FLAG_HISTORY) &&
        link->observe_func != NULL) {
        pending->observed = TRUE;
        link->observe_func(link,
                           link->profile,
                           pending->source_origin,
                           pending->source_view_id,
                           pending->flags,
                           pending->snapshot,
                           link->user_data);
        if (link->pending_engine_write != pending)
            return TRUE;
    }
    link->pending_engine_write = NULL;
    result = send_engine_result(link,
                                MUX_CLIPBOARD_WIRE_ACK,
                                transaction_id,
                                NULL,
                                error);
    pending_engine_write_free(pending);
    return result;
}

static gboolean
complete_pending_history_apply(MuxClipboardPaneLink *link, GError **error)
{
    PendingHistoryApply *pending = link->pending_history_apply;
    guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT;
    gboolean result;

    if (pending == NULL || mux_kitty_clipboard_write_pending(link->kitty))
        return TRUE;
    link->pending_history_apply = NULL;
    if (pending->rejected) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            pending->rejection_reason != NULL
                                ? pending->rejection_reason
                                : "Kitty rejected clipboard history application");
        pending_history_apply_free(pending);
        return FALSE;
    }
    if (pending->paste)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PASTE;
    result = send_snapshot(link,
                           flags,
                           pending->source_origin,
                           pending->source_view_id,
                           pending->snapshot,
                           error);
    pending_history_apply_free(pending);
    return result;
}

static guint64
next_transaction(MuxClipboardPaneLink *link)
{
    link->next_transaction_id++;
    if (link->next_transaction_id == 0)
        link->next_transaction_id++;
    return link->next_transaction_id;
}

static void
report_failure(MuxClipboardPaneLink *link,
               const gchar *operation,
               const GError *error)
{
    if (link->failure_func != NULL)
        link->failure_func(link, operation, error, link->user_data);
}

static gboolean
terminal_output(MuxKittyClipboard *clipboard,
                GBytes *packet,
                gpointer user_data,
                GError **error)
{
    MuxClipboardPaneLink *link = user_data;

    (void)clipboard;
    return link->terminal_output_func(link,
                                      packet,
                                      link->user_data,
                                      error);
}

static gboolean
wire_output(GBytes *packet, gpointer user_data, GError **error)
{
    MuxClipboardPaneLink *link = user_data;

    return link->wire_output_func(link,
                                  packet,
                                  link->user_data,
                                  error);
}

static void
kitty_failure(MuxKittyClipboard *clipboard,
              const gchar *operation,
              const GError *error,
              gpointer user_data)
{
    MuxClipboardPaneLink *link = user_data;

    (void)clipboard;
    if (g_strcmp0(operation, "clipboard-write") == 0 &&
        link->pending_engine_write != NULL) {
        set_pending_rejection(link->pending_engine_write,
                              error != NULL ? error->message : NULL);
    }
    if (g_strcmp0(operation, "clipboard-write") == 0 &&
        link->pending_history_apply != NULL) {
        g_free(link->pending_history_apply->rejection_reason);
        link->pending_history_apply->rejection_reason =
            bounded_failure_reason(error != NULL ? error->message : NULL);
        link->pending_history_apply->rejected = TRUE;
    }
    report_failure(link, operation, error);
}

static gboolean
send_snapshot(MuxClipboardPaneLink *link,
              guint32 flags,
              const gchar *source_origin,
              guint64 source_view_id,
              const MuxClipboardSnapshot *snapshot,
              GError **error)
{
    if (link->ephemeral)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL;
    return mux_clipboard_wire_send_snapshot(next_transaction(link),
                                            flags,
                                            link->profile,
                                            source_origin,
                                            source_view_id,
                                            g_get_monotonic_time(),
                                            snapshot,
                                            wire_output,
                                            link,
                                            error);
}

static void
kitty_receive(MuxKittyClipboard *clipboard,
              MuxOsc5522Location location,
              MuxClipboardSnapshot *snapshot,
              gboolean is_paste,
              gpointer user_data)
{
    MuxClipboardPaneLink *link = user_data;
    g_autoptr(GError) error = NULL;
    guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT |
                    MUX_CLIPBOARD_WIRE_FLAG_HISTORY;
    guint64 view_id = link->view_id;

    (void)clipboard;
    if (is_paste)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PASTE;
    if (location == MUX_OSC5522_LOCATION_PRIMARY)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PRIMARY;

    if (link->observe_func != NULL)
        link->observe_func(link,
                           link->profile,
                           "external",
                           view_id,
                           flags,
                           snapshot,
                           link->user_data);
    if (!send_snapshot(link,
                       flags,
                       "external",
                       view_id,
                       snapshot,
                       &error))
        report_failure(link, "clipboard-to-engine", error);
}

MuxClipboardPaneLink *
mux_clipboard_pane_link_new(
    const gchar *profile,
    gboolean ephemeral,
    guint64 view_id,
    MuxClipboardPaneTerminalOutputFunc terminal_output_func,
    MuxClipboardPaneWireOutputFunc wire_output_func,
    MuxClipboardPaneObserveFunc observe_func,
    MuxClipboardPaneFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy)
{
    MuxClipboardPaneLink *link;
    gsize profile_length;

    g_return_val_if_fail(profile != NULL, NULL);
    profile_length = strlen(profile);
    g_return_val_if_fail(profile_length > 0 &&
                         profile_length <= MUX_CLIPBOARD_WIRE_MAX_PROFILE &&
                         g_utf8_validate(profile, profile_length, NULL),
                         NULL);
    g_return_val_if_fail(terminal_output_func != NULL, NULL);
    g_return_val_if_fail(wire_output_func != NULL, NULL);

    link = g_new0(MuxClipboardPaneLink, 1);
    mux_clipboard_lifetime_init(&link->lifetime);
    link->profile = g_strdup(profile);
    link->ephemeral = ephemeral;
    link->view_id = view_id;
    link->next_transaction_id =
        ((guint64)g_random_int() << 32) | g_random_int();
    link->terminal_output_func = terminal_output_func;
    link->wire_output_func = wire_output_func;
    link->observe_func = observe_func;
    link->failure_func = failure_func;
    link->user_data = user_data;
    link->user_data_destroy = user_data_destroy;
    link->assembler = mux_clipboard_wire_assembler_new(0);
    link->kitty = mux_kitty_clipboard_new(terminal_output,
                                          kitty_receive,
                                          kitty_failure,
                                          link,
                                          NULL);
    if (link->assembler == NULL || link->kitty == NULL) {
        mux_clipboard_pane_link_free(link);
        return NULL;
    }
    return link;
}

static void
pane_link_destroy(MuxClipboardPaneLink *link)
{
    mux_kitty_clipboard_unref(link->kitty);
    mux_clipboard_wire_assembler_free(link->assembler);
    pending_engine_write_free(link->pending_engine_write);
    pending_history_apply_free(link->pending_history_apply);
    if (link->user_data_destroy != NULL)
        link->user_data_destroy(link->user_data);
    g_free(link->profile);
    g_free(link);
}

void
mux_clipboard_pane_link_free(MuxClipboardPaneLink *link)
{
    if (link != NULL &&
        mux_clipboard_lifetime_release_owner(&link->lifetime))
        pane_link_destroy(link);
}

void
mux_clipboard_pane_link_set_view_id(MuxClipboardPaneLink *link,
                                    guint64 view_id)
{
    g_return_if_fail(link != NULL);
    link->view_id = view_id;
}

gboolean
mux_clipboard_pane_link_set_enabled(MuxClipboardPaneLink *link,
                                    gboolean enabled,
                                    GError **error)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;
    g_autoptr(GError) reject_error = NULL;
    gboolean rejection_sent = TRUE;
    gboolean result;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = pane_link_acquire(link);
    (void)operation;
    if (!enabled && link->pending_engine_write != NULL) {
        rejection_sent = reject_pending_engine_write(
            link,
            "Kitty clipboard was disabled before write completion",
            &reject_error);
    }
    if (!enabled) {
        g_clear_pointer(&link->pending_history_apply,
                        pending_history_apply_free);
    }
    result = mux_kitty_clipboard_set_enabled(link->kitty, enabled, error);
    if (!rejection_sent) {
        report_failure(link, "clipboard-engine-reject", reject_error);
        if (result && error != NULL && *error == NULL)
            *error = g_error_copy(reject_error);
        return FALSE;
    }
    return result;
}

gboolean
mux_clipboard_pane_link_handle_support(MuxClipboardPaneLink *link,
                                       const guint8 *sequence,
                                       gsize length,
                                       GError **error)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = pane_link_acquire(link);
    (void)operation;
    return mux_kitty_clipboard_handle_support(link->kitty,
                                              sequence,
                                              length,
                                              error);
}

gboolean
mux_clipboard_pane_link_handle_osc(MuxClipboardPaneLink *link,
                                   const guint8 *sequence,
                                   gsize length,
                                   GError **error)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;
    gboolean result;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = pane_link_acquire(link);
    (void)operation;
    result = mux_kitty_clipboard_handle_osc(link->kitty,
                                           sequence,
                                           length,
                                           error);
    if (result)
        result = complete_pending_engine_write(link, error);
    if (result)
        result = complete_pending_history_apply(link, error);
    return result;
}

gboolean
mux_clipboard_pane_link_handle_packet(MuxClipboardPaneLink *link,
                                      const guint8 *packet,
                                      gsize packet_length,
                                      GError **error)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    const MuxClipboardSnapshot *snapshot;
    MuxOsc5522Location location;
    guint32 flags;
    guint64 transaction_id;
    g_autoptr(GError) feed_error = NULL;
    g_autoptr(GError) publish_error = NULL;
    gboolean result;

    g_return_val_if_fail(link != NULL, FALSE);
    operation = pane_link_acquire(link);
    (void)operation;
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
        mux_clipboard_wire_record_clear(&record);
        feed_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "engine rejected clipboard transaction");
        report_failure(link, "clipboard-wire", feed_error);
        g_propagate_error(error, g_steal_pointer(&feed_error));
        return FALSE;
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

    transaction_id =
        mux_clipboard_wire_transfer_get_transaction_id(transfer);
    if (mux_clipboard_wire_transfer_get_profile(transfer) == NULL ||
        !g_str_equal(mux_clipboard_wire_transfer_get_profile(transfer),
                     link->profile)) {
        g_autoptr(GError) boundary_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "clipboard transfer crossed profile boundary");

        result = send_engine_result(link,
                                    MUX_CLIPBOARD_WIRE_REMOTE_ERROR,
                                    transaction_id,
                                    boundary_error->message,
                                    error);
        report_failure(link, "clipboard-wire", boundary_error);
        mux_clipboard_wire_transfer_free(transfer);
        return result;
    }

    flags = mux_clipboard_wire_transfer_get_flags(transfer);
    snapshot = mux_clipboard_wire_transfer_get_snapshot(transfer);
    mux_clipboard_smoke_trace(
        MUX_CLIPBOARD_TRACE_ENGINE_TO_PANE,
        &(MuxClipboardTraceFields) {
            .transaction_id = transaction_id,
            .view_id =
                mux_clipboard_wire_transfer_get_source_view_id(transfer),
            .snapshot = snapshot
        });
    if (link->pending_engine_write != NULL ||
        mux_kitty_clipboard_write_pending(link->kitty)) {
        result = send_engine_result(link,
                                    MUX_CLIPBOARD_WIRE_REMOTE_ERROR,
                                    transaction_id,
                                    "Kitty clipboard write is already pending",
                                    error);
        mux_clipboard_wire_transfer_free(transfer);
        return result;
    }
    location = flags & MUX_CLIPBOARD_WIRE_FLAG_PRIMARY
                   ? MUX_OSC5522_LOCATION_PRIMARY
                   : MUX_OSC5522_LOCATION_CLIPBOARD;
    link->pending_engine_write = g_new0(PendingEngineWrite, 1);
    link->pending_engine_write->transaction_id = transaction_id;
    link->pending_engine_write->flags = flags;
    link->pending_engine_write->source_origin = g_strdup(
        mux_clipboard_wire_transfer_get_source_origin(transfer));
    link->pending_engine_write->source_view_id =
        mux_clipboard_wire_transfer_get_source_view_id(transfer);
    link->pending_engine_write->snapshot =
        mux_clipboard_snapshot_ref((MuxClipboardSnapshot *)snapshot);

    result = mux_kitty_clipboard_publish(link->kitty,
                                         location,
                                         snapshot,
                                         &publish_error);
    if (!result) {
        gboolean failure_reported =
            link->pending_engine_write != NULL &&
            link->pending_engine_write->rejected;

        if (!failure_reported && publish_error == NULL) {
            publish_error = g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "Kitty rejected clipboard publication");
        }
        if (!failure_reported) {
            set_pending_rejection(link->pending_engine_write,
                                  publish_error->message);
        }
        result = reject_pending_engine_write(
            link, "Kitty rejected clipboard publication", error);
        if (!failure_reported) {
            if (publish_error == NULL) {
                publish_error = g_error_new_literal(
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Kitty rejected clipboard publication");
            }
            report_failure(link, "clipboard-write", publish_error);
        }
    } else {
        result = complete_pending_engine_write(link, error);
    }
    mux_clipboard_wire_transfer_free(transfer);
    return result;
}

gboolean
mux_clipboard_pane_link_write_pending(const MuxClipboardPaneLink *link)
{
    g_return_val_if_fail(link != NULL, FALSE);
    return link->pending_engine_write != NULL ||
           link->pending_history_apply != NULL ||
           mux_kitty_clipboard_write_pending(link->kitty);
}

gboolean
mux_clipboard_pane_link_apply_history(
    MuxClipboardPaneLink *link,
    const MuxClipboardSnapshot *snapshot,
    const gchar *source_origin,
    guint64 source_view_id,
    gboolean paste,
    GError **error)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;
    PendingHistoryApply *pending;
    gboolean result;

    g_return_val_if_fail(link != NULL, FALSE);
    g_return_val_if_fail(snapshot != NULL, FALSE);
    operation = pane_link_acquire(link);
    (void)operation;
    if (link->pending_engine_write != NULL ||
        link->pending_history_apply != NULL ||
        mux_kitty_clipboard_write_pending(link->kitty)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "Kitty clipboard write is already pending");
        return FALSE;
    }
    pending = g_new0(PendingHistoryApply, 1);
    pending->source_origin = g_strdup(source_origin);
    pending->source_view_id = source_view_id;
    pending->snapshot = mux_clipboard_snapshot_ref(
        (MuxClipboardSnapshot *)snapshot);
    pending->paste = paste;
    link->pending_history_apply = pending;
    result = mux_kitty_clipboard_publish(link->kitty,
                                         MUX_OSC5522_LOCATION_CLIPBOARD,
                                         snapshot,
                                         error);
    if (!result) {
        link->pending_history_apply = NULL;
        pending_history_apply_free(pending);
        return FALSE;
    }
    return complete_pending_history_apply(link, error);
}

guint
mux_clipboard_pane_link_tick(MuxClipboardPaneLink *link,
                             gint64 monotonic_us)
{
    g_autoptr(MuxClipboardPaneLinkOperation) operation = NULL;
    g_autoptr(GError) error = NULL;
    guint expired;

    g_return_val_if_fail(link != NULL, 0);
    operation = pane_link_acquire(link);
    (void)operation;
    expired = mux_kitty_clipboard_tick(link->kitty, monotonic_us);
    if (!complete_pending_engine_write(link, &error)) {
        report_failure(link, "clipboard-engine-ack", error);
        g_clear_error(&error);
    }
    if (!complete_pending_history_apply(link, &error)) {
        report_failure(link, "clipboard-history-apply", error);
        g_clear_error(&error);
    }
    if (mux_clipboard_wire_assembler_tick(link->assembler,
                                          monotonic_us)) {
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_TIMED_OUT,
            "engine clipboard transfer timed out");
        report_failure(link, "clipboard-wire", error);
        expired++;
    }
    return expired;
}
