#ifndef MUX_CLIPBOARD_ENGINE_LINK_H
#define MUX_CLIPBOARD_ENGINE_LINK_H

#include "mux-clipboard-wire.h"
#include "mux-wpe-clipboard.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardEngineLink MuxClipboardEngineLink;
typedef struct _MuxClipboardEngineWrite MuxClipboardEngineWrite;

/* packet is borrowed and should be sent on the pane extension channel. */
typedef gboolean (*MuxClipboardEngineOutputFunc)(
    MuxClipboardEngineLink *link,
    GBytes *packet,
    gpointer user_data,
    GError **error);

/* Called after the WPE cache contains the paste snapshot. */
typedef void (*MuxClipboardEnginePasteFunc)(
    MuxClipboardEngineLink *link,
    guint64 target_view_id,
    const MuxClipboardSnapshot *snapshot,
    gpointer user_data);

typedef void (*MuxClipboardEngineFailureFunc)(
    MuxClipboardEngineLink *link,
    const gchar *operation,
    const GError *error,
    gpointer user_data);

MuxClipboardEngineLink *mux_clipboard_engine_link_new(
    WPEDisplay *display,
    const gchar *profile,
    gboolean ephemeral,
    MuxClipboardEngineOutputFunc output_func,
    MuxClipboardEnginePasteFunc paste_func,
    MuxClipboardEngineFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);
void mux_clipboard_engine_link_free(MuxClipboardEngineLink *link);

WPEClipboard *mux_clipboard_engine_link_get_clipboard(
    MuxClipboardEngineLink *link);

gboolean mux_clipboard_engine_link_set_active_source(
    MuxClipboardEngineLink *link,
    guint64 view_id,
    const gchar *origin,
    gboolean ephemeral,
    GError **error);

MuxClipboardEngineWrite *mux_clipboard_engine_link_begin_write(
    MuxClipboardEngineLink *link);
gboolean mux_clipboard_engine_link_complete_write(
    MuxClipboardEngineLink *link,
    MuxClipboardEngineWrite *write,
    const MuxClipboardSnapshot *snapshot,
    GError **error);
void mux_clipboard_engine_write_free(MuxClipboardEngineWrite *write);

gboolean mux_clipboard_engine_link_handle_packet(
    MuxClipboardEngineLink *link,
    const guint8 *packet,
    gsize packet_length,
    GError **error);

gboolean mux_clipboard_engine_link_tick(MuxClipboardEngineLink *link,
                                        gint64 monotonic_us);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardEngineLink,
                              mux_clipboard_engine_link_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardEngineWrite,
                              mux_clipboard_engine_write_free)

G_END_DECLS

#endif
