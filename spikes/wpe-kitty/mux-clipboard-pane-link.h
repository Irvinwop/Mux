#ifndef MUX_CLIPBOARD_PANE_LINK_H
#define MUX_CLIPBOARD_PANE_LINK_H

#include "mux-clipboard-wire.h"
#include "mux-kitty-clipboard.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardPaneLink MuxClipboardPaneLink;

typedef gboolean (*MuxClipboardPaneTerminalOutputFunc)(
    MuxClipboardPaneLink *link,
    GBytes *packet,
    gpointer user_data,
    GError **error);

/* packet is borrowed and should be sent on the engine extension channel. */
typedef gboolean (*MuxClipboardPaneWireOutputFunc)(
    MuxClipboardPaneLink *link,
    GBytes *packet,
    gpointer user_data,
    GError **error);

/* Used by the pane's muxd client to submit centrally observed snapshots. */
typedef void (*MuxClipboardPaneObserveFunc)(
    MuxClipboardPaneLink *link,
    const gchar *profile,
    const gchar *source_origin,
    guint64 source_view_id,
    guint32 flags,
    const MuxClipboardSnapshot *snapshot,
    gpointer user_data);

typedef void (*MuxClipboardPaneFailureFunc)(
    MuxClipboardPaneLink *link,
    const gchar *operation,
    const GError *error,
    gpointer user_data);

MuxClipboardPaneLink *mux_clipboard_pane_link_new(
    const gchar *profile,
    gboolean ephemeral,
    guint64 view_id,
    MuxClipboardPaneTerminalOutputFunc terminal_output_func,
    MuxClipboardPaneWireOutputFunc wire_output_func,
    MuxClipboardPaneObserveFunc observe_func,
    MuxClipboardPaneFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);
void mux_clipboard_pane_link_free(MuxClipboardPaneLink *link);

void mux_clipboard_pane_link_set_view_id(MuxClipboardPaneLink *link,
                                         guint64 view_id);
gboolean mux_clipboard_pane_link_set_enabled(MuxClipboardPaneLink *link,
                                             gboolean enabled,
                                             GError **error);
gboolean mux_clipboard_pane_link_handle_support(
    MuxClipboardPaneLink *link,
    const guint8 *sequence,
    gsize length,
    GError **error);
gboolean mux_clipboard_pane_link_handle_osc(MuxClipboardPaneLink *link,
                                            const guint8 *sequence,
                                            gsize length,
                                            GError **error);
gboolean mux_clipboard_pane_link_handle_packet(
    MuxClipboardPaneLink *link,
    const guint8 *packet,
    gsize packet_length,
    GError **error);

gboolean mux_clipboard_pane_link_apply_history(
    MuxClipboardPaneLink *link,
    const MuxClipboardSnapshot *snapshot,
    const gchar *source_origin,
    guint64 source_view_id,
    gboolean paste,
    GError **error);

guint mux_clipboard_pane_link_tick(MuxClipboardPaneLink *link,
                                   gint64 monotonic_us);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardPaneLink,
                              mux_clipboard_pane_link_free)

G_END_DECLS

#endif
