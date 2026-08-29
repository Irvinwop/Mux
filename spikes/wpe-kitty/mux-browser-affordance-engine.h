#ifndef MUX_BROWSER_AFFORDANCE_ENGINE_H
#define MUX_BROWSER_AFFORDANCE_ENGINE_H

#include "mux-ui-protocol.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxBrowserAffordanceBridge MuxBrowserAffordanceBridge;

typedef gboolean (*MuxBrowserAffordanceSendFunc)(GBytes *payload,
                                                  gpointer user_data,
                                                  GError **error);

typedef gboolean (*MuxBrowserAffordanceChoiceFunc)(guint32 choice_id,
                                                    gpointer user_data,
                                                    GError **error);

typedef gboolean (*MuxBrowserAffordanceDownloadFunc)(
    WebKitWebView *web_view,
    const gchar *uri,
    gpointer user_data,
    GError **error);

MuxBrowserAffordanceBridge *mux_browser_affordance_bridge_new(
    WebKitWebView *web_view,
    gboolean private_profile,
    MuxBrowserAffordanceSendFunc send_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_browser_affordance_bridge_free(
    MuxBrowserAffordanceBridge *bridge);

gboolean mux_browser_affordance_bridge_handle_payload(
    MuxBrowserAffordanceBridge *bridge,
    const guint8 *data,
    gsize length,
    GError **error);

void mux_browser_affordance_bridge_cancel_all(
    MuxBrowserAffordanceBridge *bridge,
    MuxUiCancelReason reason,
    gboolean notify_pane);

guint mux_browser_affordance_bridge_pending_count(
    const MuxBrowserAffordanceBridge *bridge);

void mux_browser_affordance_bridge_set_download_func(
    MuxBrowserAffordanceBridge *bridge,
    MuxBrowserAffordanceDownloadFunc download_func);

gboolean mux_browser_affordance_bridge_show_command_surface(
    MuxBrowserAffordanceBridge *bridge,
    const gchar *heading,
    const gchar *message,
    const GPtrArray *choices,
    MuxBrowserAffordanceChoiceFunc choice_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error);

gboolean mux_browser_affordance_bridge_show_status(
    MuxBrowserAffordanceBridge *bridge,
    const gchar *heading,
    const gchar *message,
    gboolean danger,
    GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxBrowserAffordanceBridge,
                              mux_browser_affordance_bridge_free)

G_END_DECLS

#endif
