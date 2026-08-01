#ifndef MUX_UI_ENGINE_H
#define MUX_UI_ENGINE_H

#include "mux-ui-protocol.h"
#include "mux-permission-store.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxUiEngineBridge MuxUiEngineBridge;

/*
 * The payload is valid only for the duration of the callback. It is a complete
 * mux-ui-protocol record and must be wrapped in the engine transport envelope.
 */
typedef gboolean (*MuxUiEngineSendFunc)(GBytes *payload,
                                        gpointer user_data,
                                        GError **error);

MuxUiEngineBridge *mux_ui_engine_bridge_new(
    WebKitWebView *web_view,
    MuxUiEngineSendFunc send_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_ui_engine_bridge_free(MuxUiEngineBridge *bridge);

/*
 * The bridge holds a reference. Passing NULL returns to one-shot decisions.
 */
void mux_ui_engine_bridge_set_permission_store(
    MuxUiEngineBridge *bridge,
    MuxPermissionStore *store);

/*
 * Handles a complete response or cancel record received from the pane. Unknown
 * request IDs are intentionally ignored so late and duplicate responses are
 * harmless.
 */
gboolean mux_ui_engine_bridge_handle_payload(MuxUiEngineBridge *bridge,
                                             const guint8 *data,
                                             gsize length,
                                             GError **error);

void mux_ui_engine_bridge_cancel(MuxUiEngineBridge *bridge,
                                 guint64 request_id,
                                 MuxUiCancelReason reason,
                                 gboolean notify_pane);

void mux_ui_engine_bridge_cancel_all(MuxUiEngineBridge *bridge,
                                     MuxUiCancelReason reason,
                                     gboolean notify_pane);

guint mux_ui_engine_bridge_pending_count(const MuxUiEngineBridge *bridge);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiEngineBridge,
                              mux_ui_engine_bridge_free)

G_END_DECLS

#endif
