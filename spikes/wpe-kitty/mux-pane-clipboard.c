#include "mux-pane-clipboard.h"
#include "mux-clipboard-lifetime.h"

#include <gio/gio.h>

#define MUX_PANE_CLIPBOARD_RECONNECT_US (G_USEC_PER_SEC)

typedef enum {
    FRESH_PASTE_IDLE,
    FRESH_PASTE_STARTING,
    FRESH_PASTE_READING,
    FRESH_PASTE_SYNCING
} FreshPasteState;

typedef enum {
    FRESH_DISPATCH_STARTED,
    FRESH_DISPATCH_RETRY,
    FRESH_DISPATCH_FAILED
} FreshDispatchResult;

typedef struct {
    guint64 request_id;
    MuxPaneClipboardFreshPasteFunc callback;
    gpointer callback_data;
} FreshPasteWaiter;

typedef struct {
    gchar *origin;
    guint64 view_id;
} SummaryMetadata;

typedef struct {
    MuxClipboardSnapshot *snapshot;
    gchar *origin;
    guint64 view_id;
    guint32 flags;
    gsize bytes;
    guint64 fresh_request_id;
    guint64 transaction_id;
    gboolean submitted;
} PendingObservation;

struct _MuxPaneClipboard {
    MuxClipboardLifetime lifetime;
    gchar *profile;
    gchar *broker_profile;
    gboolean ephemeral;
    guint64 view_id;
    GMainContext *context;
    MuxClipboardPaneLink *link;
    MuxKittyClipboard *fresh_kitty;
    MuxClipboardBrokerTransport *transport;
    MuxClipboardBrokerClient *client;
    MuxClipboardPickerBroker *picker;
    gboolean broker_ready;
    gboolean open_when_ready;
    gint64 reconnect_after_us;
    GHashTable *summary_metadata;
    gchar *selected_origin;
    guint64 selected_view_id;
    GQueue observations;
    gsize observation_bytes;
    FreshPasteState fresh_state;
    guint64 fresh_request_id;
    gint64 fresh_dispatch_deadline_us;
    gint64 fresh_read_deadline_us;
    gint64 fresh_engine_deadline_us;
    FreshPasteWaiter fresh_waiters[
        MUX_PANE_CLIPBOARD_MAX_PENDING_PASTES];
    guint fresh_waiter_count;
    MuxClipboardSnapshot *fresh_snapshot;
    guint64 next_fresh_wire_transaction;
    guint64 fresh_wire_transaction;
    gboolean fresh_wire_acked;
    gboolean fresh_broker_submitted;
    gboolean fresh_broker_acked;
    GError *fresh_error;
    MuxPaneClipboardOutputFunc terminal_output_func;
    MuxPaneClipboardOutputFunc wire_output_func;
    MuxPaneClipboardNotifyFunc changed_func;
    MuxPaneClipboardNotifyFunc closed_func;
    MuxPaneClipboardFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
};

static void pane_clipboard_destroy(MuxPaneClipboard *clipboard);

static MuxPaneClipboard *
pane_clipboard_acquire(MuxPaneClipboard *clipboard)
{
    mux_clipboard_lifetime_acquire(&clipboard->lifetime);
    return clipboard;
}

static void
pane_clipboard_release(MuxPaneClipboard *clipboard)
{
    if (mux_clipboard_lifetime_release(&clipboard->lifetime))
        pane_clipboard_destroy(clipboard);
}

typedef MuxPaneClipboard MuxPaneClipboardOperation;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxPaneClipboardOperation,
                              pane_clipboard_release)

static void
pending_observation_free(PendingObservation *observation)
{
    if (observation == NULL)
        return;
    mux_clipboard_snapshot_unref(observation->snapshot);
    g_free(observation->origin);
    g_free(observation);
}

static void
reset_submitted_observation(MuxPaneClipboard *clipboard)
{
    PendingObservation *observation =
        g_queue_peek_head(&clipboard->observations);

    if (observation != NULL) {
        observation->submitted = FALSE;
        observation->transaction_id = 0;
    }
}

