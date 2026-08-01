#include "mux-pane-clipboard.h"

#include <gio/gio.h>

#define MUX_PANE_CLIPBOARD_RECONNECT_US (G_USEC_PER_SEC)

typedef struct {
    gchar *origin;
    guint64 view_id;
} SummaryMetadata;

struct _MuxPaneClipboard {
    gchar *profile;
    gchar *broker_profile;
    gboolean ephemeral;
    guint64 view_id;
    GMainContext *context;
    MuxClipboardPaneLink *link;
    MuxClipboardBrokerTransport *transport;
    MuxClipboardBrokerClient *client;
    MuxClipboardPickerBroker *picker;
    gboolean broker_ready;
    gboolean open_when_ready;
    gint64 reconnect_after_us;
    GHashTable *summary_metadata;
    gchar *selected_origin;
    guint64 selected_view_id;
    MuxClipboardSnapshot *pending_snapshot;
    gchar *pending_origin;
    guint64 pending_view_id;
    guint32 pending_flags;
    MuxPaneClipboardOutputFunc terminal_output_func;
    MuxPaneClipboardOutputFunc wire_output_func;
    MuxPaneClipboardNotifyFunc changed_func;
    MuxPaneClipboardNotifyFunc closed_func;
    MuxPaneClipboardFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

static gboolean connect_broker(MuxPaneClipboard *clipboard, GError **error);

static gchar *
broker_profile_name(const gchar *profile, gboolean ephemeral)
{
    const gchar *prefix = ephemeral ? "private:" : "profile:";
    gsize prefix_length = strlen(prefix);

    if (strlen(profile) <=
        MUX_CLIPBOARD_HISTORY_MAX_PROFILE - prefix_length)
        return g_strconcat(prefix, profile, NULL);

    g_autofree gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, profile, -1);
    return g_strconcat(prefix, digest, NULL);
}

static void
summary_metadata_free(gpointer data)
{
    SummaryMetadata *metadata = data;

    if (metadata == NULL)
        return;
    g_free(metadata->origin);
    g_free(metadata);
}

static void
report_failure(MuxPaneClipboard *clipboard,
               const gchar *operation,
               const GError *error)
{
    if (clipboard->failure_func != NULL)
        clipboard->failure_func(clipboard,
                                operation,
                                error,
                                clipboard->user_data);
}

static gboolean
link_terminal_output(MuxClipboardPaneLink *link,
                     GBytes *packet,
                     gpointer user_data,
                     GError **error)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) link;
    return clipboard->terminal_output_func(clipboard,
                                           packet,
                                           clipboard->user_data,
                                           error);
}

static gboolean
link_wire_output(MuxClipboardPaneLink *link,
                 GBytes *packet,
                 gpointer user_data,
                 GError **error)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) link;
    return clipboard->wire_output_func(clipboard,
                                       packet,
                                       clipboard->user_data,
                                       error);
}

static void
queue_observation(MuxPaneClipboard *clipboard,
                  const gchar *origin,
                  guint64 view_id,
                  guint32 flags,
                  const MuxClipboardSnapshot *snapshot)
{
    MuxClipboardSnapshot *copy =
        mux_clipboard_snapshot_dup_sealed(snapshot);

    if (copy == NULL)
        return;
    g_clear_pointer(&clipboard->pending_snapshot,
                    mux_clipboard_snapshot_unref);
    clipboard->pending_snapshot = copy;
    g_free(clipboard->pending_origin);
    clipboard->pending_origin = g_strdup(origin != NULL ? origin : "");
    clipboard->pending_view_id = view_id;
    clipboard->pending_flags = flags;
}

static void
link_observe(MuxClipboardPaneLink *link,
             const gchar *profile,
             const gchar *source_origin,
             guint64 source_view_id,
             guint32 flags,
             const MuxClipboardSnapshot *snapshot,
             gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) link;
    (void) profile;
    queue_observation(clipboard,
                      source_origin,
                      source_view_id,
                      flags,
                      snapshot);
}

