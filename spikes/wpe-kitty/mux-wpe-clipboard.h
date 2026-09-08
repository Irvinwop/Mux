#ifndef MUX_WPE_CLIPBOARD_H
#define MUX_WPE_CLIPBOARD_H

#include "mux-clipboard.h"

#include <wpe/wpe-platform.h>

G_BEGIN_DECLS

#define MUX_TYPE_WPE_CLIPBOARD (mux_wpe_clipboard_get_type())
G_DECLARE_FINAL_TYPE(MuxWpeClipboard,
                     mux_wpe_clipboard,
                     MUX,
                     WPE_CLIPBOARD,
                     WPEClipboard)

/* Captures opaque publication state before WPE processes a local change. */
typedef gpointer (*MuxWpeClipboardPublishBeginFunc)(
    MuxWpeClipboard *clipboard,
    gpointer user_data);

/*
 * Publication is deferred to the main context captured at construction, after
 * the local snapshot has been cached. snapshot is borrowed and sealed for the
 * duration of the callback; publication_data retains the state captured by
 * publish_begin_func, even when focus changes before dispatch. Its destroy
 * callback runs exactly once after publication or cancellation.
 */
typedef void (*MuxWpeClipboardPublishFunc)(MuxWpeClipboard *clipboard,
                                          MuxClipboardSnapshot *snapshot,
                                          gpointer publication_data,
                                          gpointer user_data);

/*
 * Each instance has one publication recipient, fixed at construction. Creating
 * another clipboard for the same display does not replace this recipient or
 * share its publication queue.
 */
MuxWpeClipboard *mux_wpe_clipboard_new(
    WPEDisplay *display,
    MuxWpeClipboardPublishBeginFunc publish_begin_func,
    MuxWpeClipboardPublishFunc publish_func,
    GDestroyNotify publication_data_destroy,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

/*
 * Permanently disable publication and destroy queued publication state without
 * dispatching it. Call on the constructing main context before releasing the
 * publication recipient. This affects only this instance and is idempotent;
 * an instance cannot be rebound to a replacement recipient. An already-running
 * publication may finish; cached clipboard reads and changes remain available.
 */
void mux_wpe_clipboard_stop_publishing(MuxWpeClipboard *clipboard);

void mux_wpe_clipboard_set_external(MuxWpeClipboard *clipboard,
                                    const MuxClipboardSnapshot *snapshot);

void mux_wpe_clipboard_clear_external(MuxWpeClipboard *clipboard,
                                      guint64 serial);

const MuxClipboardSnapshot *mux_wpe_clipboard_get_external(
    MuxWpeClipboard *clipboard);

G_END_DECLS

#endif
