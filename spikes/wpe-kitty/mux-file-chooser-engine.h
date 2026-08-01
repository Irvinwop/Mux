#ifndef MUX_FILE_CHOOSER_ENGINE_H
#define MUX_FILE_CHOOSER_ENGINE_H

#include "mux-ui-protocol.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxFileChooserBridge MuxFileChooserBridge;

typedef gboolean (*MuxFileChooserSendFunc)(GBytes *payload,
                                           gpointer user_data,
                                           GError **error);

MuxFileChooserBridge *mux_file_chooser_bridge_new(
    WebKitWebView *web_view,
    MuxFileChooserSendFunc send_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_file_chooser_bridge_free(MuxFileChooserBridge *bridge);

gboolean mux_file_chooser_bridge_handle_payload(
    MuxFileChooserBridge *bridge,
    const guint8 *data,
    gsize length,
    GError **error);

void mux_file_chooser_bridge_cancel(
    MuxFileChooserBridge *bridge,
    guint64 request_id,
    MuxUiCancelReason reason,
    gboolean notify_pane);

void mux_file_chooser_bridge_cancel_all(
    MuxFileChooserBridge *bridge,
    MuxUiCancelReason reason,
    gboolean notify_pane);

guint mux_file_chooser_bridge_pending_count(
    const MuxFileChooserBridge *bridge);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxFileChooserBridge,
                              mux_file_chooser_bridge_free)

G_END_DECLS

#endif
