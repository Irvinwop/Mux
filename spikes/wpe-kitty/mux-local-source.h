#pragma once

#include "mux-local-transport.h"

G_BEGIN_DECLS

typedef void (*MuxLocalConnectionEndFunc)(
    MuxLocalConnection *connection,
    MuxLocalDispatchResult result,
    const GError *error,
    gpointer user_data);

/*
 * The accepted connection is borrowed for the duration of the callback. Take
 * a reference before returning when the application wants to retain it.
 */
typedef gboolean (*MuxLocalAcceptFunc)(MuxLocalListener *listener,
                                       MuxLocalConnection *connection,
                                       gpointer user_data,
                                       GError **error);

typedef void (*MuxLocalListenerFailureFunc)(MuxLocalListener *listener,
                                            const GError *error,
                                            gpointer user_data);

/* Returned sources are unattached and follow normal GSource ownership rules. */
GSource *mux_local_connection_source_new(
    MuxLocalConnection *connection,
    MuxLocalPacketFunc packet_func,
    MuxLocalConnectionEndFunc end_func,
    gpointer user_data,
    GDestroyNotify destroy_notify);

/*
 * Queue through the source when called outside its dispatch callback. This
 * updates writable interest and wakes the attached main context.
 */
gboolean mux_local_connection_source_queue(GSource *source,
                                           GBytes *packet,
                                           GError **error);

GSource *mux_local_listener_source_new(
    MuxLocalListener *listener,
    uid_t expected_uid,
    gsize max_packet,
    gsize queue_limit,
    MuxLocalAcceptFunc accept_func,
    MuxLocalListenerFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify destroy_notify);

G_END_DECLS
