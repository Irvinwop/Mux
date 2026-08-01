#ifndef MUX_CLIPBOARD_BROKER_H
#define MUX_CLIPBOARD_BROKER_H

#include "mux-clipboard-history.h"

G_BEGIN_DECLS

#define MUX_CLIPBOARD_BROKER_MAX_PROFILES 32U

typedef struct _MuxClipboardBroker MuxClipboardBroker;

typedef struct {
    guint64 id;
    gint64 created_us;
    gchar *source_origin;
    guint64 source_view_id;
    gboolean pinned;
    guint format_count;
    gsize total_bytes;
    gchar *preview;
} MuxClipboardBrokerSummary;

MuxClipboardBroker *mux_clipboard_broker_new(void);
void mux_clipboard_broker_free(MuxClipboardBroker *broker);

gboolean mux_clipboard_broker_set_profile_mode(
    MuxClipboardBroker *broker,
    const gchar *profile,
    MuxClipboardHistoryMode mode,
    GError **error);
gboolean mux_clipboard_broker_remove_profile(MuxClipboardBroker *broker,
                                             const gchar *profile);

MuxClipboardHistoryAddResult mux_clipboard_broker_observe(
    MuxClipboardBroker *broker,
    const gchar *profile,
    const MuxClipboardSnapshot *snapshot,
    gint64 created_us,
    const gchar *source_origin,
    guint64 source_view_id,
    guint64 *history_entry_id,
    GError **error);

MuxClipboardSnapshot *mux_clipboard_broker_get_current(
    MuxClipboardBroker *broker,
    const gchar *profile,
    GError **error);

GPtrArray *mux_clipboard_broker_list(MuxClipboardBroker *broker,
                                     const gchar *profile,
                                     GError **error);

MuxClipboardSnapshot *mux_clipboard_broker_select(
    MuxClipboardBroker *broker,
    const gchar *profile,
    guint64 entry_id,
    GError **error);

gboolean mux_clipboard_broker_set_pinned(MuxClipboardBroker *broker,
                                         const gchar *profile,
                                         guint64 entry_id,
                                         gboolean pinned,
                                         GError **error);
gboolean mux_clipboard_broker_delete(MuxClipboardBroker *broker,
                                     const gchar *profile,
                                     guint64 entry_id,
                                     GError **error);
guint mux_clipboard_broker_clear(MuxClipboardBroker *broker,
                                 const gchar *profile,
                                 gboolean include_pinned,
                                 GError **error);

void mux_clipboard_broker_summary_free(
    MuxClipboardBrokerSummary *summary);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxClipboardBroker,
                              mux_clipboard_broker_free)

G_END_DECLS

#endif
