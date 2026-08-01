#include "mux-clipboard-broker-bindings.h"

#include <gio/gio.h>
#include <unistd.h>

#define CLIENT_PROTOCOL_MAGIC 0x4d584243u
#define PEER_PROTOCOL_MAGIC 0x4d584250u

typedef struct {
    MuxClipboardBrokerTransport *transport;
    gchar *profile;
    MuxClipboardHistoryMode mode;
    MuxClipboardBrokerClientReadyFunc ready_func;
    MuxClipboardBrokerClientListFunc list_func;
    MuxClipboardBrokerClientSelectFunc select_func;
    MuxClipboardBrokerClientMutationFunc mutation_func;
    MuxClipboardBrokerClientFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
} ClientBinding;

typedef struct {
    guint32 magic;
    ClientBinding *binding;
    MuxClipboardBrokerClient *client;
} ClientProtocol;

typedef struct {
    MuxClipboardBrokerTransport *transport;
    MuxClipboardBroker *broker;
} PeerBinding;

typedef struct {
    guint32 magic;
    PeerBinding *binding;
    MuxClipboardBrokerPeer *peer;
} PeerProtocol;

static gboolean
client_output(MuxClipboardBrokerClient *client,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
    ClientBinding *binding = user_data;

    (void) client;
    if (binding->transport == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "clipboard client transport is unavailable");
        return FALSE;
    }
    return mux_clipboard_broker_transport_send(binding->transport,
                                               packet,
                                               error);
}

static void
client_ready(MuxClipboardBrokerClient *client, gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->ready_func != NULL)
        binding->ready_func(client, binding->user_data);
}

static void
client_list(MuxClipboardBrokerClient *client,
            GPtrArray *summaries,
            gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->list_func != NULL)
        binding->list_func(client, summaries, binding->user_data);
}

static void
client_select(MuxClipboardBrokerClient *client,
              guint64 entry_id,
              const MuxClipboardSnapshot *snapshot,
              gboolean paste,
              gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->select_func != NULL)
        binding->select_func(client,
                             entry_id,
                             snapshot,
                             paste,
                             binding->user_data);
}

static void
client_mutation(MuxClipboardBrokerClient *client,
                MuxClipboardControlType operation,
                guint64 result_value,
                gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->mutation_func != NULL)
        binding->mutation_func(client,
                               operation,
                               result_value,
                               binding->user_data);
}

static void
client_failure(MuxClipboardBrokerClient *client,
               const gchar *operation,
               const GError *error,
               gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->failure_func != NULL)
        binding->failure_func(client,
                              operation,
                              error,
                              binding->user_data);
}

static gpointer
client_factory(MuxClipboardBrokerTransport *transport,
               gpointer user_data,
               GError **error)
{
    ClientBinding *binding = user_data;
    ClientProtocol *protocol = g_new0(ClientProtocol, 1);

    binding->transport = transport;
    protocol->magic = CLIENT_PROTOCOL_MAGIC;
    protocol->binding = binding;
    protocol->client = mux_clipboard_broker_client_new(
        binding->profile,
        binding->mode,
        client_output,
        client_ready,
        client_list,
        client_select,
        client_mutation,
        client_failure,
        binding,
        NULL);
    if (protocol->client == NULL) {
        binding->transport = NULL;
        g_free(protocol);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "failed to construct clipboard broker client");
        return NULL;
    }

    if (!mux_clipboard_broker_client_start(protocol->client, error)) {
        binding->transport = NULL;
        mux_clipboard_broker_client_unref(protocol->client);
        g_free(protocol);
        return NULL;
    }
    return protocol;
}

static gboolean
client_handle_packet(gpointer protocol_pointer,
                     GBytes *packet,
                     GError **error)
{
    ClientProtocol *protocol = protocol_pointer;
    gsize packet_length;
    gconstpointer packet_data = g_bytes_get_data(packet, &packet_length);

    return mux_clipboard_broker_client_handle_packet(protocol->client,
                                                     packet_data,
                                                     packet_length,
                                                     error);
}

static gboolean
client_tick(gpointer protocol_pointer, gint64 now_us, GError **error)
{
    ClientProtocol *protocol = protocol_pointer;

    (void) error;
    mux_clipboard_broker_client_tick(protocol->client, now_us);
    return TRUE;
}

static void
client_disconnected(gpointer protocol_pointer, const GError *error)
{
    ClientProtocol *protocol = protocol_pointer;
    ClientBinding *binding = protocol->binding;
    g_autoptr(GError) fallback = NULL;

    if (error == NULL) {
        fallback = g_error_new_literal(G_IO_ERROR,
                                       G_IO_ERROR_CONNECTION_CLOSED,
                                       "clipboard broker disconnected");
        error = fallback;
    }
    if (binding->failure_func != NULL)
        binding->failure_func(protocol->client,
                              "transport",
                              error,
                              binding->user_data);
}

static void
client_protocol_destroy(gpointer protocol_pointer)
{
    ClientProtocol *protocol = protocol_pointer;

    protocol->binding->transport = NULL;
    mux_clipboard_broker_client_unref(protocol->client);
    protocol->magic = 0;
    g_free(protocol);
}

static void
client_binding_destroy(gpointer user_data)
{
    ClientBinding *binding = user_data;

    if (binding->user_data_destroy != NULL)
        binding->user_data_destroy(binding->user_data);
    g_free(binding->profile);
    g_free(binding);
}