static gboolean queue_observation(MuxPaneClipboard *clipboard,
                                  const gchar *origin,
                                  guint64 view_id,
                                  guint32 flags,
                                  const MuxClipboardSnapshot *snapshot,
                                  guint64 fresh_request_id,
                                  GError **error);
static void broker_observation_result(
    MuxClipboardBrokerClient *client,
    guint64 transaction_id,
    MuxClipboardBrokerObservationResult result,
    const GError *error,
    gpointer user_data);
static gboolean connect_broker(MuxPaneClipboard *clipboard, GError **error);
static void flush_fresh_observation(MuxPaneClipboard *clipboard);

static guint32
fresh_snapshot_flags(const MuxPaneClipboard *clipboard)
{
    guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT |
                    MUX_CLIPBOARD_WIRE_FLAG_HISTORY;

    if (clipboard->ephemeral)
        flags |= MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL;
    return flags;
}

static guint64
next_fresh_wire_transaction(MuxPaneClipboard *clipboard)
{
    clipboard->next_fresh_wire_transaction++;
    if (clipboard->next_fresh_wire_transaction == 0)
        clipboard->next_fresh_wire_transaction++;
    return clipboard->next_fresh_wire_transaction;
}

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

static void
clear_fresh_paste(MuxPaneClipboard *clipboard)
{
    if (clipboard->fresh_kitty != NULL)
        mux_kitty_clipboard_cancel_read(clipboard->fresh_kitty);
    clipboard->fresh_state = FRESH_PASTE_IDLE;
    clipboard->fresh_request_id = 0;
    clipboard->fresh_dispatch_deadline_us = 0;
    clipboard->fresh_read_deadline_us = 0;
    clipboard->fresh_engine_deadline_us = 0;
    for (guint i = 0; i < clipboard->fresh_waiter_count; i++)
        clipboard->fresh_waiters[i] = (FreshPasteWaiter) { 0 };
    clipboard->fresh_waiter_count = 0;
    g_clear_pointer(&clipboard->fresh_snapshot,
                    mux_clipboard_snapshot_unref);
    clipboard->fresh_wire_transaction = 0;
    clipboard->fresh_wire_acked = FALSE;
    clipboard->fresh_broker_submitted = FALSE;
    clipboard->fresh_broker_acked = FALSE;
    g_clear_error(&clipboard->fresh_error);
}

static void
finish_fresh_paste(MuxPaneClipboard *clipboard, gboolean fresh)
{
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);
    FreshPasteWaiter waiters[MUX_PANE_CLIPBOARD_MAX_PENDING_PASTES] = {
        0
    };
    guint waiter_count = clipboard->fresh_waiter_count;

    for (guint i = 0; i < waiter_count; i++)
        waiters[i] = clipboard->fresh_waiters[i];

    clear_fresh_paste(clipboard);
    for (guint i = 0; i < waiter_count; i++) {
        if (waiters[i].callback != NULL)
            waiters[i].callback(clipboard,
                                waiters[i].request_id,
                                fresh,
                                waiters[i].callback_data);
    }
}

static void
fail_fresh_paste(MuxPaneClipboard *clipboard,
                 const gchar *operation,
                 const GError *error)
{
    if (clipboard->fresh_state == FRESH_PASTE_IDLE ||
        clipboard->fresh_error != NULL)
        return;
    clipboard->fresh_error = error != NULL
        ? g_error_copy(error)
        : g_error_new_literal(G_IO_ERROR,
                              G_IO_ERROR_FAILED,
                              "fresh clipboard paste failed");
    report_failure(clipboard, operation, clipboard->fresh_error);
}

static void
complete_failed_fresh_paste(MuxPaneClipboard *clipboard)
{
    if (clipboard->fresh_error != NULL)
        finish_fresh_paste(clipboard, FALSE);
}

