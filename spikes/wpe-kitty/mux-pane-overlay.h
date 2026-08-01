#ifndef MUX_PANE_OVERLAY_H
#define MUX_PANE_OVERLAY_H

#include "mux-ui-protocol.h"

G_BEGIN_DECLS

#define MUX_PANE_OVERLAY_MAX_PENDING 8U
#define MUX_PANE_OVERLAY_ROWS 5U

typedef struct _MuxPaneOverlay MuxPaneOverlay;

typedef enum {
    MUX_PANE_OVERLAY_KEY_TEXT,
    MUX_PANE_OVERLAY_KEY_ENTER,
    MUX_PANE_OVERLAY_KEY_ESCAPE,
    MUX_PANE_OVERLAY_KEY_BACKSPACE,
    MUX_PANE_OVERLAY_KEY_UP,
    MUX_PANE_OVERLAY_KEY_DOWN,
    MUX_PANE_OVERLAY_KEY_TAB,
} MuxPaneOverlayKey;

MuxPaneOverlay *mux_pane_overlay_new(void);
void mux_pane_overlay_free(MuxPaneOverlay *overlay);

/*
 * Takes ownership of request on success. The caller retains ownership on
 * failure.
 */
gboolean mux_pane_overlay_push(MuxPaneOverlay *overlay,
                               MuxUiRequest *request,
                               GError **error);

gboolean mux_pane_overlay_cancel(MuxPaneOverlay *overlay, guint64 request_id);
void mux_pane_overlay_clear(MuxPaneOverlay *overlay);

gboolean mux_pane_overlay_contains(const MuxPaneOverlay *overlay,
                                   guint64 request_id);
gboolean mux_pane_overlay_is_active(const MuxPaneOverlay *overlay);
const MuxUiRequest *mux_pane_overlay_active(const MuxPaneOverlay *overlay);
guint mux_pane_overlay_pending_count(const MuxPaneOverlay *overlay);

/*
 * Returns TRUE when an active overlay consumed the key. When the key resolves
 * the request, response receives a newly allocated response owned by caller.
 */
gboolean mux_pane_overlay_handle_key(MuxPaneOverlay *overlay,
                                     MuxPaneOverlayKey key,
                                     gunichar text,
                                     MuxUiResponse **response);

/*
 * Resolves one expired request, if any, using its fail-closed action.
 */
gboolean mux_pane_overlay_tick(MuxPaneOverlay *overlay,
                               gint64 monotonic_us,
                               MuxUiResponse **response);

/*
 * Returns an ANSI synchronized-update sequence that paints the active prompt
 * over the bottom terminal rows. Web graphics should use a negative z-index.
 */
gchar *mux_pane_overlay_render(const MuxPaneOverlay *overlay,
                               guint columns,
                               guint rows,
                               gint64 monotonic_us);

gchar *mux_pane_overlay_render_clear(guint rows);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxPaneOverlay, mux_pane_overlay_free)

G_END_DECLS

#endif
