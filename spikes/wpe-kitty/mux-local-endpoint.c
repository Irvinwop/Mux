#include "mux-local-endpoint.h"

#include <gio/gio.h>

typedef struct {
    MuxLocalEndpoint *endpoint;
} EndpointHook;

struct _MuxLocalEndpoint {
    gatomicrefcount references;
    gboolean closed;
    MuxLocalConnection *connection;
    GSource *source;
    EndpointHook *hook;
    MuxLocalEndpointPacketFunc packet_func;
    MuxLocalEndpointEndFunc end_func;
    gpointer user_data;
    GDestroyNotify destroy_notify;
};

static gboolean
source_packet(MuxLocalConnection *connection,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
    EndpointHook *hook = user_data;
    MuxLocalEndpoint *endpoint;
    MuxLocalEndpoint *guard;
    gboolean accepted;

    (void) connection;
    if (hook->endpoint == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "local endpoint was destroyed during dispatch");
        return FALSE;
    }

    endpoint = hook->endpoint;
    guard = mux_local_endpoint_ref(endpoint);
    if (endpoint->closed) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "local endpoint is closed");
        accepted = FALSE;
    } else {
        accepted = endpoint->packet_func(endpoint,
                                         packet,
                                         endpoint->user_data,
                                         error);
    }
    mux_local_endpoint_unref(guard);
    return accepted;
}

static void
source_ended(MuxLocalConnection *connection,
             MuxLocalDispatchResult result,
             const GError *error,
             gpointer user_data)
{
    EndpointHook *hook = user_data;
    MuxLocalEndpoint *endpoint;
    MuxLocalEndpoint *guard;

    (void) connection;
    if (hook->endpoint == NULL)
        return;

    endpoint = hook->endpoint;
    guard = mux_local_endpoint_ref(endpoint);
    if (!endpoint->closed) {
        endpoint->closed = TRUE;
        if (endpoint->end_func != NULL)
            endpoint->end_func(endpoint,
                               result,
                               error,
                               endpoint->user_data);
    }
    mux_local_endpoint_unref(guard);
}

MuxLocalEndpoint *
mux_local_endpoint_new(MuxLocalConnection *connection,
                       GMainContext *context,
                       MuxLocalEndpointPacketFunc packet_func,
                       MuxLocalEndpointEndFunc end_func,
                       gpointer user_data,
                       GDestroyNotify destroy_notify)
{
    MuxLocalEndpoint *endpoint;
    EndpointHook *hook;

    g_return_val_if_fail(connection != NULL, NULL);
    g_return_val_if_fail(packet_func != NULL, NULL);

    endpoint = g_new0(MuxLocalEndpoint, 1);
    g_atomic_ref_count_init(&endpoint->references);
    endpoint->connection = mux_local_connection_ref(connection);
    endpoint->packet_func = packet_func;
    endpoint->end_func = end_func;
    endpoint->user_data = user_data;
    endpoint->destroy_notify = destroy_notify;

    hook = g_new0(EndpointHook, 1);
    hook->endpoint = endpoint;
    endpoint->hook = hook;
    endpoint->source = mux_local_connection_source_new(connection,
                                                       source_packet,
                                                       source_ended,
                                                       hook,
                                                       g_free);
    if (endpoint->source == NULL) {
        hook->endpoint = NULL;
        g_free(hook);
        endpoint->hook = NULL;
        mux_local_connection_unref(endpoint->connection);
        endpoint->connection = NULL;
        endpoint->user_data = NULL;
        endpoint->destroy_notify = NULL;
        g_free(endpoint);
        return NULL;
    }

    g_source_attach(endpoint->source, context);
    return endpoint;
}

MuxLocalEndpoint *
mux_local_endpoint_connect(const gchar *service,
                           uid_t expected_uid,
                           gsize max_packet,
                           gsize queue_limit,
                           GMainContext *context,
                           MuxLocalEndpointPacketFunc packet_func,
                           MuxLocalEndpointEndFunc end_func,
                           gpointer user_data,
                           GDestroyNotify destroy_notify,
                           GError **error)
{
    MuxLocalConnection *connection;
    MuxLocalEndpoint *endpoint;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    connection = mux_local_connection_connect(service,
                                              expected_uid,
                                              max_packet,
                                              queue_limit,
                                              error);
    if (connection == NULL)
        return NULL;

    endpoint = mux_local_endpoint_new(connection,
                                      context,
                                      packet_func,
                                      end_func,
                                      user_data,
                                      destroy_notify);
    mux_local_connection_unref(connection);
    if (endpoint == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to attach local endpoint source");
        return NULL;
    }
    return endpoint;
}

MuxLocalEndpoint *
mux_local_endpoint_ref(MuxLocalEndpoint *endpoint)
{
    g_return_val_if_fail(endpoint != NULL, NULL);
    g_atomic_ref_count_inc(&endpoint->references);
    return endpoint;
}

void
mux_local_endpoint_unref(MuxLocalEndpoint *endpoint)
{
    if (endpoint == NULL ||
        !g_atomic_ref_count_dec(&endpoint->references))
        return;

    endpoint->closed = TRUE;
    if (endpoint->hook != NULL)
        endpoint->hook->endpoint = NULL;
    if (endpoint->source != NULL) {
        g_source_destroy(endpoint->source);
        g_source_unref(endpoint->source);
    }
    mux_local_connection_close(endpoint->connection);
    mux_local_connection_unref(endpoint->connection);
    if (endpoint->destroy_notify != NULL)
        endpoint->destroy_notify(endpoint->user_data);
    g_free(endpoint);
}

gboolean
mux_local_endpoint_send(MuxLocalEndpoint *endpoint,
                        GBytes *packet,
                        GError **error)
{
    g_return_val_if_fail(endpoint != NULL, FALSE);
    g_return_val_if_fail(packet != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (endpoint->closed) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "local endpoint is closed");
        return FALSE;
    }
    return mux_local_connection_source_queue(endpoint->source, packet, error);
}

void
mux_local_endpoint_close(MuxLocalEndpoint *endpoint)
{
    g_return_if_fail(endpoint != NULL);

    if (endpoint->closed)
        return;
    endpoint->closed = TRUE;
    mux_local_connection_close(endpoint->connection);
    if (endpoint->source != NULL)
        g_source_destroy(endpoint->source);
}

gboolean
mux_local_endpoint_is_open(const MuxLocalEndpoint *endpoint)
{
    g_return_val_if_fail(endpoint != NULL, FALSE);
    return !endpoint->closed;
}

MuxLocalConnection *
mux_local_endpoint_get_connection(const MuxLocalEndpoint *endpoint)
{
    g_return_val_if_fail(endpoint != NULL, NULL);
    return endpoint->connection;
}

const MuxLocalPeerCredentials *
mux_local_endpoint_get_peer(const MuxLocalEndpoint *endpoint)
{
    g_return_val_if_fail(endpoint != NULL, NULL);
    return mux_local_connection_get_peer(endpoint->connection);
}