static void
client_binding_abandon(ClientBinding *binding)
{
    g_free(binding->profile);
    g_free(binding);
}

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
    GError **error)
{
    static const MuxClipboardBrokerProtocolOps protocol_ops = {
        .handle_packet = client_handle_packet,
        .tick = client_tick,
        .disconnected = client_disconnected,
        .destroy = client_protocol_destroy,
    };
    ClientBinding *binding;
    MuxClipboardBrokerTransport *transport;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (profile == NULL || *profile == '\0') {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "clipboard profile must not be empty");
        return NULL;
    }

    binding = g_new0(ClientBinding, 1);
    binding->profile = g_strdup(profile);
    binding->mode = mode;
    binding->ready_func = ready_func;
    binding->list_func = list_func;
    binding->select_func = select_func;
    binding->mutation_func = mutation_func;
    binding->failure_func = failure_func;
    binding->user_data = user_data;
    binding->user_data_destroy = user_data_destroy;

    transport = mux_clipboard_broker_transport_connect(
        MUX_CLIPBOARD_BROKER_SERVICE,
        geteuid(),
        context,
        &protocol_ops,
        client_factory,
        binding,
        client_binding_destroy,
        MUX_CLIPBOARD_BROKER_TICK_MS,
        error);
    if (transport == NULL)
        client_binding_abandon(binding);
    return transport;
}

MuxClipboardBrokerClient *
mux_clipboard_broker_client_transport_get_client(
    MuxClipboardBrokerTransport *transport)
{
    ClientProtocol *protocol;

    g_return_val_if_fail(transport != NULL, NULL);
    protocol = mux_clipboard_broker_transport_get_protocol(transport);
    g_return_val_if_fail(protocol != NULL &&
                         protocol->magic == CLIENT_PROTOCOL_MAGIC,
                         NULL);
    return protocol->client;
}

static gboolean
peer_output(MuxClipboardBrokerPeer *peer,
            GBytes *packet,
            gpointer user_data,
            GError **error)
{
    PeerBinding *binding = user_data;

    (void) peer;
    if (binding->transport == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "clipboard peer transport is unavailable");
        return FALSE;
    }
    return mux_clipboard_broker_transport_send(binding->transport,
                                               packet,
                                               error);
}

static gpointer
peer_factory(MuxClipboardBrokerTransport *transport,
             gpointer user_data,
             GError **error)
{
    PeerBinding *binding = user_data;
    PeerProtocol *protocol = g_new0(PeerProtocol, 1);

    binding->transport = transport;
    protocol->magic = PEER_PROTOCOL_MAGIC;
    protocol->binding = binding;
    protocol->peer = mux_clipboard_broker_peer_new(binding->broker,
                                                   peer_output,
                                                   binding,
                                                   NULL);
    if (protocol->peer == NULL) {
        binding->transport = NULL;
        g_free(protocol);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to construct clipboard broker peer");
        return NULL;
    }
    return protocol;
}

static gboolean
peer_handle_packet(gpointer protocol_pointer,
                   GBytes *packet,
                   GError **error)
{
    PeerProtocol *protocol = protocol_pointer;
    gsize packet_length;
    gconstpointer packet_data = g_bytes_get_data(packet, &packet_length);

    return mux_clipboard_broker_peer_handle_packet(protocol->peer,
                                                   packet_data,
                                                   packet_length,
                                                   error);
}

static gboolean
peer_tick(gpointer protocol_pointer, gint64 now_us, GError **error)
{
    PeerProtocol *protocol = protocol_pointer;

    if (!mux_clipboard_broker_peer_tick(protocol->peer, now_us))
        return TRUE;
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "clipboard broker peer timed out");
    return FALSE;
}

static void
peer_disconnected(gpointer protocol_pointer, const GError *error)
{
    (void) protocol_pointer;
    (void) error;
}

static void
peer_protocol_destroy(gpointer protocol_pointer)
{
    PeerProtocol *protocol = protocol_pointer;

    protocol->binding->transport = NULL;
    mux_clipboard_broker_peer_unref(protocol->peer);
    protocol->magic = 0;
    g_free(protocol);
}

static void
peer_binding_destroy(gpointer user_data)
{
    g_free(user_data);
}

MuxClipboardBrokerTransport *
mux_clipboard_broker_peer_transport_new(
    MuxLocalConnection *connection,
    MuxClipboardBroker *broker,
    GMainContext *context,
    GError **error)
{
    static const MuxClipboardBrokerProtocolOps protocol_ops = {
        .handle_packet = peer_handle_packet,
        .tick = peer_tick,
        .disconnected = peer_disconnected,
        .destroy = peer_protocol_destroy,
    };
    PeerBinding *binding;
    MuxClipboardBrokerTransport *transport;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (connection == NULL || broker == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "peer transport requires a connection and broker");
        return NULL;
    }

    binding = g_new0(PeerBinding, 1);
    binding->broker = broker;
    transport = mux_clipboard_broker_transport_new(
        connection,
        context,
        &protocol_ops,
        peer_factory,
        binding,
        peer_binding_destroy,
        MUX_CLIPBOARD_BROKER_TICK_MS,
        error);
    if (transport == NULL)
        g_free(binding);
    return transport;
}

MuxClipboardBrokerPeer *
mux_clipboard_broker_peer_transport_get_peer(
    MuxClipboardBrokerTransport *transport)
{
    PeerProtocol *protocol;

    g_return_val_if_fail(transport != NULL, NULL);
    protocol = mux_clipboard_broker_transport_get_protocol(transport);
    g_return_val_if_fail(protocol != NULL &&
                         protocol->magic == PEER_PROTOCOL_MAGIC,
                         NULL);
    return protocol->peer;
}