static void
maybe_complete_fresh_paste(MuxPaneClipboard *clipboard)
{
    if (clipboard->fresh_state == FRESH_PASTE_SYNCING &&
        clipboard->fresh_wire_acked)
        finish_fresh_paste(clipboard, TRUE);
}

static gboolean
fresh_terminal_output(MuxKittyClipboard *kitty,
                      GBytes *packet,
                      gpointer user_data,
                      GError **error)
{
    MuxPaneClipboard *clipboard = user_data;

    (void)kitty;
    return clipboard->terminal_output_func(clipboard,
                                           packet,
                                           clipboard->user_data,
                                           error);
}

static gboolean
fresh_wire_output(GBytes *packet, gpointer user_data, GError **error)
{
    MuxPaneClipboard *clipboard = user_data;

    return clipboard->wire_output_func(clipboard,
                                       packet,
                                       clipboard->user_data,
                                       error);
}

static void
fresh_receive(MuxKittyClipboard *kitty,
              MuxOsc5522Location location,
              MuxClipboardSnapshot *snapshot,
              gboolean is_paste,
              gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);
    g_autoptr(GError) error = NULL;

    (void)kitty;
    (void)is_paste;
    if (clipboard->fresh_state != FRESH_PASTE_READING ||
        location != MUX_OSC5522_LOCATION_CLIPBOARD)
        return;

    clipboard->fresh_snapshot =
        mux_clipboard_snapshot_dup_sealed(snapshot);
    if (clipboard->fresh_snapshot == NULL) {
        error = g_error_new_literal(G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "fresh clipboard snapshot is invalid");
        fail_fresh_paste(clipboard, "fresh-paste", error);
        return;
    }

    clipboard->fresh_state = FRESH_PASTE_SYNCING;
    clipboard->fresh_read_deadline_us = 0;
    clipboard->fresh_engine_deadline_us = g_get_monotonic_time() +
        (gint64)MUX_PANE_CLIPBOARD_FRESH_ENGINE_TIMEOUT_MS * 1000;
    clipboard->fresh_wire_transaction =
        next_fresh_wire_transaction(clipboard);
    flush_fresh_observation(clipboard);
    if (!mux_clipboard_wire_send_snapshot(
            clipboard->fresh_wire_transaction,
            fresh_snapshot_flags(clipboard),
            clipboard->profile,
            "external",
            clipboard->view_id,
            g_get_monotonic_time(),
            clipboard->fresh_snapshot,
            fresh_wire_output,
            clipboard,
            &error))
        fail_fresh_paste(clipboard, "fresh-paste-engine", error);
    g_clear_pointer(&clipboard->fresh_snapshot,
                    mux_clipboard_snapshot_unref);
}

static void
fresh_failure(MuxKittyClipboard *kitty,
              const gchar *operation,
              const GError *error,
              gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;

    (void)kitty;
    fail_fresh_paste(clipboard,
                     operation != NULL ? operation : "fresh-paste-read",
                     error);
}

static FreshDispatchResult
start_fresh_read(MuxPaneClipboard *clipboard, GError **error)
{
    gint64 dispatch_deadline_us = clipboard->fresh_dispatch_deadline_us;

    clipboard->fresh_state = FRESH_PASTE_READING;
    clipboard->fresh_dispatch_deadline_us = 0;
    clipboard->fresh_read_deadline_us = g_get_monotonic_time() +
        (gint64)MUX_PANE_CLIPBOARD_FRESH_READ_TIMEOUT_MS * 1000;
    if (mux_kitty_clipboard_request_all(clipboard->fresh_kitty,
                                        MUX_OSC5522_LOCATION_CLIPBOARD,
                                        NULL,
                                        "Mux fresh paste",
                                        FALSE,
                                        error))
        return FRESH_DISPATCH_STARTED;
    if (clipboard->fresh_state != FRESH_PASTE_READING)
        return FRESH_DISPATCH_STARTED;
    if (error != NULL && *error != NULL &&
        g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
        g_clear_error(error);
        clipboard->fresh_state = FRESH_PASTE_STARTING;
        clipboard->fresh_dispatch_deadline_us = dispatch_deadline_us;
        clipboard->fresh_read_deadline_us = 0;
        return FRESH_DISPATCH_RETRY;
    }
    clipboard->fresh_state = FRESH_PASTE_STARTING;
    clipboard->fresh_dispatch_deadline_us = dispatch_deadline_us;
    clipboard->fresh_read_deadline_us = 0;
    return FRESH_DISPATCH_FAILED;
}

