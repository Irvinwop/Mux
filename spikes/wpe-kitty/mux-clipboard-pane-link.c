#include "mux-clipboard-pane-link.h"

#include <string.h>

struct _MuxClipboardPaneLink {
    gchar *profile;
    gboolean ephemeral;
    guint64 view_id;
    guint64 next_transaction_id;
    MuxKittyClipboard *kitty;
    MuxClipboardWireAssembler *assembler;
    MuxClipboardPaneTerminalOutputFunc terminal_output_func;
    MuxClipboardPaneWireOutputFunc wire_output_func;
    MuxClipboardPaneObserveFunc observe_func;
    MuxClipboardPaneFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

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

    (void)clipboard;
    if (is_paste)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PASTE;
    if (location == MUX_OSC5522_LOCATION_PRIMARY)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PRIMARY;

    if (link->observe_func != NULL)
        link->observe_func(link,
                           link->profile,
                           "external",
                           link->view_id,
                           flags,
                           snapshot,
                           link->user_data);
    if (!send_snapshot(link,
                       flags,
                       "external",
                       link->view_id,
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

void
mux_clipboard_pane_link_free(MuxClipboardPaneLink *link)
{
    if (link == NULL)
        return;

    mux_kitty_clipboard_unref(link->kitty);
    mux_clipboard_wire_assembler_free(link->assembler);
    if (link->user_data_destroy != NULL)
        link->user_data_destroy(link->user_data);
    g_free(link->profile);
    g_free(link);
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
    g_return_val_if_fail(link != NULL, FALSE);
    return mux_kitty_clipboard_set_enabled(link->kitty, enabled, error);
}

gboolean
mux_clipboard_pane_link_handle_support(MuxClipboardPaneLink *link,
                                       const guint8 *sequence,
                                       gsize length,
                                       GError **error)
{
    g_return_val_if_fail(link != NULL, FALSE);
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
    g_return_val_if_fail(link != NULL, FALSE);
    return mux_kitty_clipboard_handle_osc(link->kitty,
                                         sequence,
                                         length,
                                         error);
}

static gboolean
send_ack(MuxClipboardPaneLink *link,
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
    result = link->wire_output_func(link,
                                    packet,
                                    link->user_data,
                                    error);
    g_bytes_unref(packet);
    return result;
}

gboolean
mux_clipboard_pane_link_handle_packet(MuxClipboardPaneLink *link,
                                      const guint8 *packet,
                                      gsize packet_length,
                                      GError **error)
{
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireTransfer *transfer = NULL;
    MuxClipboardWireFeedResult feed_result;
    const MuxClipboardSnapshot *snapshot;
    MuxOsc5522Location location;
    guint32 flags;
    g_autoptr(GError) feed_error = NULL;
    gboolean result;

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

    if (mux_clipboard_wire_transfer_get_profile(transfer) == NULL ||
        !g_str_equal(mux_clipboard_wire_transfer_get_profile(transfer),
                     link->profile)) {
        mux_clipboard_wire_transfer_free(transfer);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "clipboard transfer crossed profile boundary");
        return FALSE;
    }

    flags = mux_clipboard_wire_transfer_get_flags(transfer);
    snapshot = mux_clipboard_wire_transfer_get_snapshot(transfer);
    location = flags & MUX_CLIPBOARD_WIRE_FLAG_PRIMARY
                   ? MUX_OSC5522_LOCATION_PRIMARY
                   : MUX_OSC5522_LOCATION_CLIPBOARD;
    result = mux_kitty_clipboard_publish(link->kitty,
                                         location,
                                         snapshot,
                                         error);
    if (result && (flags & MUX_CLIPBOARD_WIRE_FLAG_HISTORY) &&
        link->observe_func != NULL)
        link->observe_func(link,
                           link->profile,
                           mux_clipboard_wire_transfer_get_source_origin(
                               transfer),
                           mux_clipboard_wire_transfer_get_source_view_id(
                               transfer),
                           flags,
                           snapshot,
                           link->user_data);
    if (result)
        result = send_ack(
            link,
            mux_clipboard_wire_transfer_get_transaction_id(transfer),
            error);
    mux_clipboard_wire_transfer_free(transfer);
    return result;
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
    guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT;

    g_return_val_if_fail(link != NULL, FALSE);
    g_return_val_if_fail(snapshot != NULL, FALSE);
    if (paste)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_PASTE;

    if (!mux_kitty_clipboard_publish(link->kitty,
                                     MUX_OSC5522_LOCATION_CLIPBOARD,
                                     snapshot,
                                     error))
        return FALSE;
    return send_snapshot(link,
                         flags,
                         source_origin,
                         source_view_id != 0
                             ? source_view_id
                             : link->view_id,
                         snapshot,
                         error);
}

guint
mux_clipboard_pane_link_tick(MuxClipboardPaneLink *link,
                             gint64 monotonic_us)
{
    g_autoptr(GError) error = NULL;
    guint expired;

    g_return_val_if_fail(link != NULL, 0);
    expired = mux_kitty_clipboard_tick(link->kitty, monotonic_us);
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
