#include "mux-clipboard-broker-transport.h"

#include <gio/gio.h>
#include <unistd.h>

typedef struct {
    MuxClipboardBrokerTransport *transport;
} TransportHook;

struct _MuxClipboardBrokerTransport {
    gatomicrefcount references;
    gboolean open;
    gboolean destroying;
    gboolean disconnected_notified;
    MuxLocalEndpoint *endpoint;
    TransportHook *hook;
    MuxClipboardBrokerProtocolOps ops;
    gpointer protocol;
    gpointer factory_data;
    GDestroyNotify factory_destroy;
    GSource *tick_source;
    guint tick_interval_ms;
};

static void
stop_tick(MuxClipboardBrokerTransport *transport)
{
    if (transport->tick_source == NULL)
        return;
    g_source_destroy(transport->tick_source);
    g_source_unref(transport->tick_source);
    transport->tick_source = NULL;
}

static void
notify_disconnected(MuxClipboardBrokerTransport *transport,
                    const GError *error)
{
    if (transport->disconnected_notified)
        return;
    transport->disconnected_notified = TRUE;
    if (transport->protocol != NULL && transport->ops.disconnected != NULL)
        transport->ops.disconnected(transport->protocol, error);
}

static gboolean
endpoint_packet(MuxLocalEndpoint *endpoint,
                GBytes *packet,
                gpointer user_data,
                GError **error)
{
    TransportHook *hook = user_data;
    MuxClipboardBrokerTransport *transport;
    MuxClipboardBrokerTransport *guard;
    gboolean accepted;

    (void) endpoint;
    if (hook->transport == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "clipboard broker transport was destroyed");
        return FALSE;
    }

    transport = hook->transport;
    guard = mux_clipboard_broker_transport_ref(transport);
    if (!transport->open || transport->protocol == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "clipboard packet arrived before protocol initialization");
        accepted = FALSE;
    } else {
        accepted = transport->ops.handle_packet(transport->protocol,
                                                packet,
                                                error);
    }
    mux_clipboard_broker_transport_unref(guard);
    return accepted;
}

static void
endpoint_ended(MuxLocalEndpoint *endpoint,
               MuxLocalDispatchResult result,
               const GError *error,
               gpointer user_data)
{
    TransportHook *hook = user_data;
    MuxClipboardBrokerTransport *transport;
    MuxClipboardBrokerTransport *guard;
    g_autoptr(GError) closed_error = NULL;

    (void) endpoint;
    if (hook->transport == NULL)
        return;

    transport = hook->transport;
    guard = mux_clipboard_broker_transport_ref(transport);
    transport->open = FALSE;
    stop_tick(transport);
    if (error == NULL && result == MUX_LOCAL_DISPATCH_CLOSED) {
        closed_error = g_error_new_literal(G_IO_ERROR,
                                           G_IO_ERROR_CONNECTION_CLOSED,
                                           "clipboard broker disconnected");
        error = closed_error;
    }
    notify_disconnected(transport, error);
    mux_clipboard_broker_transport_unref(guard);
}

static gboolean
tick_protocol(gpointer user_data)
{
    MuxClipboardBrokerTransport *transport = user_data;
    MuxClipboardBrokerTransport *guard;
    g_autoptr(GError) error = NULL;
    gboolean healthy;

    guard = mux_clipboard_broker_transport_ref(transport);
    if (!transport->open || transport->protocol == NULL ||
        transport->ops.tick == NULL) {
        mux_clipboard_broker_transport_unref(guard);
        return G_SOURCE_REMOVE;
    }

    healthy = transport->ops.tick(transport->protocol,
                                  g_get_monotonic_time(),
                                  &error);
    if (!healthy) {
        if (error == NULL)
            error = g_error_new_literal(G_IO_ERROR,
                                        G_IO_ERROR_TIMED_OUT,
                                        "clipboard broker protocol timed out");
        transport->open = FALSE;
        if (transport->tick_source != NULL) {
            g_source_unref(transport->tick_source);
            transport->tick_source = NULL;
        }
        mux_local_endpoint_close(transport->endpoint);
        notify_disconnected(transport, error);
        mux_clipboard_broker_transport_unref(guard);
        return G_SOURCE_REMOVE;
    }

    mux_clipboard_broker_transport_unref(guard);
    return G_SOURCE_CONTINUE;
}

static void
free_partial(MuxClipboardBrokerTransport *transport,
             gboolean destroy_factory_data)
{
    transport->destroying = TRUE;
    stop_tick(transport);
    if (transport->hook != NULL)
        transport->hook->transport = NULL;
    if (transport->protocol != NULL && transport->ops.destroy != NULL)
        transport->ops.destroy(transport->protocol);
    transport->protocol = NULL;
    g_clear_pointer(&transport->endpoint, mux_local_endpoint_unref);
    if (destroy_factory_data && transport->factory_destroy != NULL)
        transport->factory_destroy(transport->factory_data);
    g_free(transport);
}