static void
link_failure(MuxClipboardPaneLink *link,
             const gchar *operation,
             const GError *error,
             gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) link;
    report_failure(clipboard, operation, error);
}

static gboolean
picker_list(gpointer client, GError **error)
{
    MuxPaneClipboard *clipboard = client;

    if (!clipboard->broker_ready || clipboard->client == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "clipboard broker is not ready");
        return FALSE;
    }
    return mux_clipboard_broker_client_list(clipboard->client, error);
}

static gboolean
picker_select(gpointer client, guint64 entry_id, GError **error)
{
    MuxPaneClipboard *clipboard = client;
    SummaryMetadata *metadata = g_hash_table_lookup(
        clipboard->summary_metadata,
        &entry_id);

    if (!clipboard->broker_ready || clipboard->client == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "clipboard broker is not ready");
        return FALSE;
    }
    g_free(clipboard->selected_origin);
    clipboard->selected_origin = g_strdup(
        metadata != NULL ? metadata->origin : "history");
    clipboard->selected_view_id =
        metadata != NULL ? metadata->view_id : clipboard->view_id;
    return mux_clipboard_broker_client_select(clipboard->client,
                                              entry_id,
                                              TRUE,
                                              error);
}

static gboolean
picker_set_pinned(gpointer client,
                  guint64 entry_id,
                  gboolean pinned,
                  GError **error)
{
    MuxPaneClipboard *clipboard = client;

    if (!clipboard->broker_ready || clipboard->client == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "clipboard broker is not ready");
        return FALSE;
    }
    return mux_clipboard_broker_client_set_pinned(clipboard->client,
                                                  entry_id,
                                                  pinned,
                                                  error);
}

static gboolean
picker_delete(gpointer client, guint64 entry_id, GError **error)
{
    MuxPaneClipboard *clipboard = client;

    if (!clipboard->broker_ready || clipboard->client == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "clipboard broker is not ready");
        return FALSE;
    }
    return mux_clipboard_broker_client_delete(clipboard->client,
                                              entry_id,
                                              error);
}

static gboolean
picker_clear(gpointer client, GError **error)
{
    MuxPaneClipboard *clipboard = client;

    if (!clipboard->broker_ready || clipboard->client == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_CONNECTED,
                    "clipboard broker is not ready");
        return FALSE;
    }
    return mux_clipboard_broker_client_clear(clipboard->client,
                                             FALSE,
                                             error);
}

static void
disconnect_broker(MuxPaneClipboard *clipboard)
{
    clipboard->broker_ready = FALSE;
    clipboard->client = NULL;
    if (clipboard->transport != NULL) {
        mux_clipboard_broker_transport_close(clipboard->transport);
        mux_clipboard_broker_transport_unref(clipboard->transport);
        clipboard->transport = NULL;
    }
    clipboard->reconnect_after_us =
        g_get_monotonic_time() + MUX_PANE_CLIPBOARD_RECONNECT_US;
}

static void
picker_cancel(gpointer client)
{
    disconnect_broker(client);
}

static gboolean
apply_selection(gpointer snapshot, gpointer user_data, GError **error)
{
    MuxPaneClipboard *clipboard = user_data;

    return mux_clipboard_pane_link_apply_history(
        clipboard->link,
        snapshot,
        clipboard->selected_origin != NULL
            ? clipboard->selected_origin
            : "history",
        clipboard->selected_view_id,
        TRUE,
        error);
}

static void
picker_changed(MuxClipboardPickerBroker *picker, gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) picker;
    if (clipboard->changed_func != NULL)
        clipboard->changed_func(clipboard, clipboard->user_data);
}

static void
picker_closed(MuxClipboardPickerBroker *picker, gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) picker;
    g_clear_pointer(&clipboard->selected_origin, g_free);
    clipboard->selected_view_id = 0;
    if (clipboard->closed_func != NULL)
        clipboard->closed_func(clipboard, clipboard->user_data);
}

