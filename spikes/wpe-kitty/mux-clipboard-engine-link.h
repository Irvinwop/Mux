#ifndef MUX_CLIPBOARD_ENGINE_LINK_H
#define MUX_CLIPBOARD_ENGINE_LINK_H

#include "mux-clipboard-wire.h"
#include "mux-wpe-clipboard.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardEngineLink MuxClipboardEngineLink;
typedef struct _MuxClipboardEngineWrite MuxClipboardEngineWrite;

/*
 * packet is borrowed and must be sent on target_view_id's extension channel.
 * The target is captured independently of clipboard provenance and remains
 * fixed for every record in a transaction. Reject an unavailable target;
 * never substitute the currently focused view.
 */
typedef gboolean (*MuxClipboardEngineOutputFunc)(
    MuxClipboardEngineLink *link,
    guint64 target_view_id,
    GBytes *packet,
    gpointer user_data,
    GError **error);

/*
 * Called after the WPE cache contains the paste snapshot. target_view_id is
 * captured from the incoming packet's routing context before cache callbacks
 * run; focus changes must not replace it. The recipient must validate that
 * this exact view is still owned and live, returning FALSE with an error if it
 * cannot paste there. TRUE means the paste was dispatched to that view.
 */
typedef gboolean (*MuxClipboardEnginePasteFunc)(
    MuxClipboardEngineLink *link,
    guint64 target_view_id,
    const MuxClipboardSnapshot *snapshot,
    gpointer user_data,
    GError **error);

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
/*
 * Sends the captured transaction once. A successful return means the complete
 * snapshot was emitted, not that the peer acknowledged it. After sending
 * begins, output failure also consumes the write: partial wire transactions
 * must not be retried with the same identifier. Output callbacks may reenter
 * the link, acknowledge the transaction, or close it.
 */
gboolean mux_clipboard_engine_link_complete_write(
    MuxClipboardEngineLink *link,
    MuxClipboardEngineWrite *write,
    const MuxClipboardSnapshot *snapshot,
    GError **error);
void mux_clipboard_engine_write_free(MuxClipboardEngineWrite *write);

/*
 * The caller must set the trusted incoming view with set_active_source before
 * handing over its packet. A transfer's source_view_id describes provenance,
 * including historical content, and is never used as its paste destination.
 */
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
