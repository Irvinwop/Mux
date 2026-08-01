#pragma once

#include "mux-clipboard-broker-bindings.h"
#include "mux-clipboard-pane-link.h"
#include "mux-clipboard-picker-broker.h"

G_BEGIN_DECLS

typedef struct _MuxPaneClipboard MuxPaneClipboard;

typedef gboolean (*MuxPaneClipboardOutputFunc)(MuxPaneClipboard *clipboard,
                                               GBytes *packet,
                                               gpointer user_data,
                                               GError **error);
typedef void (*MuxPaneClipboardNotifyFunc)(MuxPaneClipboard *clipboard,
                                          gpointer user_data);
typedef void (*MuxPaneClipboardFailureFunc)(MuxPaneClipboard *clipboard,
                                           const gchar *operation,
                                           const GError *error,
                                           gpointer user_data);

MuxPaneClipboard *mux_pane_clipboard_new(
    const gchar *profile,
    gboolean ephemeral,
    guint64 view_id,
    GMainContext *context,
    MuxPaneClipboardOutputFunc terminal_output_func,
    MuxPaneClipboardOutputFunc wire_output_func,
    MuxPaneClipboardNotifyFunc changed_func,
    MuxPaneClipboardNotifyFunc closed_func,
    MuxPaneClipboardFailureFunc failure_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error);
void mux_pane_clipboard_free(MuxPaneClipboard *clipboard);

gboolean mux_pane_clipboard_set_enabled(MuxPaneClipboard *clipboard,
                                        gboolean enabled,
                                        GError **error);
gboolean mux_pane_clipboard_handle_support(MuxPaneClipboard *clipboard,
                                           const guint8 *sequence,
                                           gsize length,
                                           GError **error);
gboolean mux_pane_clipboard_handle_osc(MuxPaneClipboard *clipboard,
                                       const guint8 *sequence,
                                       gsize length,
                                       GError **error);
gboolean mux_pane_clipboard_handle_engine_packet(
    MuxPaneClipboard *clipboard,
    const guint8 *packet,
    gsize packet_length,
    GError **error);

void mux_pane_clipboard_open_picker(MuxPaneClipboard *clipboard);
void mux_pane_clipboard_close_picker(MuxPaneClipboard *clipboard);
gboolean mux_pane_clipboard_picker_is_open(
    const MuxPaneClipboard *clipboard);
gboolean mux_pane_clipboard_handle_picker_key(
    MuxPaneClipboard *clipboard,
    MuxClipboardPickerKey key,
    gunichar text);
gchar *mux_pane_clipboard_render_picker(MuxPaneClipboard *clipboard,
                                       guint terminal_columns,
                                       guint terminal_rows);

void mux_pane_clipboard_tick(MuxPaneClipboard *clipboard,
                             gint64 monotonic_us);

G_END_DECLS
