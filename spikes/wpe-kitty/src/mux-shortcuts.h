#ifndef MUX_SHORTCUTS_H
#define MUX_SHORTCUTS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    MUX_SHORTCUT_MODIFIER_CONTROL = 1u << 0,
    MUX_SHORTCUT_MODIFIER_SHIFT = 1u << 1,
    MUX_SHORTCUT_MODIFIER_ALT = 1u << 2,
    MUX_SHORTCUT_MODIFIER_META = 1u << 3,
} MuxShortcutModifiers;

typedef enum {
    MUX_SHORTCUT_NONE = 0,
    MUX_SHORTCUT_CLIPBOARD_HISTORY,
    MUX_SHORTCUT_CLOSE,
    MUX_SHORTCUT_LOCATION,
    MUX_SHORTCUT_RELOAD,
    MUX_SHORTCUT_COMMAND_PALETTE,
    MUX_SHORTCUT_BOOKMARK,
    MUX_SHORTCUT_HISTORY_BACK,
    MUX_SHORTCUT_HISTORY_FORWARD,
    MUX_SHORTCUT_PICKER_DELETE_WORD,
    MUX_SHORTCUT_PICKER_CLEAR_QUERY,
    MUX_SHORTCUT_PICKER_TOGGLE_PIN,
    MUX_SHORTCUT_PICKER_DELETE_ENTRY,
    MUX_SHORTCUT_PICKER_CLEAR_HISTORY,
    MUX_SHORTCUT_BAR_CLEAR,
} MuxShortcut;

typedef enum {
    MUX_SHORTCUT_EVENT_PRESS = 1,
    MUX_SHORTCUT_EVENT_REPEAT = 2,
    MUX_SHORTCUT_EVENT_RELEASE = 3,
} MuxShortcutEventType;

guint mux_shortcut_modifiers_from_kitty(guint encoded_modifiers);
gboolean mux_shortcut_key_is_kitty_functional(guint keyval);
MuxShortcut mux_shortcut_match_pane(guint modifiers, guint keyval);
MuxShortcut mux_shortcut_match_engine(guint modifiers, guint keyval);
MuxShortcut mux_shortcut_match_picker(guint modifiers, guint keyval);
MuxShortcut mux_shortcut_match_bar(guint modifiers, guint keyval);
gboolean mux_shortcut_is_page_paste(guint modifiers, guint keyval);
gboolean mux_shortcut_handle_event(MuxShortcut shortcut,
                                   guint event_type,
                                   gboolean *execute);

G_END_DECLS

#endif
