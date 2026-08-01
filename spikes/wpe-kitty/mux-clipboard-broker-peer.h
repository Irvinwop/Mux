#ifndef MUX_CLIPBOARD_BROKER_PEER_H
#define MUX_CLIPBOARD_BROKER_PEER_H

#include "mux-clipboard-broker.h"
#include "mux-clipboard-control.h"
#include "mux-clipboard-wire.h"
#include "mux-extension-protocol.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardBrokerPeer MuxClipboardBrokerPeer;

/* packet is a borrowed complete MXEX envelope. */
typedef gboolean (*MuxClipboardBrokerPeerOutputFunc)(
    MuxClipboardBrokerPeer *peer,
    GBytes *packet,
    gpointer user_data,
    GError **error);

MuxClipboardBrokerPeer *mux_clipboard_broker_peer_new(
    MuxClipboardBroker *broker,
    MuxClipboardBrokerPeerOutputFunc output_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);
MuxClipboardBrokerPeer *mux_clipboard_broker_peer_ref(
    MuxClipboardBrokerPeer *peer);
void mux_clipboard_broker_peer_unref(MuxClipboardBrokerPeer *peer);

gboolean mux_clipboard_broker_peer_handle_packet(
    MuxClipboardBrokerPeer *peer,
    const guint8 *packet,
    gsize packet_length,
    GError **error);
gboolean mux_clipboard_broker_peer_tick(MuxClipboardBrokerPeer *peer,
                                        gint64 monotonic_us);

const gchar *mux_clipboard_broker_peer_get_profile(
    const MuxClipboardBrokerPeer *peer);
MuxClipboardHistoryMode mux_clipboard_broker_peer_get_mode(
    const MuxClipboardBrokerPeer *peer);
gboolean mux_clipboard_broker_peer_is_closed(
    const MuxClipboardBrokerPeer *peer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardBrokerPeer,
                              mux_clipboard_broker_peer_unref)

G_END_DECLS

#endif
