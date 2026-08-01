#pragma once

#include "mux-local-endpoint.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardBrokerTransport MuxClipboardBrokerTransport;

typedef gpointer (*MuxClipboardBrokerProtocolFactoryFunc)(
    MuxClipboardBrokerTransport *transport,
    gpointer user_data,
    GError **error);
typedef gboolean (*MuxClipboardBrokerProtocolPacketFunc)(gpointer protocol,
                                                         GBytes *packet,
                                                         GError **error);
typedef gboolean (*MuxClipboardBrokerProtocolTickFunc)(gpointer protocol,
                                                       gint64 now_us,
                                                       GError **error);
typedef void (*MuxClipboardBrokerProtocolDisconnectedFunc)(
    gpointer protocol,
    const GError *error);

typedef struct {
    MuxClipboardBrokerProtocolPacketFunc handle_packet;
    MuxClipboardBrokerProtocolTickFunc tick;
    MuxClipboardBrokerProtocolDisconnectedFunc disconnected;
    GDestroyNotify destroy;
} MuxClipboardBrokerProtocolOps;

/*
 * Factory data transfers only when construction succeeds. protocol_factory may
 * send an initial HELLO through the supplied transport before returning.
 */
MuxClipboardBrokerTransport *mux_clipboard_broker_transport_new(
    MuxLocalConnection *connection,
    GMainContext *context,
    const MuxClipboardBrokerProtocolOps *ops,
    MuxClipboardBrokerProtocolFactoryFunc protocol_factory,
    gpointer factory_data,
    GDestroyNotify factory_destroy,
    guint tick_interval_ms,
    GError **error);

MuxClipboardBrokerTransport *mux_clipboard_broker_transport_connect(
    const gchar *service,
    uid_t expected_uid,
    GMainContext *context,
    const MuxClipboardBrokerProtocolOps *ops,
    MuxClipboardBrokerProtocolFactoryFunc protocol_factory,
    gpointer factory_data,
    GDestroyNotify factory_destroy,
    guint tick_interval_ms,
    GError **error);

MuxClipboardBrokerTransport *mux_clipboard_broker_transport_ref(
    MuxClipboardBrokerTransport *transport);
void mux_clipboard_broker_transport_unref(
    MuxClipboardBrokerTransport *transport);

/* Protocol output callbacks call this; it never dispatches input inline. */
gboolean mux_clipboard_broker_transport_send(
    MuxClipboardBrokerTransport *transport,
    GBytes *packet,
    GError **error);
void mux_clipboard_broker_transport_close(
    MuxClipboardBrokerTransport *transport);

gboolean mux_clipboard_broker_transport_is_open(
    const MuxClipboardBrokerTransport *transport);
gpointer mux_clipboard_broker_transport_get_protocol(
    const MuxClipboardBrokerTransport *transport);
MuxLocalEndpoint *mux_clipboard_broker_transport_get_endpoint(
    const MuxClipboardBrokerTransport *transport);

G_END_DECLS
