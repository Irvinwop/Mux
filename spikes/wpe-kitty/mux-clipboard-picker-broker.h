#pragma once

#include "mux-clipboard-picker-controller.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardPickerBroker MuxClipboardPickerBroker;

typedef gboolean (*MuxClipboardPickerBrokerListFunc)(gpointer client,
                                                     GError **error);
typedef gboolean (*MuxClipboardPickerBrokerSelectFunc)(gpointer client,
                                                       guint64 entry_id,
                                                       GError **error);
typedef gboolean (*MuxClipboardPickerBrokerSetPinnedFunc)(gpointer client,
                                                          guint64 entry_id,
                                                          gboolean pinned,
                                                          GError **error);
typedef gboolean (*MuxClipboardPickerBrokerDeleteFunc)(gpointer client,
                                                       guint64 entry_id,
                                                       GError **error);
typedef gboolean (*MuxClipboardPickerBrokerClearFunc)(gpointer client,
                                                      GError **error);
typedef void (*MuxClipboardPickerBrokerCancelFunc)(gpointer client);

typedef struct {
    MuxClipboardPickerBrokerListFunc list;
    MuxClipboardPickerBrokerSelectFunc select;
    MuxClipboardPickerBrokerSetPinnedFunc set_pinned;
    MuxClipboardPickerBrokerDeleteFunc delete_entry;
    MuxClipboardPickerBrokerClearFunc clear;
    MuxClipboardPickerBrokerCancelFunc cancel;
} MuxClipboardPickerBrokerOps;

/* snapshot is the broker client's complete immutable multi-MIME snapshot. */
typedef gboolean (*MuxClipboardPickerBrokerApplyFunc)(gpointer snapshot,
                                                      gpointer user_data,
                                                      GError **error);
typedef void (*MuxClipboardPickerBrokerNotifyFunc)(
    MuxClipboardPickerBroker *broker,
    gpointer user_data);

MuxClipboardPickerBroker *mux_clipboard_picker_broker_new(
    const gchar *profile,
    gpointer client,
    const MuxClipboardPickerBrokerOps *ops,
    GDestroyNotify client_destroy,
    MuxClipboardPickerBrokerApplyFunc apply_func,
    gpointer apply_data,
    GDestroyNotify apply_destroy,
    MuxClipboardPickerBrokerNotifyFunc changed_func,
    MuxClipboardPickerBrokerNotifyFunc closed_func,
    gpointer notify_data,
    GDestroyNotify notify_destroy);
MuxClipboardPickerBroker *mux_clipboard_picker_broker_ref(
    MuxClipboardPickerBroker *broker);
void mux_clipboard_picker_broker_unref(MuxClipboardPickerBroker *broker);

void mux_clipboard_picker_broker_open(MuxClipboardPickerBroker *broker);
void mux_clipboard_picker_broker_close(MuxClipboardPickerBroker *broker);
MuxClipboardPickerControllerState mux_clipboard_picker_broker_get_state(
    const MuxClipboardPickerBroker *broker);
gboolean mux_clipboard_picker_broker_handle_key(
    MuxClipboardPickerBroker *broker,
    MuxClipboardPickerKey key,
    gunichar text);
gchar *mux_clipboard_picker_broker_render(MuxClipboardPickerBroker *broker,
                                         guint terminal_columns,
                                         guint terminal_rows);

/* Broker-client callbacks feed the currently active list transaction. */
gboolean mux_clipboard_picker_broker_add_summary(
    MuxClipboardPickerBroker *broker,
    guint64 id,
    gint64 created_us,
    const gchar *origin,
    guint64 source_view_id,
    gboolean pinned,
    gsize total_size,
    const gchar *preview,
    const gchar *const *mime_types,
    gsize mime_type_count,
    GError **error);
gboolean mux_clipboard_picker_broker_add_summary_full(
    MuxClipboardPickerBroker *broker,
    guint64 id,
    gint64 created_us,
    const gchar *origin,
    guint64 source_view_id,
    gboolean pinned,
    gsize total_size,
    const gchar *preview,
    const gchar *const *mime_types,
    gsize mime_type_count,
    gsize format_count,
    GError **error);
void mux_clipboard_picker_broker_complete_list(
    MuxClipboardPickerBroker *broker,
    const GError *error);

/* Selection completes only after apply_func accepts the full snapshot. */
void mux_clipboard_picker_broker_complete_selection(
    MuxClipboardPickerBroker *broker,
    gpointer snapshot,
    const GError *error);
void mux_clipboard_picker_broker_complete_mutation(
    MuxClipboardPickerBroker *broker,
    const GError *error);
void mux_clipboard_picker_broker_fail_active(
    MuxClipboardPickerBroker *broker,
    const GError *error);

G_END_DECLS
