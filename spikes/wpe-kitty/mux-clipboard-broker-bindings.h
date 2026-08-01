#pragma once

#include "mux-clipboard-broker-client.h"
#include "mux-clipboard-broker-peer.h"
#include "mux-clipboard-broker-transport.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_BROKER_SERVICE "clipboard.sock"
#define MUX_CLIPBOARD_BROKER_TICK_MS 250u

MuxClipboardBrokerTransport *
mux_clipboard_broker_client_transport_connect(
    const gchar *profile,
    MuxClipboardHistoryMode mode,
    GMainContext *context,
    MuxClipboardBrokerClientReadyFunc ready_func,
    MuxClipboardBrokerClientListFunc list_func,
    MuxClipboardBrokerClientSelectFunc select_func,
    MuxClipboardBrokerClientMutationFunc mutation_func,
    MuxClipboardBrokerClientFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error);

MuxClipboardBrokerClient *
mux_clipboard_broker_client_transport_get_client(
    MuxClipboardBrokerTransport *transport);

/* broker is borrowed and must outlive the returned peer transport. */
MuxClipboardBrokerTransport *
mux_clipboard_broker_peer_transport_new(
    MuxLocalConnection *connection,
    MuxClipboardBroker *broker,
    GMainContext *context,
    GError **error);

MuxClipboardBrokerPeer *
mux_clipboard_broker_peer_transport_get_peer(
    MuxClipboardBrokerTransport *transport);

G_END_DECLS
