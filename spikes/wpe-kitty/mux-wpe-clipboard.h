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

/* snapshot is borrowed and sealed for the duration of the callback. */
typedef void (*MuxWpeClipboardPublishFunc)(MuxWpeClipboard *clipboard,
                                          MuxClipboardSnapshot *snapshot,
                                          gpointer user_data);

MuxWpeClipboard *mux_wpe_clipboard_new(
    WPEDisplay *display,
    MuxWpeClipboardPublishFunc publish_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_wpe_clipboard_set_external(MuxWpeClipboard *clipboard,
                                    const MuxClipboardSnapshot *snapshot);

void mux_wpe_clipboard_clear_external(MuxWpeClipboard *clipboard,
                                      guint64 serial);

const MuxClipboardSnapshot *mux_wpe_clipboard_get_external(
    MuxWpeClipboard *clipboard);

G_END_DECLS

#endif
