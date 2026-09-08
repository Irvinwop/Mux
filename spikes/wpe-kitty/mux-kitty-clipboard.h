#ifndef MUX_KITTY_CLIPBOARD_H
#define MUX_KITTY_CLIPBOARD_H

#include "mux-clipboard.h"
#include "mux-osc5522.h"

G_BEGIN_DECLS

#define MUX_KITTY_CLIPBOARD_TIMEOUT_MS 10000U
#define MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK 16U
#define MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK (96U * 1024U)
#define MUX_KITTY_CLIPBOARD_MAX_QUEUED_WRITES 1U

/*
 * Writes retain one active snapshot and one replacement snapshot. If another
 * snapshot is published while that replacement is queued, the newer snapshot
 * replaces it. Snapshot byte and MIME limits remain those in mux-clipboard.h.
 */

typedef struct _MuxKittyClipboard MuxKittyClipboard;

/*
 * packet is borrowed and must be accepted atomically. Returning
 * G_IO_ERROR_WOULD_BLOCK means that no bytes were accepted and the complete
 * packet will be retried by a later tick.
 */
typedef gboolean (*MuxKittyClipboardOutputFunc)(MuxKittyClipboard *clipboard,
                                                GBytes *packet,
                                                gpointer user_data,
                                                GError **error);

/* snapshot is borrowed and sealed for the duration of the callback. */
typedef void (*MuxKittyClipboardReceiveFunc)(MuxKittyClipboard *clipboard,
                                             MuxOsc5522Location location,
                                             MuxClipboardSnapshot *snapshot,
                                             gboolean is_paste,
                                             gpointer user_data);

typedef void (*MuxKittyClipboardFailureFunc)(MuxKittyClipboard *clipboard,
                                             const gchar *operation,
                                             const GError *error,
                                             gpointer user_data);

MuxKittyClipboard *mux_kitty_clipboard_new(
    MuxKittyClipboardOutputFunc output_func,
    MuxKittyClipboardReceiveFunc receive_func,
    MuxKittyClipboardFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

MuxKittyClipboard *mux_kitty_clipboard_ref(MuxKittyClipboard *clipboard);
void mux_kitty_clipboard_unref(MuxKittyClipboard *clipboard);

gboolean mux_kitty_clipboard_set_enabled(MuxKittyClipboard *clipboard,
                                         gboolean enabled,
                                         GError **error);

gboolean mux_kitty_clipboard_request(MuxKittyClipboard *clipboard,
                                     MuxOsc5522Location location,
                                     const gchar *const *mime_types,
                                     const gchar *password,
                                     const gchar *human_name,
                                     gboolean is_paste,
                                     GError **error);

/* Discovers and reads every MIME type currently advertised by Kitty. */
gboolean mux_kitty_clipboard_request_all(MuxKittyClipboard *clipboard,
                                         MuxOsc5522Location location,
                                         const gchar *password,
                                         const gchar *human_name,
                                         gboolean is_paste,
                                         GError **error);

/* Cancels only the current read transaction; late responses are ignored. */
void mux_kitty_clipboard_cancel_read(MuxKittyClipboard *clipboard);

gboolean mux_kitty_clipboard_publish(MuxKittyClipboard *clipboard,
                                     MuxOsc5522Location location,
                                     const MuxClipboardSnapshot *snapshot,
                                     GError **error);

gboolean mux_kitty_clipboard_handle_support(MuxKittyClipboard *clipboard,
                                            const guint8 *sequence,
                                            gsize length,
                                            GError **error);

gboolean mux_kitty_clipboard_handle_osc(MuxKittyClipboard *clipboard,
                                        const guint8 *sequence,
                                        gsize length,
                                        GError **error);
gboolean mux_kitty_clipboard_osc_matches_pending_read(
    const MuxKittyClipboard *clipboard,
    const guint8 *sequence,
    gsize length,
    gboolean *matches,
    GError **error);

/*
 * Expires stale transactions and advances an active write by at most
 * MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK packets and
 * MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK encoded bytes.
 */
guint mux_kitty_clipboard_tick(MuxKittyClipboard *clipboard,
                               gint64 monotonic_us);

MuxOsc5522Support mux_kitty_clipboard_get_support(
    const MuxKittyClipboard *clipboard);
gboolean mux_kitty_clipboard_read_pending(const MuxKittyClipboard *clipboard);
gboolean mux_kitty_clipboard_write_pending(const MuxKittyClipboard *clipboard);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxKittyClipboard,
                              mux_kitty_clipboard_unref)

G_END_DECLS

#endif
