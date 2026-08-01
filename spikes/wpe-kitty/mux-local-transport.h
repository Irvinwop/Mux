#pragma once

#include <gio/gio.h>
#include <sys/types.h>

G_BEGIN_DECLS

#define MUX_LOCAL_TRANSPORT_DEFAULT_MAX_PACKET ((gsize) (256u * 1024u + 64u))
#define MUX_LOCAL_TRANSPORT_DEFAULT_QUEUE_LIMIT ((gsize) (4u * 1024u * 1024u))

typedef struct _MuxLocalListener MuxLocalListener;
typedef struct _MuxLocalConnection MuxLocalConnection;

typedef struct {
    pid_t pid;
    uid_t uid;
    gid_t gid;
} MuxLocalPeerCredentials;

typedef enum {
    MUX_LOCAL_DISPATCH_OK,
    MUX_LOCAL_DISPATCH_CLOSED,
    MUX_LOCAL_DISPATCH_ERROR,
} MuxLocalDispatchResult;

typedef gboolean (*MuxLocalPacketFunc)(MuxLocalConnection *connection,
                                       GBytes *packet,
                                       gpointer user_data,
                                       GError **error);

/*
 * Service names are simple filenames such as "clipboard.sock". They are
 * resolved below $XDG_RUNTIME_DIR/mux, or /tmp/mux-UID when no runtime
 * directory is available. The containing directory is always owner-only.
 */
gchar *mux_local_transport_socket_path(const gchar *service,
                                       GError **error);

MuxLocalListener *mux_local_listener_new(const gchar *service,
                                         guint backlog,
                                         GError **error);
MuxLocalListener *mux_local_listener_ref(MuxLocalListener *listener);
void mux_local_listener_unref(MuxLocalListener *listener);
void mux_local_listener_free(MuxLocalListener *listener);

gint mux_local_listener_get_fd(const MuxLocalListener *listener);
const gchar *mux_local_listener_get_path(const MuxLocalListener *listener);

/*
 * Returns NULL with out_would_block set when all pending accepts have been
 * drained. Any accepted descriptor is closed internally if authentication or
 * connection construction fails.
 */
MuxLocalConnection *mux_local_listener_accept(MuxLocalListener *listener,
                                              uid_t expected_uid,
                                              gsize max_packet,
                                              gsize queue_limit,
                                              gboolean *out_would_block,
                                              GError **error);

MuxLocalConnection *mux_local_connection_connect(const gchar *service,
                                                  uid_t expected_uid,
                                                  gsize max_packet,
                                                  gsize queue_limit,
                                                  GError **error);

/* Takes ownership of fd, including on failure. */
MuxLocalConnection *mux_local_connection_new_take_fd(
    gint fd,
    uid_t expected_uid,
    gsize max_packet,
    gsize queue_limit,
    GError **error);

MuxLocalConnection *mux_local_connection_ref(MuxLocalConnection *connection);
void mux_local_connection_unref(MuxLocalConnection *connection);

gint mux_local_connection_get_fd(const MuxLocalConnection *connection);
const MuxLocalPeerCredentials *mux_local_connection_get_peer(
    const MuxLocalConnection *connection);
gsize mux_local_connection_get_queued_bytes(
    const MuxLocalConnection *connection);
GIOCondition mux_local_connection_wanted_condition(
    const MuxLocalConnection *connection);

/* Queueing never invokes packet callbacks and is safe from output reentry. */
gboolean mux_local_connection_queue(MuxLocalConnection *connection,
                                    GBytes *packet,
                                    GError **error);

/*
 * Drain readable packets and queued writes for one main-loop readiness event.
 * A FALSE packet callback is treated as a protocol failure. The callback may
 * queue response packets on this connection.
 */
MuxLocalDispatchResult mux_local_connection_dispatch(
    MuxLocalConnection *connection,
    GIOCondition condition,
    MuxLocalPacketFunc packet_func,
    gpointer user_data,
    GError **error);

void mux_local_connection_close(MuxLocalConnection *connection);

G_END_DECLS