static void
flush_fresh_observation(MuxPaneClipboard *clipboard)
{
    g_autoptr(GError) error = NULL;

    if (clipboard->fresh_state != FRESH_PASTE_SYNCING ||
        clipboard->fresh_snapshot == NULL ||
        clipboard->fresh_broker_submitted)
        return;

    if (!queue_observation(clipboard,
                           "external",
                           clipboard->view_id,
                           fresh_snapshot_flags(clipboard),
                           clipboard->fresh_snapshot,
                           0,
                           &error)) {
        report_failure(clipboard, "fresh-paste-history", error);
    }
    clipboard->fresh_broker_submitted = TRUE;
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

static gboolean
queue_observation(MuxPaneClipboard *clipboard,
                  const gchar *origin,
                  guint64 view_id,
                  guint32 flags,
                  const MuxClipboardSnapshot *snapshot,
                  guint64 fresh_request_id,
                  GError **error)
{
    PendingObservation *observation;
    gsize bytes = mux_clipboard_snapshot_get_total_bytes(snapshot);

    if (clipboard->observations.length >=
            MUX_PANE_CLIPBOARD_MAX_PENDING_OBSERVATIONS ||
        bytes > MUX_PANE_CLIPBOARD_MAX_OBSERVATION_BYTES -
                    clipboard->observation_bytes) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "clipboard observation queue is full");
        return FALSE;
    }

    observation = g_new0(PendingObservation, 1);
    observation->snapshot = mux_clipboard_snapshot_dup_sealed(snapshot);
    if (observation->snapshot == NULL) {
        pending_observation_free(observation);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "clipboard observation is invalid");
        return FALSE;
    }
    observation->origin = g_strdup(origin != NULL ? origin : "");
    observation->view_id = view_id;
    observation->flags = flags;
    observation->bytes = bytes;
    observation->fresh_request_id = fresh_request_id;
    g_queue_push_tail(&clipboard->observations, observation);
    clipboard->observation_bytes += bytes;
    return TRUE;
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
    g_autoptr(GError) error = NULL;

    (void) link;
    (void) profile;
    if (!queue_observation(clipboard,
                           source_origin,
                           source_view_id,
                           flags,
                           snapshot,
                           0,
                           &error))
        report_failure(clipboard, "observe-queue", error);
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
    reset_submitted_observation(clipboard);
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
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);

    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);

    (void)operation;
    (void) picker;
    if (clipboard->changed_func != NULL)
        clipboard->changed_func(clipboard, clipboard->user_data);
}

static void
picker_closed(MuxClipboardPickerBroker *picker, gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);

    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);

    (void)operation;
    clipboard->client = client;
    mux_clipboard_broker_client_set_observation_func(
        client,
        broker_observation_result,
        clipboard);
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
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);
    g_autoptr(GError) error = NULL;
    guint index;

    (void)operation;
    (void) client;
    g_hash_table_remove_all(clipboard->summary_metadata);
    for (index = 0; index < summaries->len; index++) {
        MuxClipboardControlSummary *summary =
            g_ptr_array_index(summaries, index);
        SummaryMetadata *metadata;
        guint64 *key;

        if (!mux_clipboard_picker_broker_add_summary_full(
                clipboard->picker,
                summary->entry_id,
                summary->created_us,
                summary->source_origin,
                summary->source_view_id,
                summary->pinned,
                (gsize) summary->total_bytes,
                summary->preview,
                (const gchar *const *)summary->mime_types,
                summary->mime_type_count,
                summary->format_count,
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
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);

    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) lifetime_operation =
        pane_clipboard_acquire(clipboard);

    (void)lifetime_operation;
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
    g_autoptr(MuxPaneClipboardOperation) lifetime_operation =
        pane_clipboard_acquire(clipboard);

    (void)lifetime_operation;
    (void) client;
    if (g_strcmp0(operation, "transport") == 0) {
        clipboard->broker_ready = FALSE;
        clipboard->client = NULL;
        clipboard->reconnect_after_us =
            g_get_monotonic_time() + MUX_PANE_CLIPBOARD_RECONNECT_US;
        reset_submitted_observation(clipboard);
    }
    mux_clipboard_picker_broker_fail_active(clipboard->picker, error);
    report_failure(clipboard, operation, error);
}

