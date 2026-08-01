#ifndef MUX_CLIPBOARD_BROKER_CLIENT_H
#define MUX_CLIPBOARD_BROKER_CLIENT_H

#include "mux-clipboard-control.h"
#include "mux-clipboard-history.h"
#include "mux-clipboard-wire.h"
#include "mux-extension-protocol.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_BROKER_CLIENT_TIMEOUT_MS 10000U
#define MUX_CLIPBOARD_BROKER_CLIENT_MAX_OBSERVATIONS 25U

typedef struct _MuxClipboardBrokerClient MuxClipboardBrokerClient;

typedef enum {
    MUX_CLIPBOARD_BROKER_CLIENT_NEW,
    MUX_CLIPBOARD_BROKER_CLIENT_HELLO_PENDING,
    MUX_CLIPBOARD_BROKER_CLIENT_READY,
    MUX_CLIPBOARD_BROKER_CLIENT_CLOSED
} MuxClipboardBrokerClientState;

typedef enum {
    MUX_CLIPBOARD_BROKER_OBSERVATION_ACCEPTED,
    MUX_CLIPBOARD_BROKER_OBSERVATION_DEGRADED,
    MUX_CLIPBOARD_BROKER_OBSERVATION_REJECTED
} MuxClipboardBrokerObservationResult;

/* packet is a borrowed complete MXEX envelope. */
typedef gboolean (*MuxClipboardBrokerClientOutputFunc)(
    MuxClipboardBrokerClient *client,
    GBytes *packet,
    gpointer user_data,
    GError **error);

typedef void (*MuxClipboardBrokerClientReadyFunc)(
    MuxClipboardBrokerClient *client,
    gpointer user_data);

/* summaries and their elements are borrowed for the callback duration. */
typedef void (*MuxClipboardBrokerClientListFunc)(
    MuxClipboardBrokerClient *client,
    GPtrArray *summaries,
    gpointer user_data);

/* snapshot is borrowed for the callback duration. */
typedef void (*MuxClipboardBrokerClientSelectFunc)(
    MuxClipboardBrokerClient *client,
    guint64 entry_id,
    const MuxClipboardSnapshot *snapshot,
    gboolean paste,
    gpointer user_data);

typedef void (*MuxClipboardBrokerClientMutationFunc)(
    MuxClipboardBrokerClient *client,
    MuxClipboardControlType operation,
    guint64 result_value,
    gpointer user_data);

typedef void (*MuxClipboardBrokerClientFailureFunc)(
    MuxClipboardBrokerClient *client,
    const gchar *operation,
    const GError *error,
    gpointer user_data);

typedef void (*MuxClipboardBrokerClientObservationFunc)(
    MuxClipboardBrokerClient *client,
    guint64 transaction_id,
    MuxClipboardBrokerObservationResult result,
    const GError *error,
    gpointer user_data);

MuxClipboardBrokerClient *mux_clipboard_broker_client_new(
    const gchar *profile,
    MuxClipboardHistoryMode mode,
    MuxClipboardBrokerClientOutputFunc output_func,
    MuxClipboardBrokerClientReadyFunc ready_func,
    MuxClipboardBrokerClientListFunc list_func,
    MuxClipboardBrokerClientSelectFunc select_func,
    MuxClipboardBrokerClientMutationFunc mutation_func,
    MuxClipboardBrokerClientFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);
MuxClipboardBrokerClient *mux_clipboard_broker_client_ref(
    MuxClipboardBrokerClient *client);
void mux_clipboard_broker_client_unref(
    MuxClipboardBrokerClient *client);
void mux_clipboard_broker_client_set_observation_func(
    MuxClipboardBrokerClient *client,
    MuxClipboardBrokerClientObservationFunc observation_func);

gboolean mux_clipboard_broker_client_start(
    MuxClipboardBrokerClient *client,
    GError **error);
gboolean mux_clipboard_broker_client_observe(
    MuxClipboardBrokerClient *client,
    guint32 flags,
    const gchar *source_origin,
    guint64 source_view_id,
    const MuxClipboardSnapshot *snapshot,
    GError **error);
gboolean mux_clipboard_broker_client_observe_full(
    MuxClipboardBrokerClient *client,
    guint32 flags,
    const gchar *source_origin,
    guint64 source_view_id,
    const MuxClipboardSnapshot *snapshot,
    guint64 *transaction_id,
    GError **error);
gboolean mux_clipboard_broker_client_list(
    MuxClipboardBrokerClient *client,
    GError **error);
gboolean mux_clipboard_broker_client_select(
    MuxClipboardBrokerClient *client,
    guint64 entry_id,
    gboolean paste,
    GError **error);
gboolean mux_clipboard_broker_client_set_pinned(
    MuxClipboardBrokerClient *client,
    guint64 entry_id,
    gboolean pinned,
    GError **error);
gboolean mux_clipboard_broker_client_delete(
    MuxClipboardBrokerClient *client,
    guint64 entry_id,
    GError **error);
gboolean mux_clipboard_broker_client_clear(
    MuxClipboardBrokerClient *client,
    gboolean include_pinned,
    GError **error);
gboolean mux_clipboard_broker_client_close(
    MuxClipboardBrokerClient *client,
    GError **error);

gboolean mux_clipboard_broker_client_handle_packet(
    MuxClipboardBrokerClient *client,
    const guint8 *packet,
    gsize packet_length,
    GError **error);
guint mux_clipboard_broker_client_tick(
    MuxClipboardBrokerClient *client,
    gint64 monotonic_us);

MuxClipboardBrokerClientState mux_clipboard_broker_client_get_state(
    const MuxClipboardBrokerClient *client);
gboolean mux_clipboard_broker_client_request_pending(
    const MuxClipboardBrokerClient *client);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardBrokerClient,
                              mux_clipboard_broker_client_unref)

G_END_DECLS

#endif