static void
broker_ready(MuxClipboardBrokerClient *client, gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    clipboard->client = client;
    clipboard->broker_ready = TRUE;
    clipboard->reconnect_after_us = 0;
    if (clipboard->open_when_ready) {
        clipboard->open_when_ready = FALSE;
        mux_clipboard_picker_broker_open(clipboard->picker);
    }
}

static void
broker_list(MuxClipboardBrokerClient *client,
            GPtrArray *summaries,
            gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;
    g_autoptr(GError) error = NULL;
    guint index;

    (void) client;
    g_hash_table_remove_all(clipboard->summary_metadata);
    for (index = 0; index < summaries->len; index++) {
        MuxClipboardControlSummary *summary =
            g_ptr_array_index(summaries, index);
        SummaryMetadata *metadata;
        guint64 *key;

        if (!mux_clipboard_picker_broker_add_summary(
                clipboard->picker,
                summary->entry_id,
                summary->created_us,
                summary->source_origin,
                summary->source_view_id,
                summary->pinned,
                (gsize) summary->total_bytes,
                summary->preview,
                NULL,
                0,
                &error))
            break;

        key = g_new(guint64, 1);
        *key = summary->entry_id;
        metadata = g_new0(SummaryMetadata, 1);
        metadata->origin = g_strdup(summary->source_origin);
        metadata->view_id = summary->source_view_id;
        g_hash_table_replace(clipboard->summary_metadata, key, metadata);
    }
    mux_clipboard_picker_broker_complete_list(clipboard->picker, error);
}

static void
broker_select(MuxClipboardBrokerClient *client,
              guint64 entry_id,
              const MuxClipboardSnapshot *snapshot,
              gboolean paste,
              gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) client;
    (void) entry_id;
    (void) paste;
    mux_clipboard_picker_broker_complete_selection(clipboard->picker,
                                                   (gpointer) snapshot,
                                                   NULL);
}

static void
broker_mutation(MuxClipboardBrokerClient *client,
                MuxClipboardControlType operation,
                guint64 result_value,
                gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) client;
    (void) operation;
    (void) result_value;
    mux_clipboard_picker_broker_complete_mutation(clipboard->picker, NULL);
}

static void
broker_failure(MuxClipboardBrokerClient *client,
               const gchar *operation,
               const GError *error,
               gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void) client;
    if (g_strcmp0(operation, "transport") == 0) {
        clipboard->broker_ready = FALSE;
        clipboard->client = NULL;
        clipboard->reconnect_after_us =
            g_get_monotonic_time() + MUX_PANE_CLIPBOARD_RECONNECT_US;
    }
    mux_clipboard_picker_broker_fail_active(clipboard->picker, error);
    report_failure(clipboard, operation, error);
}

static gboolean
connect_broker(MuxPaneClipboard *clipboard, GError **error)
{
    if (clipboard->transport != NULL &&
        mux_clipboard_broker_transport_is_open(clipboard->transport))
        return TRUE;
    if (clipboard->transport != NULL) {
        mux_clipboard_broker_transport_unref(clipboard->transport);
        clipboard->transport = NULL;
    }

    clipboard->client = NULL;
    clipboard->broker_ready = FALSE;
    clipboard->transport = mux_clipboard_broker_client_transport_connect(
        clipboard->broker_profile,
        clipboard->ephemeral
            ? MUX_CLIPBOARD_HISTORY_EPHEMERAL
            : MUX_CLIPBOARD_HISTORY_MEMORY,
        clipboard->context,
        broker_ready,
        broker_list,
        broker_select,
        broker_mutation,
        broker_failure,
        clipboard,
        NULL,
        error);
    if (clipboard->transport == NULL) {
        clipboard->reconnect_after_us =
            g_get_monotonic_time() + MUX_PANE_CLIPBOARD_RECONNECT_US;
        return FALSE;
    }
    clipboard->client =
        mux_clipboard_broker_client_transport_get_client(
            clipboard->transport);
    return TRUE;
}