static void
broker_observation_result(
    MuxClipboardBrokerClient *client,
    guint64 transaction_id,
    MuxClipboardBrokerObservationResult result,
    const GError *error,
    gpointer user_data)
{
    MuxPaneClipboard *clipboard = user_data;
    g_autoptr(MuxPaneClipboardOperation) operation =
        pane_clipboard_acquire(clipboard);
    PendingObservation *observation =
        g_queue_peek_head(&clipboard->observations);
    gboolean fresh;

    (void)operation;
    (void)client;
    if (observation == NULL || !observation->submitted ||
        observation->transaction_id != transaction_id) {
        g_autoptr(GError) protocol_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "clipboard observation result arrived out of order");

        report_failure(clipboard, "observe-result", protocol_error);
        return;
    }

    g_queue_pop_head(&clipboard->observations);
    clipboard->observation_bytes -= observation->bytes;
    fresh = observation->fresh_request_id != 0 &&
            observation->fresh_request_id == clipboard->fresh_request_id;
    pending_observation_free(observation);

    if (result == MUX_CLIPBOARD_BROKER_OBSERVATION_REJECTED) {
        if (fresh) {
            fail_fresh_paste(clipboard, "fresh-paste-broker", error);
            complete_failed_fresh_paste(clipboard);
        } else {
            report_failure(clipboard, "observe", error);
        }
        return;
    }
    if (result == MUX_CLIPBOARD_BROKER_OBSERVATION_DEGRADED) {
        g_autoptr(GError) degraded = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_PARTIAL_INPUT,
            "clipboard history stored only eligible fallback formats");

        if (fresh) {
            report_failure(clipboard, "fresh-paste-history", degraded);
            if (!mux_clipboard_lifetime_owner_released(
                    &clipboard->lifetime)) {
                clipboard->fresh_broker_acked = TRUE;
                maybe_complete_fresh_paste(clipboard);
            }
        } else {
            report_failure(clipboard, "observe-degraded", degraded);
        }
        return;
    }
    if (fresh) {
        clipboard->fresh_broker_acked = TRUE;
        maybe_complete_fresh_paste(clipboard);
    }
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
    mux_clipboard_broker_client_set_observation_func(
        clipboard->client,
        broker_observation_result,
        clipboard);
    return TRUE;
}

