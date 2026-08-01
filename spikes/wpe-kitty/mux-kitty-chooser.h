#ifndef MUX_KITTY_CHOOSER_H
#define MUX_KITTY_CHOOSER_H

#include "mux-ui-protocol.h"

G_BEGIN_DECLS

#define MUX_KITTY_CHOOSER_MAX_PENDING 4U

typedef struct _MuxKittyChooser MuxKittyChooser;

typedef gboolean (*MuxKittyChooserSendFunc)(GBytes *payload,
                                            gpointer user_data,
                                            GError **error);

/*
 * suspend_func must disable the pane's TTY input watch, leave raw mode, and
 * hide its graphics placement. resume_func restores those resources and
 * repaints the latest browser frame.
 */
typedef gboolean (*MuxKittyChooserStateFunc)(gpointer user_data,
                                             GError **error);

MuxKittyChooser *mux_kitty_chooser_new(
    MuxKittyChooserSendFunc send_func,
    MuxKittyChooserStateFunc suspend_func,
    MuxKittyChooserStateFunc resume_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_kitty_chooser_free(MuxKittyChooser *chooser);

/*
 * Copies and queues a FILE_CHOOSER request. Other request kinds are rejected.
 * A full queue is answered immediately with UNSUPPORTED.
 */
gboolean mux_kitty_chooser_handle_request(MuxKittyChooser *chooser,
                                          const MuxUiRequest *request,
                                          GError **error);

/*
 * Cancels a request because the engine invalidated it. No response is sent.
 */
gboolean mux_kitty_chooser_cancel(MuxKittyChooser *chooser,
                                  guint64 request_id);
void mux_kitty_chooser_cancel_all(MuxKittyChooser *chooser);

/*
 * Expires queued or active requests using monotonic microseconds. Call this
 * from the pane's existing timer.
 */
guint mux_kitty_chooser_tick(MuxKittyChooser *chooser,
                             gint64 monotonic_us);

gboolean mux_kitty_chooser_is_busy(const MuxKittyChooser *chooser);
guint mux_kitty_chooser_pending_count(const MuxKittyChooser *chooser);

G_END_DECLS

#endif
