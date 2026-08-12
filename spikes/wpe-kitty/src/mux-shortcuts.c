#include "mux-shortcuts.h"

#define MUX_SHORTCUT_KEY_LEFT 0xff51u
#define MUX_SHORTCUT_KEY_RIGHT 0xff53u
#define MUX_SHORTCUT_KITTY_FUNCTIONAL_FIRST 57344u
#define MUX_SHORTCUT_KITTY_FUNCTIONAL_LAST 63743u

static guint
normalized_ascii_key(guint keyval)
{
    if (keyval >= 'A' && keyval <= 'Z')
        return keyval + ('a' - 'A');
    return keyval;
}

static gboolean
modifiers_are(guint modifiers, guint expected)
{
    const guint keyboard_modifiers =
        MUX_SHORTCUT_MODIFIER_CONTROL |
        MUX_SHORTCUT_MODIFIER_SHIFT |
        MUX_SHORTCUT_MODIFIER_ALT |
        MUX_SHORTCUT_MODIFIER_META;

    return (modifiers & keyboard_modifiers) == expected;
}

static gboolean
command_modifiers_are(guint modifiers, guint additional)
{
    return modifiers_are(modifiers,
                         MUX_SHORTCUT_MODIFIER_META | additional) ||
        modifiers_are(modifiers,
                      MUX_SHORTCUT_MODIFIER_CONTROL | additional);
}

guint
mux_shortcut_modifiers_from_kitty(guint encoded_modifiers)
{
    guint kitty = encoded_modifiers ? encoded_modifiers - 1 : 0;
    guint modifiers = 0;

    if (kitty & 1)
        modifiers |= MUX_SHORTCUT_MODIFIER_SHIFT;
    if (kitty & 2)
        modifiers |= MUX_SHORTCUT_MODIFIER_ALT;
    if (kitty & 4)
        modifiers |= MUX_SHORTCUT_MODIFIER_CONTROL;
    if (kitty & (8 | 32))
        modifiers |= MUX_SHORTCUT_MODIFIER_META;
    return modifiers;
}

gboolean
mux_shortcut_key_is_kitty_functional(guint keyval)
{
    return keyval >= MUX_SHORTCUT_KITTY_FUNCTIONAL_FIRST &&
        keyval <= MUX_SHORTCUT_KITTY_FUNCTIONAL_LAST;
}

MuxShortcut
mux_shortcut_match_pane(guint modifiers, guint keyval)
{
    guint key = normalized_ascii_key(keyval);

    if (key == 'v' &&
        command_modifiers_are(modifiers, MUX_SHORTCUT_MODIFIER_SHIFT))
        return MUX_SHORTCUT_CLIPBOARD_HISTORY;
    if (!command_modifiers_are(modifiers, 0))
        return MUX_SHORTCUT_NONE;
    if (key == 'q' || key == 'w')
        return MUX_SHORTCUT_CLOSE;
    if (key == 'l')
        return MUX_SHORTCUT_LOCATION;
    if (key == 'r')
        return MUX_SHORTCUT_RELOAD;
    return MUX_SHORTCUT_NONE;
}

MuxShortcut
mux_shortcut_match_engine(guint modifiers, guint keyval)
{
    guint key = normalized_ascii_key(keyval);

    if (key == 'p' &&
        command_modifiers_are(modifiers, MUX_SHORTCUT_MODIFIER_SHIFT))
        return MUX_SHORTCUT_COMMAND_PALETTE;
    if (key == 'd' && command_modifiers_are(modifiers, 0))
        return MUX_SHORTCUT_BOOKMARK;
    if (modifiers_are(modifiers, MUX_SHORTCUT_MODIFIER_ALT)) {
        if (keyval == MUX_SHORTCUT_KEY_LEFT)
            return MUX_SHORTCUT_HISTORY_BACK;
        if (keyval == MUX_SHORTCUT_KEY_RIGHT)
            return MUX_SHORTCUT_HISTORY_FORWARD;
    }
    return MUX_SHORTCUT_NONE;
}

MuxShortcut
mux_shortcut_match_picker(guint modifiers, guint keyval)
{
    guint key = normalized_ascii_key(keyval);

    if (command_modifiers_are(modifiers, 0)) {
        if (key == 'w')
            return MUX_SHORTCUT_PICKER_DELETE_WORD;
        if (key == 'u')
            return MUX_SHORTCUT_PICKER_CLEAR_QUERY;
    }
    if (modifiers_are(modifiers, MUX_SHORTCUT_MODIFIER_ALT)) {
        if (key == 'p')
            return MUX_SHORTCUT_PICKER_TOGGLE_PIN;
        if (key == 'd')
            return MUX_SHORTCUT_PICKER_DELETE_ENTRY;
        if (key == 'c')
            return MUX_SHORTCUT_PICKER_CLEAR_HISTORY;
    }
    return MUX_SHORTCUT_NONE;
}

MuxShortcut
mux_shortcut_match_bar(guint modifiers, guint keyval)
{
    guint key = normalized_ascii_key(keyval);

    if (!command_modifiers_are(modifiers, 0))
        return MUX_SHORTCUT_NONE;
    if (key == 'l')
        return MUX_SHORTCUT_LOCATION;
    if (key == 'u')
        return MUX_SHORTCUT_BAR_CLEAR;
    return MUX_SHORTCUT_NONE;
}

gboolean
mux_shortcut_is_page_paste(guint modifiers, guint keyval)
{
    return normalized_ascii_key(keyval) == 'v' &&
        command_modifiers_are(modifiers, 0);
}

gboolean
mux_shortcut_handle_event(MuxShortcut shortcut,
                          guint event_type,
                          gboolean *execute)
{
    g_return_val_if_fail(execute != NULL, FALSE);

    *execute = shortcut != MUX_SHORTCUT_NONE &&
        event_type == MUX_SHORTCUT_EVENT_PRESS;
    return shortcut != MUX_SHORTCUT_NONE;
}
