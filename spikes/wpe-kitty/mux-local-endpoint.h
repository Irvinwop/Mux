#pragma once

#include "mux-local-source.h"

G_BEGIN_DECLS

typedef struct _MuxLocalEndpoint MuxLocalEndpoint;

typedef gboolean (*MuxLocalEndpointPacketFunc)(MuxLocalEndpoint *endpoint,
                                               GBytes *packet,
                                               gpointer user_data,
                                               GError **error);
typedef void (*MuxLocalEndpointEndFunc)(MuxLocalEndpoint *endpoint,
                                       MuxLocalDispatchResult result,
                                       const GError *error,
                                       gpointer user_data);

/*
 * Both constructors take ownership of user_data only on success. The
 * connection itself is referenced and remains owned by its caller as well.
 */
MuxLocalEndpoint *mux_local_endpoint_new(
    MuxLocalConnection *connection,
    GMainContext *context,
    MuxLocalEndpointPacketFunc packet_func,
    MuxLocalEndpointEndFunc end_func,
    gpointer user_data,
    GDestroyNotify destroy_notify);

MuxLocalEndpoint *mux_local_endpoint_connect(
    const gchar *service,
    uid_t expected_uid,
    gsize max_packet,
    gsize queue_limit,
    GMainContext *context,
    MuxLocalEndpointPacketFunc packet_func,
    MuxLocalEndpointEndFunc end_func,
    gpointer user_data,
    GDestroyNotify destroy_notify,
    GError **error);

MuxLocalEndpoint *mux_local_endpoint_ref(MuxLocalEndpoint *endpoint);
void mux_local_endpoint_unref(MuxLocalEndpoint *endpoint);

gboolean mux_local_endpoint_send(MuxLocalEndpoint *endpoint,
                                 GBytes *packet,
                                 GError **error);
void mux_local_endpoint_close(MuxLocalEndpoint *endpoint);

gboolean mux_local_endpoint_is_open(const MuxLocalEndpoint *endpoint);
MuxLocalConnection *mux_local_endpoint_get_connection(
    const MuxLocalEndpoint *endpoint);
const MuxLocalPeerCredentials *mux_local_endpoint_get_peer(
    const MuxLocalEndpoint *endpoint);

G_END_DECLS