static void
flush_observation(MuxPaneClipboard *clipboard)
{
    g_autoptr(GError) error = NULL;

    if (clipboard->pending_snapshot == NULL || !clipboard->broker_ready ||
        clipboard->client == NULL ||
        mux_clipboard_broker_client_request_pending(clipboard->client))
        return;

    if (!mux_clipboard_broker_client_observe(
            clipboard->client,
            clipboard->pending_flags,
            clipboard->pending_origin,
            clipboard->pending_view_id,
            clipboard->pending_snapshot,
            &error)) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING))
            report_failure(clipboard, "observe", error);
        return;
    }
    g_clear_pointer(&clipboard->pending_snapshot,
                    mux_clipboard_snapshot_unref);
    g_clear_pointer(&clipboard->pending_origin, g_free);
    clipboard->pending_view_id = 0;
    clipboard->pending_flags = 0;
}

MuxPaneClipboard *
mux_pane_clipboard_new(
    const gchar *profile,
    gboolean ephemeral,
    guint64 view_id,
    GMainContext *context,
    MuxPaneClipboardOutputFunc terminal_output_func,
    MuxPaneClipboardOutputFunc wire_output_func,
    MuxPaneClipboardNotifyFunc changed_func,
    MuxPaneClipboardNotifyFunc closed_func,
    MuxPaneClipboardFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error)
{
    static const MuxClipboardPickerBrokerOps picker_ops = {
        .list = picker_list,
        .select = picker_select,
        .set_pinned = picker_set_pinned,
        .delete_entry = picker_delete,
        .clear = picker_clear,
        .cancel = picker_cancel,
    };
    MuxPaneClipboard *clipboard;
    g_autoptr(GError) connect_error = NULL;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (profile == NULL || *profile == '\0' || context == NULL ||
        terminal_output_func == NULL || wire_output_func == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid pane clipboard construction");
        return NULL;
    }

    clipboard = g_new0(MuxPaneClipboard, 1);
    clipboard->profile = g_strdup(profile);
    clipboard->broker_profile = broker_profile_name(profile, ephemeral);
    clipboard->ephemeral = ephemeral;
    clipboard->view_id = view_id;
    clipboard->context = g_main_context_ref(context);
    clipboard->terminal_output_func = terminal_output_func;
    clipboard->wire_output_func = wire_output_func;
    clipboard->changed_func = changed_func;
    clipboard->closed_func = closed_func;
    clipboard->failure_func = failure_func;
    clipboard->user_data = user_data;
    clipboard->user_data_destroy = user_data_destroy;
    clipboard->summary_metadata = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        summary_metadata_free);
    clipboard->link = mux_clipboard_pane_link_new(
        profile,
        ephemeral,
        view_id,
        link_terminal_output,
        link_wire_output,
        link_observe,
        link_failure,
        clipboard,
        NULL);
    clipboard->picker = mux_clipboard_picker_broker_new(
        profile,
        clipboard,
        &picker_ops,
        NULL,
        apply_selection,
        clipboard,
        NULL,
        picker_changed,
        picker_closed,
        clipboard,
        NULL);
    if (clipboard->link == NULL || clipboard->picker == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to construct pane clipboard components");
        mux_pane_clipboard_free(clipboard);
        return NULL;
    }

    if (!connect_broker(clipboard, &connect_error))
        report_failure(clipboard, "broker-connect", connect_error);
    return clipboard;
}

void
mux_pane_clipboard_free(MuxPaneClipboard *clipboard)
{
    if (clipboard == NULL)
        return;

    if (clipboard->link != NULL)
        mux_clipboard_pane_link_set_enabled(clipboard->link, FALSE, NULL);
    g_clear_pointer(&clipboard->picker,
                    mux_clipboard_picker_broker_unref);
    disconnect_broker(clipboard);
    g_clear_pointer(&clipboard->link, mux_clipboard_pane_link_free);
    g_clear_pointer(&clipboard->summary_metadata, g_hash_table_unref);
    g_clear_pointer(&clipboard->pending_snapshot,
                    mux_clipboard_snapshot_unref);
    g_free(clipboard->pending_origin);
    g_free(clipboard->selected_origin);
    g_free(clipboard->broker_profile);
    g_free(clipboard->profile);
    g_main_context_unref(clipboard->context);
    if (clipboard->user_data_destroy != NULL)
        clipboard->user_data_destroy(clipboard->user_data);
    g_free(clipboard);
}