static void
flush_observation(MuxPaneClipboard *clipboard)
{
    g_autoptr(GError) error = NULL;
    PendingObservation *observation =
        g_queue_peek_head(&clipboard->observations);
    guint64 transaction_id = 0;

    if (observation == NULL || observation->submitted ||
        !clipboard->broker_ready || clipboard->client == NULL)
        return;

    observation->submitted = TRUE;
    if (!mux_clipboard_broker_client_observe_full(
            clipboard->client,
            observation->flags,
            observation->origin,
            observation->view_id,
            observation->snapshot,
            &observation->transaction_id,
            &error)) {
        transaction_id = observation->transaction_id;
        observation = g_queue_peek_head(&clipboard->observations);
        if (observation != NULL &&
            observation->transaction_id == transaction_id) {
            observation->submitted = FALSE;
            observation->transaction_id = 0;
        }
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            report_failure(clipboard, "observe", error);
    }
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
    gboolean owner_released;

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
    mux_clipboard_lifetime_init(&clipboard->lifetime);
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
    g_queue_init(&clipboard->observations);
    clipboard->next_fresh_wire_transaction =
        ((guint64)g_random_int() << 32) | g_random_int();
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
    clipboard->fresh_kitty = mux_kitty_clipboard_new(
        fresh_terminal_output,
        fresh_receive,
        fresh_failure,
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
    if (clipboard->link == NULL || clipboard->fresh_kitty == NULL ||
        clipboard->picker == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to construct pane clipboard components");
        mux_pane_clipboard_free(clipboard);
        return NULL;
    }

    pane_clipboard_acquire(clipboard);
    if (!connect_broker(clipboard, &connect_error))
        report_failure(clipboard, "broker-connect", connect_error);
    owner_released = mux_clipboard_lifetime_owner_released(
        &clipboard->lifetime);
    if (owner_released && error != NULL && *error == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "pane clipboard was destroyed during construction");
    }
    pane_clipboard_release(clipboard);
    if (owner_released)
        return NULL;
    return clipboard;
}

static void
pane_clipboard_destroy(MuxPaneClipboard *clipboard)
{
    if (clipboard->link != NULL)
        mux_clipboard_pane_link_set_enabled(clipboard->link, FALSE, NULL);
    clear_fresh_paste(clipboard);
    g_clear_pointer(&clipboard->fresh_kitty,
                    mux_kitty_clipboard_unref);
    g_clear_pointer(&clipboard->picker,
                    mux_clipboard_picker_broker_unref);
    disconnect_broker(clipboard);
    g_clear_pointer(&clipboard->link, mux_clipboard_pane_link_free);
    g_clear_pointer(&clipboard->summary_metadata, g_hash_table_unref);
    g_queue_clear_full(&clipboard->observations,
                       (GDestroyNotify)pending_observation_free);
    g_free(clipboard->selected_origin);
    g_free(clipboard->broker_profile);
    g_free(clipboard->profile);
    g_main_context_unref(clipboard->context);
    if (clipboard->user_data_destroy != NULL)
        clipboard->user_data_destroy(clipboard->user_data);
    g_free(clipboard);
}

void
mux_pane_clipboard_free(MuxPaneClipboard *clipboard)
{
    if (clipboard != NULL &&
        mux_clipboard_lifetime_release_owner(&clipboard->lifetime))
        pane_clipboard_destroy(clipboard);
}

gboolean
mux_pane_clipboard_set_enabled(MuxPaneClipboard *clipboard,
                               gboolean enabled,
                               GError **error)
{
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;
    g_autoptr(GError) probe_error = NULL;
    gboolean matches_fresh_read = FALSE;
    gboolean result;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
    if (clipboard->fresh_state != FRESH_PASTE_IDLE) {
        gboolean probe_result = mux_kitty_clipboard_osc_matches_pending_read(
            clipboard->fresh_kitty,
            sequence,
            length,
            &matches_fresh_read,
            &probe_error);

        if (!probe_result) {
            fail_fresh_paste(clipboard,
                             "fresh-paste-response",
                             probe_error);
            if (error != NULL && *error == NULL && probe_error != NULL)
                g_propagate_error(error, g_steal_pointer(&probe_error));
            complete_failed_fresh_paste(clipboard);
            return FALSE;
        }
    }
    if (matches_fresh_read) {
        if (clipboard->fresh_state == FRESH_PASTE_READING)
            clipboard->fresh_read_deadline_us = g_get_monotonic_time() +
                (gint64)MUX_PANE_CLIPBOARD_FRESH_READ_TIMEOUT_MS * 1000;
        if (!mux_kitty_clipboard_handle_osc(clipboard->fresh_kitty,
                                            sequence,
                                            length,
                                            error)) {
            fail_fresh_paste(clipboard, "fresh-paste-response",
                             error != NULL ? *error : NULL);
            complete_failed_fresh_paste(clipboard);
            return FALSE;
        }
        complete_failed_fresh_paste(clipboard);
        return TRUE;
    }
    result = mux_clipboard_pane_link_handle_osc(clipboard->link,
                                               sequence,
                                               length,
                                               error);
    complete_failed_fresh_paste(clipboard);
    return result;
}

