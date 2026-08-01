#pragma once

#include "mux-clipboard-picker.h"

G_BEGIN_DECLS

typedef struct _MuxClipboardPickerController MuxClipboardPickerController;

typedef enum {
    MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED,
    MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING,
    MUX_CLIPBOARD_PICKER_CONTROLLER_READY,
    MUX_CLIPBOARD_PICKER_CONTROLLER_SELECTING,
    MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING,
} MuxClipboardPickerControllerState;

typedef gboolean (*MuxClipboardPickerListFunc)(guint64 serial,
                                               gpointer user_data,
                                               GError **error);
typedef gboolean (*MuxClipboardPickerSelectFunc)(guint64 serial,
                                                 guint64 entry_id,
                                                 gpointer user_data,
                                                 GError **error);
typedef gboolean (*MuxClipboardPickerSetPinnedFunc)(guint64 serial,
                                                    guint64 entry_id,
                                                    gboolean pinned,
                                                    gpointer user_data,
                                                    GError **error);
typedef gboolean (*MuxClipboardPickerDeleteFunc)(guint64 serial,
                                                 guint64 entry_id,
                                                 gpointer user_data,
                                                 GError **error);
typedef gboolean (*MuxClipboardPickerClearFunc)(guint64 serial,
                                                gpointer user_data,
                                                GError **error);
typedef void (*MuxClipboardPickerCancelFunc)(guint64 serial,
                                             gpointer user_data);

typedef struct {
    MuxClipboardPickerListFunc list;
    MuxClipboardPickerSelectFunc select;
    MuxClipboardPickerSetPinnedFunc set_pinned;
    MuxClipboardPickerDeleteFunc delete_entry;
    MuxClipboardPickerClearFunc clear;
    MuxClipboardPickerCancelFunc cancel;
} MuxClipboardPickerBackend;

typedef void (*MuxClipboardPickerControllerNotifyFunc)(
    MuxClipboardPickerController *controller,
    gpointer user_data);

MuxClipboardPickerController *mux_clipboard_picker_controller_new(
    const gchar *profile,
    const MuxClipboardPickerBackend *backend,
    gpointer backend_data,
    GDestroyNotify backend_destroy,
    MuxClipboardPickerControllerNotifyFunc changed_func,
    MuxClipboardPickerControllerNotifyFunc closed_func,
    gpointer notify_data,
    GDestroyNotify notify_destroy);
MuxClipboardPickerController *mux_clipboard_picker_controller_ref(
    MuxClipboardPickerController *controller);
void mux_clipboard_picker_controller_unref(
    MuxClipboardPickerController *controller);

void mux_clipboard_picker_controller_open(
    MuxClipboardPickerController *controller);
void mux_clipboard_picker_controller_close(
    MuxClipboardPickerController *controller);

MuxClipboardPickerControllerState mux_clipboard_picker_controller_get_state(
    const MuxClipboardPickerController *controller);
guint64 mux_clipboard_picker_controller_get_active_serial(
    const MuxClipboardPickerController *controller);

/*
 * Broker adapters complete only the matching serial. Older responses are
 * ignored after close, reopen, cancellation, or a newer request.
 */
void mux_clipboard_picker_controller_complete_list(
    MuxClipboardPickerController *controller,
    guint64 serial,
    GPtrArray *items,
    const GError *error);
void mux_clipboard_picker_controller_complete_request(
    MuxClipboardPickerController *controller,
    guint64 serial,
    const GError *error);

gboolean mux_clipboard_picker_controller_handle_key(
    MuxClipboardPickerController *controller,
    MuxClipboardPickerKey key,
    gunichar text);
gchar *mux_clipboard_picker_controller_render(
    MuxClipboardPickerController *controller,
    guint terminal_columns,
    guint terminal_rows);

G_END_DECLS