MuxClipboardBrokerTransport *
mux_clipboard_broker_transport_new(
    MuxLocalConnection *connection,
    GMainContext *context,
    const MuxClipboardBrokerProtocolOps *ops,
    MuxClipboardBrokerProtocolFactoryFunc protocol_factory,
    gpointer factory_data,
    GDestroyNotify factory_destroy,
    guint tick_interval_ms,
    GError **error)
{
    MuxClipboardBrokerTransport *transport;
    TransportHook *hook;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (connection == NULL || ops == NULL || ops->handle_packet == NULL ||
        ops->destroy == NULL || protocol_factory == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid clipboard broker transport construction");
        return NULL;
    }

    transport = g_new0(MuxClipboardBrokerTransport, 1);
    g_atomic_ref_count_init(&transport->references);
    transport->ops = *ops;
    transport->factory_data = factory_data;
    transport->factory_destroy = factory_destroy;
    transport->tick_interval_ms = tick_interval_ms;

    hook = g_new0(TransportHook, 1);
    hook->transport = transport;
    transport->hook = hook;
    transport->endpoint = mux_local_endpoint_new(connection,
                                                 context,
                                                 endpoint_packet,
                                                 endpoint_ended,
                                                 hook,
                                                 g_free);
    if (transport->endpoint == NULL) {
        hook->transport = NULL;
        g_free(hook);
        transport->hook = NULL;
        free_partial(transport, FALSE);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to attach clipboard broker endpoint");
        return NULL;
    }

    transport->open = TRUE;
    transport->protocol = protocol_factory(transport, factory_data, error);
    if (transport->protocol == NULL) {
        transport->open = FALSE;
        free_partial(transport, FALSE);
        if (error != NULL && *error == NULL)
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "failed to construct clipboard broker protocol");
        return NULL;
    }

    if (ops->tick != NULL && tick_interval_ms > 0) {
        transport->tick_source = g_timeout_source_new(tick_interval_ms);
        g_source_set_callback(transport->tick_source,
                              tick_protocol,
                              transport,
                              NULL);
        g_source_set_name(transport->tick_source,
                          "mux-clipboard-broker-tick");
        g_source_attach(transport->tick_source, context);
    }
    return transport;
}

MuxClipboardBrokerTransport *
mux_clipboard_broker_transport_connect(
    const gchar *service,
    uid_t expected_uid,
    GMainContext *context,
    const MuxClipboardBrokerProtocolOps *ops,
    MuxClipboardBrokerProtocolFactoryFunc protocol_factory,
    gpointer factory_data,
    GDestroyNotify factory_destroy,
    guint tick_interval_ms,
    GError **error)
{
    MuxLocalConnection *connection;
    MuxClipboardBrokerTransport *transport;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    connection = mux_local_connection_connect(
        service,
        expected_uid,
        MUX_LOCAL_TRANSPORT_DEFAULT_MAX_PACKET,
        MUX_LOCAL_TRANSPORT_DEFAULT_QUEUE_LIMIT,
        error);
    if (connection == NULL)
        return NULL;

    transport = mux_clipboard_broker_transport_new(connection,
                                                   context,
                                                   ops,
                                                   protocol_factory,
                                                   factory_data,
                                                   factory_destroy,
                                                   tick_interval_ms,
                                                   error);
    mux_local_connection_unref(connection);
    return transport;
}

MuxClipboardBrokerTransport *
mux_clipboard_broker_transport_ref(MuxClipboardBrokerTransport *transport)
{
    g_return_val_if_fail(transport != NULL, NULL);
    g_atomic_ref_count_inc(&transport->references);
    return transport;
}

void
mux_clipboard_broker_transport_unref(MuxClipboardBrokerTransport *transport)
{
    if (transport == NULL ||
        !g_atomic_ref_count_dec(&transport->references))
        return;

    transport->open = FALSE;
    free_partial(transport, TRUE);
}

gboolean
mux_clipboard_broker_transport_send(MuxClipboardBrokerTransport *transport,
                                    GBytes *packet,
                                    GError **error)
{
    g_return_val_if_fail(transport != NULL, FALSE);
    g_return_val_if_fail(packet != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (!transport->open) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "clipboard broker transport is closed");
        return FALSE;
    }
    return mux_local_endpoint_send(transport->endpoint, packet, error);
}

void
mux_clipboard_broker_transport_close(MuxClipboardBrokerTransport *transport)
{
    g_return_if_fail(transport != NULL);

    if (!transport->open)
        return;
    transport->open = FALSE;
    stop_tick(transport);
    mux_local_endpoint_close(transport->endpoint);
}

gboolean
mux_clipboard_broker_transport_is_open(
    const MuxClipboardBrokerTransport *transport)
{
    g_return_val_if_fail(transport != NULL, FALSE);
    return transport->open;
}

gpointer
mux_clipboard_broker_transport_get_protocol(
    const MuxClipboardBrokerTransport *transport)
{
    g_return_val_if_fail(transport != NULL, NULL);
    return transport->protocol;
}

MuxLocalEndpoint *
mux_clipboard_broker_transport_get_endpoint(
    const MuxClipboardBrokerTransport *transport)
{
    g_return_val_if_fail(transport != NULL, NULL);
    return transport->endpoint;
}