gboolean
mux_pane_clipboard_handle_engine_packet(MuxPaneClipboard *clipboard,
                                        const guint8 *packet,
                                        gsize packet_length,
                                        GError **error)
{
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;
    MuxClipboardWireRecord record = { 0 };
    g_autoptr(GError) probe_error = NULL;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
    if (clipboard->fresh_state == FRESH_PASTE_SYNCING &&
        !mux_clipboard_wire_record_decode(packet,
                                          packet_length,
                                          &record,
                                          &probe_error)) {
        fail_fresh_paste(clipboard,
                         "fresh-paste-engine-response",
                         probe_error);
        if (error != NULL && *error == NULL && probe_error != NULL)
            g_propagate_error(error, g_steal_pointer(&probe_error));
        complete_failed_fresh_paste(clipboard);
        return FALSE;
    }
    if (clipboard->fresh_state == FRESH_PASTE_SYNCING &&
        record.transaction_id == clipboard->fresh_wire_transaction &&
        (record.type == MUX_CLIPBOARD_WIRE_ACK ||
         record.type == MUX_CLIPBOARD_WIRE_REMOTE_ERROR)) {
        if (record.type == MUX_CLIPBOARD_WIRE_ACK) {
            clipboard->fresh_wire_acked = TRUE;
            maybe_complete_fresh_paste(clipboard);
        } else {
            g_autoptr(GError) error = g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "engine rejected fresh clipboard snapshot");

            fail_fresh_paste(clipboard, "fresh-paste-engine", error);
            complete_failed_fresh_paste(clipboard);
        }
        mux_clipboard_wire_record_clear(&record);
        return TRUE;
    }
    mux_clipboard_wire_record_clear(&record);
    return mux_clipboard_pane_link_handle_packet(clipboard->link,
                                                packet,
                                                packet_length,
                                                error);
}

gboolean
mux_pane_clipboard_request_fresh_paste(
    MuxPaneClipboard *clipboard,
    guint64 request_id,
    MuxPaneClipboardFreshPasteFunc callback,
    gpointer callback_data,
    GError **error)
{
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;
    FreshDispatchResult dispatch_result = FRESH_DISPATCH_RETRY;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
    if (request_id == 0 || callback == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "fresh paste request requires an id and callback");
        return FALSE;
    }
    for (guint i = 0; i < clipboard->fresh_waiter_count; i++) {
        if (clipboard->fresh_waiters[i].request_id == request_id) {
            if (clipboard->fresh_waiters[i].callback == callback &&
                clipboard->fresh_waiters[i].callback_data == callback_data)
                return TRUE;
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "fresh paste request id is already in use");
            return FALSE;
        }
    }
    if (clipboard->fresh_waiter_count >=
        MUX_PANE_CLIPBOARD_MAX_PENDING_PASTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "fresh paste request queue is full");
        return FALSE;
    }

    clipboard->fresh_waiters[clipboard->fresh_waiter_count++] =
        (FreshPasteWaiter) {
            .request_id = request_id,
            .callback = callback,
            .callback_data = callback_data,
        };
    if (clipboard->fresh_state != FRESH_PASTE_IDLE)
        return TRUE;

    clipboard->fresh_state = FRESH_PASTE_STARTING;
    clipboard->fresh_request_id = request_id;
    clipboard->fresh_dispatch_deadline_us = g_get_monotonic_time() +
        (gint64)MUX_PANE_CLIPBOARD_FRESH_DISPATCH_TIMEOUT_MS * 1000;
    if (!mux_clipboard_pane_link_write_pending(clipboard->link))
        dispatch_result = start_fresh_read(clipboard, error);
    if (dispatch_result == FRESH_DISPATCH_FAILED) {
        clear_fresh_paste(clipboard);
        return FALSE;
    }
    return TRUE;
}

