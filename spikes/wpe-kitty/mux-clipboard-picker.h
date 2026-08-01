#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MuxClipboardPickerItem MuxClipboardPickerItem;
typedef struct _MuxClipboardPicker MuxClipboardPicker;

typedef enum {
    MUX_CLIPBOARD_PICKER_KEY_TEXT,
    MUX_CLIPBOARD_PICKER_KEY_BACKSPACE,
    MUX_CLIPBOARD_PICKER_KEY_DELETE_WORD,
    MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY,
    MUX_CLIPBOARD_PICKER_KEY_UP,
    MUX_CLIPBOARD_PICKER_KEY_DOWN,
    MUX_CLIPBOARD_PICKER_KEY_PAGE_UP,
    MUX_CLIPBOARD_PICKER_KEY_PAGE_DOWN,
    MUX_CLIPBOARD_PICKER_KEY_HOME,
    MUX_CLIPBOARD_PICKER_KEY_END,
    MUX_CLIPBOARD_PICKER_KEY_ENTER,
    MUX_CLIPBOARD_PICKER_KEY_ESCAPE,
    MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN,
    MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY,
    MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY,
} MuxClipboardPickerKey;

typedef enum {
    MUX_CLIPBOARD_PICKER_ACTION_NONE,
    MUX_CLIPBOARD_PICKER_ACTION_SELECT,
    MUX_CLIPBOARD_PICKER_ACTION_CLOSE,
    MUX_CLIPBOARD_PICKER_ACTION_SET_PINNED,
    MUX_CLIPBOARD_PICKER_ACTION_DELETE,
    MUX_CLIPBOARD_PICKER_ACTION_CLEAR,
} MuxClipboardPickerActionKind;

typedef struct {
    MuxClipboardPickerActionKind kind;
    guint64 entry_id;
    gboolean pinned;
} MuxClipboardPickerAction;

MuxClipboardPickerItem *mux_clipboard_picker_item_new(
    guint64 id,
    gint64 created_us,
    const gchar *origin,
    guint64 source_view_id,
    gboolean pinned,
    gsize total_size,
    const gchar *preview,
    const gchar *const *mime_types,
    gsize mime_type_count);
MuxClipboardPickerItem *mux_clipboard_picker_item_ref(
    MuxClipboardPickerItem *item);
void mux_clipboard_picker_item_unref(MuxClipboardPickerItem *item);

guint64 mux_clipboard_picker_item_get_id(
    const MuxClipboardPickerItem *item);
gboolean mux_clipboard_picker_item_get_pinned(
    const MuxClipboardPickerItem *item);

MuxClipboardPicker *mux_clipboard_picker_new(const gchar *profile);
void mux_clipboard_picker_free(MuxClipboardPicker *picker);

/* The picker takes references to every MuxClipboardPickerItem in items. */
void mux_clipboard_picker_set_items(MuxClipboardPicker *picker,
                                    GPtrArray *items);
void mux_clipboard_picker_set_query(MuxClipboardPicker *picker,
                                    const gchar *query);
void mux_clipboard_picker_set_status(MuxClipboardPicker *picker,
                                     const gchar *status);
const gchar *mux_clipboard_picker_get_query(
    const MuxClipboardPicker *picker);
guint mux_clipboard_picker_get_match_count(
    const MuxClipboardPicker *picker);
const MuxClipboardPickerItem *mux_clipboard_picker_get_selected(
    const MuxClipboardPicker *picker);

/* text is used only for MUX_CLIPBOARD_PICKER_KEY_TEXT. */
gboolean mux_clipboard_picker_handle_key(
    MuxClipboardPicker *picker,
    MuxClipboardPickerKey key,
    gunichar text,
    MuxClipboardPickerAction *out_action);

/* Returns a complete ANSI panel without moving or saving the cursor. */
gchar *mux_clipboard_picker_render(MuxClipboardPicker *picker,
                                  guint terminal_columns,
                                  guint terminal_rows);

G_END_DECLS
