#include "mux-bar-render.h"

#include <string.h>

#define BAR_BACKGROUND "\033[48;2;8;22;19m"
#define BAR_MUTED "\033[38;2;154;179;168m"
#define BAR_TEXT "\033[38;2;222;246;232m"
#define BAR_ACCENT "\033[38;2;255;203;102m"

static guint
character_columns(gunichar character)
{
    if (g_unichar_combining_class(character) != 0)
        return 0;
    return g_unichar_iswide(character) ? 2u : 1u;
}

static gchar *
visible_text(const gchar *text)
{
    g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
    GString *clean = g_string_new(NULL);

    for (const gchar *cursor = valid; *cursor;
         cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        GUnicodeType type = g_unichar_type(character);

        if (!g_unichar_isprint(character) ||
            type == G_UNICODE_FORMAT ||
            type == G_UNICODE_LINE_SEPARATOR ||
            type == G_UNICODE_PARAGRAPH_SEPARATOR)
            continue;
        g_string_append_unichar(clean, character);
    }
    return g_string_free(clean, FALSE);
}

static guint
text_columns(const gchar *text)
{
    guint width = 0;

    for (const gchar *cursor = text; *cursor;
         cursor = g_utf8_next_char(cursor))
        width += character_columns(g_utf8_get_char(cursor));
    return width;
}

static guint
append_clipped(GString *output, const gchar *text, guint columns,
               gboolean ellipsis)
{
    guint used = 0;
    gboolean clipped = text_columns(text) > columns;
    guint limit = clipped && ellipsis && columns >= 4 ? columns - 3 : columns;

    for (const gchar *cursor = text; *cursor;
         cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        guint width = character_columns(character);

        if (width > limit - used)
            break;
        g_string_append_unichar(output, character);
        used += width;
    }
    if (clipped && ellipsis && columns >= 4) {
        g_string_append(output, "...");
        used += 3;
    }
    return used;
}

static void
pad_columns(GString *output, guint columns)
{
    for (guint i = 0; i < columns; i++)
        g_string_append_c(output, ' ');
}

static void
append_header(GString *output, const MuxBarRenderState *state)
{
    g_autofree gchar *title = visible_text(state->editing
        ? (state->replace_on_type ? "Replace address" : "Edit address")
        : (state->title && *state->title ? state->title : "No page open"));
    const gchar *hint = "";

    if (state->editing)
        hint = state->columns >= 48 ? "Enter open  Esc cancel" : "Enter / Esc";
    else if (state->columns >= 88)
        hint = "Super-L address  Super-Shift-P commands";
    else if (state->columns >= 52)
        hint = "Super-L address";
    else if (state->columns >= 28)
        hint = "Super-L";

    guint hint_columns = (guint)strlen(hint);
    if (hint_columns + 12 > state->columns) {
        hint = "";
        hint_columns = 0;
    }
    guint left_columns = state->columns - hint_columns;
    guint used = 0;

    g_string_append(output, BAR_BACKGROUND);
    g_string_append(output, state->editing ? BAR_ACCENT : BAR_MUTED);
    if (left_columns > 0) {
        g_string_append_c(output, ' ');
        used = 1 + append_clipped(output, title,
                                  left_columns > 2 ? left_columns - 2 : 0,
                                  TRUE);
    }
    pad_columns(output, left_columns - used);
    g_string_append(output, hint);
}

static guint
append_address(GString *output, const MuxBarRenderState *state)
{
    g_autofree gchar *clean = visible_text(state->editing
        ? state->edit : (state->uri && *state->uri
            ? state->uri : "Open an address with Super-L"));
    const gchar *prefix = state->columns >= 14
        ? (state->editing ? " URL> " : " URL  ")
        : (state->editing ? "> " : "");
    guint prefix_columns = MIN((guint)strlen(prefix), state->columns);
    guint capacity = state->columns - prefix_columns;
    guint used = prefix_columns;
    guint cursor_column;
    const gchar *visible = clean;

    g_string_append(output, state->editing
        ? "\033[48;2;27;57;48m" BAR_TEXT
        : "\033[48;2;17;34;29m" BAR_TEXT);
    g_string_append_len(output, prefix, prefix_columns);

    if (state->editing && capacity > 0) {
        /* The current editor appends at the end. Reserve its cursor cell and
         * scroll the visible tail, never the underlying navigation value. */
        guint budget = capacity - 1;
        guint width = text_columns(visible);
        gboolean scrolled = width > budget;
        gboolean marker = scrolled && budget > 0;

        if (marker)
            budget--;
        while (*visible && width > budget) {
            width -= character_columns(g_utf8_get_char(visible));
            visible = g_utf8_next_char(visible);
        }
        while (*visible && g_unichar_combining_class(g_utf8_get_char(visible)))
            visible = g_utf8_next_char(visible);
        if (marker) {
            g_string_append_c(output, '<');
            used++;
        }
        if (state->replace_on_type)
            g_string_append(output, "\033[7m");
        used += append_clipped(output, visible, budget, FALSE);
        if (state->replace_on_type)
            g_string_append(output, "\033[27m");
    } else {
        used += append_clipped(output, visible, capacity, !state->editing);
    }
    cursor_column = MIN(used + 1, state->columns);
    pad_columns(output, state->columns - used);
    return cursor_column;
}

gchar *
mux_bar_render(const MuxBarRenderState *state)
{
    g_return_val_if_fail(state != NULL, NULL);
    if (state->columns == 0 || state->rows == 0)
        return g_strdup("");

    GString *output = g_string_new("\033[?7l\033[H");
    guint address_row = state->rows > 1 ? 2 : 1;

    if (state->rows > 1) {
        append_header(output, state);
        g_string_append(output, "\r\n");
    }
    guint cursor_column = append_address(output, state);
    for (guint row = address_row; row < state->rows; row++) {
        g_string_append(output, "\r\n" BAR_BACKGROUND);
        pad_columns(output, state->columns);
    }
    /* Never emit a newline after the last row: it would scroll the bar. */
    if (state->editing)
        g_string_append_printf(output, "\033[%u;%uH\033[?25h",
                               address_row, cursor_column);
    else
        g_string_append(output, "\033[?25l");
    g_string_append(output, "\033[?7h");
    return g_string_free(output, FALSE);
}