gboolean
mux_pane_clipboard_fresh_paste_pending(
    const MuxPaneClipboard *clipboard)
{
    g_return_val_if_fail(clipboard != NULL, FALSE);
    return clipboard->fresh_state != FRESH_PASTE_IDLE;
}

void
mux_pane_clipboard_open_picker(MuxPaneClipboard *clipboard)
{
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(clipboard != NULL);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;

    g_return_if_fail(clipboard != NULL);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;

    g_return_val_if_fail(clipboard != NULL, FALSE);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
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
    g_autoptr(MuxPaneClipboardOperation) operation = NULL;
    g_autoptr(GError) error = NULL;
    FreshDispatchResult dispatch_result = FRESH_DISPATCH_RETRY;
    const gchar *timeout_operation = NULL;
    const gchar *timeout_message = NULL;

    g_return_if_fail(clipboard != NULL);
    operation = pane_clipboard_acquire(clipboard);
    (void)operation;
    mux_clipboard_pane_link_tick(clipboard->link, monotonic_us);
    if (clipboard->fresh_state == FRESH_PASTE_STARTING &&
        !mux_clipboard_pane_link_write_pending(clipboard->link)) {
        dispatch_result = start_fresh_read(clipboard, &error);
        if (dispatch_result == FRESH_DISPATCH_FAILED) {
            fail_fresh_paste(clipboard, "fresh-paste-dispatch", error);
            g_clear_error(&error);
        }
    }
    mux_kitty_clipboard_tick(clipboard->fresh_kitty, monotonic_us);
    if (clipboard->transport != NULL &&
        !mux_clipboard_broker_transport_is_open(clipboard->transport)) {
        mux_clipboard_broker_transport_unref(clipboard->transport);
        clipboard->transport = NULL;
        clipboard->client = NULL;
        clipboard->broker_ready = FALSE;
        reset_submitted_observation(clipboard);
    }
    if (clipboard->transport == NULL &&
        monotonic_us >= clipboard->reconnect_after_us &&
        !connect_broker(clipboard, &error)) {
        clipboard->reconnect_after_us =
            monotonic_us + MUX_PANE_CLIPBOARD_RECONNECT_US;
        g_clear_error(&error);
    }
    flush_fresh_observation(clipboard);
    flush_observation(clipboard);

    if (clipboard->fresh_state == FRESH_PASTE_STARTING &&
        clipboard->fresh_dispatch_deadline_us != 0 &&
        monotonic_us >= clipboard->fresh_dispatch_deadline_us) {
        timeout_operation = "fresh-paste-dispatch-timeout";
        timeout_message = "fresh clipboard dispatch timed out";
    } else if (clipboard->fresh_state == FRESH_PASTE_READING &&
               clipboard->fresh_read_deadline_us != 0 &&
               monotonic_us >= clipboard->fresh_read_deadline_us) {
        timeout_operation = "fresh-paste-read-timeout";
        timeout_message = "fresh clipboard read timed out";
    } else if (clipboard->fresh_state == FRESH_PASTE_SYNCING &&
               clipboard->fresh_engine_deadline_us != 0 &&
               monotonic_us >= clipboard->fresh_engine_deadline_us) {
        timeout_operation = "fresh-paste-engine-timeout";
        timeout_message = "fresh clipboard engine delivery timed out";
    }
    if (timeout_operation != NULL) {
        error = g_error_new_literal(G_IO_ERROR,
                                    G_IO_ERROR_TIMED_OUT,
                                    timeout_message);
        fail_fresh_paste(clipboard, timeout_operation, error);
        g_clear_error(&error);
    }
    complete_failed_fresh_paste(clipboard);
}
