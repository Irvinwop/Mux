#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _MuxdClipboard MuxdClipboard;

MuxdClipboard *muxd_clipboard_new(GMainContext *context,
                                  GError **error);
void muxd_clipboard_free(MuxdClipboard *clipboard);

G_END_DECLS
