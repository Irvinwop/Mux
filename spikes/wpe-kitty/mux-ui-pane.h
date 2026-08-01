#ifndef MUX_UI_PANE_H
#define MUX_UI_PANE_H

#include "mux-pane-overlay.h"

G_BEGIN_DECLS

#define MUX_ENGINE_EXTENSION_MESSAGE_UI 64U
#define MUX_ENGINE_EXTENSION_CAP_DIALOGS (G_GUINT64_CONSTANT(1) << 0)
#define MUX_UI_PANE_GRAPHICS_Z_INDEX (-1)

typedef struct _MuxUiPaneBridge MuxUiPaneBridge;

/*
 * send_func wraps a complete mux-ui-protocol record in the engine transport.
 * write_func writes ANSI bytes to the pane's controlling terminal.
 */
typedef gboolean (*MuxUiPaneSendFunc)(GBytes *payload,
                                      gpointer user_data,
                                      GError **error);

typedef gboolean (*MuxUiPaneWriteFunc)(const guint8 *data,
                                       gsize length,
                                       gpointer user_data,
                                       GError **error);

MuxUiPaneBridge *mux_ui_pane_bridge_new(
    MuxUiPaneSendFunc send_func,
    MuxUiPaneWriteFunc write_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_ui_pane_bridge_free(MuxUiPaneBridge *bridge);

/*
 * Handles a complete UI request or cancel record from mux-engine.
 */
gboolean mux_ui_pane_bridge_handle_payload(MuxUiPaneBridge *bridge,
                                           const guint8 *data,
                                           gsize length,
                                           GError **error);

/*
 * Normalized key input must pass through this function before it is forwarded
 * to WebKit. consumed is TRUE whenever a visible prompt owns the key.
 */
gboolean mux_ui_pane_bridge_handle_key(MuxUiPaneBridge *bridge,
                                       MuxPaneOverlayKey key,
                                       gunichar text,
                                       gboolean *consumed,
                                       GError **error);

/*
 * Call from the pane's monotonic timer. resolved is TRUE when a prompt timed
 * out and a response was sent.
 */
gboolean mux_ui_pane_bridge_tick(MuxUiPaneBridge *bridge,
                                 gint64 monotonic_us,
                                 gboolean *resolved,
                                 GError **error);

gboolean mux_ui_pane_bridge_set_size(MuxUiPaneBridge *bridge,
                                     guint columns,
                                     guint rows,
                                     GError **error);

gboolean mux_ui_pane_bridge_repaint(MuxUiPaneBridge *bridge,
                                    gint64 monotonic_us,
                                    GError **error);

gboolean mux_ui_pane_bridge_is_active(const MuxUiPaneBridge *bridge);
guint mux_ui_pane_bridge_pending_count(const MuxUiPaneBridge *bridge);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiPaneBridge,
                              mux_ui_pane_bridge_free)

G_END_DECLS

#endif