gboolean
mux_pane_clipboard_set_enabled(MuxPaneClipboard *clipboard,
                               gboolean enabled,
                               GError **error)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_pane_link_set_enabled(clipboard->link,
                                              enabled,
                                              error);
}

gboolean
mux_pane_clipboard_handle_support(MuxPaneClipboard *clipboard,
                                  const guint8 *sequence,
                                  gsize length,
                                  GError **error)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_pane_link_handle_support(clipboard->link,
                                                 sequence,
                                                 length,
                                                 error);
}

gboolean
mux_pane_clipboard_handle_osc(MuxPaneClipboard *clipboard,
                              const guint8 *sequence,
                              gsize length,
                              GError **error)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_pane_link_handle_osc(clipboard->link,
                                             sequence,
                                             length,
                                             error);
}

gboolean
mux_pane_clipboard_handle_engine_packet(MuxPaneClipboard *clipboard,
                                        const guint8 *packet,
                                        gsize packet_length,
                                        GError **error)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_pane_link_handle_packet(clipboard->link,
                                                packet,
                                                packet_length,
                                                error);
}

void
mux_pane_clipboard_open_picker(MuxPaneClipboard *clipboard)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(clipboard != NULL);
    if (mux_pane_clipboard_picker_is_open(clipboard))
        return;
    if (!clipboard->broker_ready) {
        clipboard->open_when_ready = TRUE;
        if (!connect_broker(clipboard, &error))
            report_failure(clipboard, "broker-connect", error);
        return;
    }
    mux_clipboard_picker_broker_open(clipboard->picker);
}

void
mux_pane_clipboard_close_picker(MuxPaneClipboard *clipboard)
{
    g_return_if_fail(clipboard != NULL);
    clipboard->open_when_ready = FALSE;
    mux_clipboard_picker_broker_close(clipboard->picker);
}

gboolean
mux_pane_clipboard_picker_is_open(const MuxPaneClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_picker_broker_get_state(clipboard->picker) !=
           MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED;
}

gboolean
mux_pane_clipboard_handle_picker_key(MuxPaneClipboard *clipboard,
                                     MuxClipboardPickerKey key,
                                     gunichar text)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return mux_clipboard_picker_broker_handle_key(clipboard->picker,
                                                 key,
                                                 text);
}

gchar *
mux_pane_clipboard_render_picker(MuxPaneClipboard *clipboard,
                                 guint terminal_columns,
                                 guint terminal_rows)
{
    g_return_val_if_fail(clipboard != NULL, NULL);
    return mux_clipboard_picker_broker_render(clipboard->picker,
                                             terminal_columns,
                                             terminal_rows);
}

void
mux_pane_clipboard_tick(MuxPaneClipboard *clipboard, gint64 monotonic_us)
{
    g_autoptr(GError) error = NULL;

    g_return_if_fail(clipboard != NULL);
    mux_clipboard_pane_link_tick(clipboard->link, monotonic_us);
    if (clipboard->transport != NULL &&
        !mux_clipboard_broker_transport_is_open(clipboard->transport)) {
        mux_clipboard_broker_transport_unref(clipboard->transport);
        clipboard->transport = NULL;
        clipboard->client = NULL;
        clipboard->broker_ready = FALSE;
    }
    if (clipboard->transport == NULL &&
        monotonic_us >= clipboard->reconnect_after_us &&
        !connect_broker(clipboard, &error)) {
        clipboard->reconnect_after_us =
            monotonic_us + MUX_PANE_CLIPBOARD_RECONNECT_US;
    }
    flush_observation(clipboard);
}
