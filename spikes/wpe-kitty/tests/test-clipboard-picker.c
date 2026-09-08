#include "../mux-clipboard-picker.h"
#include "../mux-clipboard-picker-controller.h"

#include <glib.h>
#include <string.h>

#define TEST_COLUMNS 160u
#define TEST_ROWS 40u
#define PANEL_COLUMNS 88u
#define PANEL_ROWS 14u

typedef struct {
    gunichar character;
    gboolean opaque;
} TestCell;

typedef struct {
    TestCell cells[TEST_ROWS][TEST_COLUMNS];
    guint columns;
    guint rows;
    guint column;
    guint row;
    guint saved_column;
    guint saved_row;
    gboolean background;
    gboolean saved_background;
    gboolean wrap_pending;
    gboolean saved_wrap_pending;
    guint painted_columns;
    guint painted_rows;
    guint transparent_writes;
    guint automatic_wraps;
    guint scrolls;
    guint invalid_positions;
    guint position_commands;
    guint saves;
    guint restores;
    guint line_erases;
    guint nondefault_erases;
    guint screen_erases;
} TestTerminal;

typedef struct {
    guint lists;
    guint selects;
    guint pins;
    guint deletes;
    guint clears;
    guint cancels;
    guint changed;
    guint closed;
    guint64 serial;
    guint64 cancelled_serial;
    guint64 entry_id;
    gboolean pinned;
} TestBackend;

static gchar *
visible_text(const gchar *ansi)
{
    GString *text = g_string_new(NULL);
    const gchar *cursor = ansi;

    g_assert_nonnull(ansi);
    while (*cursor != '\0') {
        if (*cursor != '\033') {
            g_string_append_c(text, *cursor++);
            continue;
        }
        cursor++;
        if (*cursor == '[') {
            cursor++;
            while (*cursor != '\0' &&
                   !(*cursor >= '@' && *cursor <= '~'))
                cursor++;
        }
        if (*cursor != '\0')
            cursor++;
    }
    return g_string_free(text, FALSE);
}

static void
assert_contains(const gchar *text, const gchar *needle)
{
    g_assert_nonnull(strstr(text, needle));
}

static void
assert_absent(const gchar *text, const gchar *needle)
{
    g_assert_null(strstr(text, needle));
}

static void
assert_no_selection_hints(const gchar *text)
{
    assert_absent(text, "Enter paste");
    assert_absent(text, "Alt+P pin");
    assert_absent(text, "Alt+P unpin");
    assert_absent(text, "Alt+D delete");
}

static void
assert_only_close_hint(const gchar *text)
{
    assert_no_selection_hints(text);
    assert_absent(text, "Alt+C clear unpinned");
    assert_absent(text, "Ctrl+U clear search");
    assert_contains(text, "Esc close");
}

static MuxClipboardPickerItem *
test_item_new(guint64 id, gboolean pinned)
{
    static const gchar *const mime_types[] = {
        "text/plain", "text/html", "image/png",
    };
    g_autofree gchar *preview = g_strdup_printf(
        "payload-%" G_GUINT64_FORMAT " with rich formats", id);

    return mux_clipboard_picker_item_new_full(
        id,
        G_GINT64_CONSTANT(1700000000000000) + (gint64)id,
        "https://source.test",
        300 + id,
        pinned,
        8192,
        preview,
        mime_types,
        G_N_ELEMENTS(mime_types),
        7);
}

static GPtrArray *
test_items_new(guint count, gboolean pinned)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_clipboard_picker_item_unref);
    guint index;

    for (index = 0; index < count; index++)
        g_ptr_array_add(items, test_item_new(index + 1, pinned));
    return items;
}

static gchar *
render_visible(MuxClipboardPicker *picker, gboolean ready)
{
    g_autofree gchar *panel = mux_clipboard_picker_render_full(
        picker, 120, 24, ready);

    return visible_text(panel);
}

static void
assert_action(MuxClipboardPicker *picker,
              MuxClipboardPickerKey key,
              MuxClipboardPickerActionKind kind,
              guint64 entry_id,
              gboolean pinned)
{
    MuxClipboardPickerAction action = { 0 };

    g_assert_true(mux_clipboard_picker_handle_key(picker, key, 0, &action));
    g_assert_cmpint(action.kind, ==, kind);
    g_assert_cmpuint(action.entry_id, ==, entry_id);
    g_assert_cmpint(action.pinned, ==, pinned);
}

static void
assert_rejected_action(MuxClipboardPicker *picker, MuxClipboardPickerKey key)
{
    MuxClipboardPickerAction action = {
        .kind = MUX_CLIPBOARD_PICKER_ACTION_SELECT,
        .entry_id = 99,
        .pinned = TRUE,
    };

    g_assert_false(mux_clipboard_picker_handle_key(picker, key, 0, &action));
    g_assert_cmpint(action.kind, ==, MUX_CLIPBOARD_PICKER_ACTION_NONE);
    g_assert_cmpuint(action.entry_id, ==, 0);
    g_assert_false(action.pinned);
}

static void
test_empty_history_and_no_results(void)
{
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    g_autoptr(GPtrArray) items = test_items_new(1, FALSE);
    g_autofree gchar *legacy = mux_clipboard_picker_render(picker, 120, 24);
    g_autofree gchar *full = mux_clipboard_picker_render_full(
        picker, 120, 24, TRUE);

    g_assert_cmpstr(legacy, ==, full);
    g_assert_cmpuint(mux_clipboard_picker_get_match_count(picker), ==, 0);
    g_assert_null(mux_clipboard_picker_get_selected(picker));
    {
        g_autofree gchar *text = visible_text(full);

        assert_contains(text, "No clipboard history in this profile yet.");
        assert_contains(text, "Copy in this profile to add an item.");
        assert_contains(text, "History from other profiles stays separate.");
        assert_absent(text, "No matches for this search.");
        assert_only_close_hint(text);
    }
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_ENTER);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY);
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_ESCAPE,
                  MUX_CLIPBOARD_PICKER_ACTION_CLOSE, 0, FALSE);

    mux_clipboard_picker_set_query(picker, "zzzzzz");
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "No clipboard history in this profile yet.");
        assert_absent(text, "No matches for this search.");
        assert_contains(text, "Ctrl+U clear search");
        assert_absent(text, "Alt+C clear unpinned");
    }

    mux_clipboard_picker_set_items(picker, items);
    g_assert_cmpuint(mux_clipboard_picker_get_match_count(picker), ==, 0);
    g_assert_null(mux_clipboard_picker_get_selected(picker));
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "No matches for this search.");
        assert_absent(text, "No clipboard history in this profile yet.");
        assert_absent(text, "Copy in this profile to add an item.");
        assert_no_selection_hints(text);
        assert_contains(text, "Ctrl+U clear search");
        assert_contains(text, "Alt+C clear unpinned");
        assert_contains(text, "Esc close");
    }
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_ENTER);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY);
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY,
                  MUX_CLIPBOARD_PICKER_ACTION_CLEAR, 0, FALSE);
    mux_clipboard_picker_free(picker);
}

static void
test_contextual_actions(void)
{
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    g_autoptr(GPtrArray) unpinned = test_items_new(1, FALSE);
    g_autoptr(GPtrArray) pinned = test_items_new(1, TRUE);
    g_autoptr(GPtrArray) mixed = test_items_new(1, TRUE);

    g_ptr_array_add(mixed, test_item_new(2, FALSE));
    mux_clipboard_picker_set_items(picker, unpinned);
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "Enter paste");
        assert_contains(text, "Alt+P pin");
        assert_contains(text, "Alt+D delete");
        assert_contains(text, "Alt+C clear unpinned");
        assert_absent(text, "Alt+P unpin");
        assert_absent(text, "Ctrl+U clear search");
        assert_contains(text, "Esc close");
    }
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_ENTER,
                  MUX_CLIPBOARD_PICKER_ACTION_SELECT, 1, FALSE);
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN,
                  MUX_CLIPBOARD_PICKER_ACTION_SET_PINNED, 1, TRUE);
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY,
                  MUX_CLIPBOARD_PICKER_ACTION_DELETE, 1, FALSE);

    mux_clipboard_picker_set_items(picker, pinned);
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "Alt+P unpin");
        assert_absent(text, "Alt+P pin");
        assert_absent(text, "Alt+C clear unpinned");
    }
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN,
                  MUX_CLIPBOARD_PICKER_ACTION_SET_PINNED, 1, FALSE);
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY);
    mux_clipboard_picker_set_query(picker, "zzzzzz");
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_no_selection_hints(text);
        assert_absent(text, "Alt+C clear unpinned");
        assert_contains(text, "Ctrl+U clear search");
    }
    assert_rejected_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY);
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY,
                  MUX_CLIPBOARD_PICKER_ACTION_NONE, 0, FALSE);
    g_assert_cmpstr(mux_clipboard_picker_get_query(picker), ==, "");

    /* Clearing is a history-wide action, not an action on the pinned row. */
    mux_clipboard_picker_set_items(picker, mixed);
    g_assert_true(mux_clipboard_picker_item_get_pinned(
        mux_clipboard_picker_get_selected(picker)));
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "Alt+P unpin");
        assert_contains(text, "Alt+C clear unpinned");
    }
    assert_action(picker, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY,
                  MUX_CLIPBOARD_PICKER_ACTION_CLEAR, 0, FALSE);
    mux_clipboard_picker_free(picker);
}

static void
test_metadata_and_multi_mime_search(void)
{
    static const gchar *const queries[] = {
        "payload-1", "source.test", "text/html", "IMAGE/PNG",
    };
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    g_autoptr(GPtrArray) items = test_items_new(1, FALSE);
    guint index;

    mux_clipboard_picker_set_items(picker, items);
    for (index = 0; index < G_N_ELEMENTS(queries); index++) {
        g_autofree gchar *text = NULL;
        const MuxClipboardPickerItem *selected;

        mux_clipboard_picker_set_query(picker, queries[index]);
        g_assert_cmpuint(mux_clipboard_picker_get_match_count(picker), ==, 1);
        selected = mux_clipboard_picker_get_selected(picker);
        g_assert_nonnull(selected);
        g_assert_cmpuint(mux_clipboard_picker_item_get_id(selected), ==, 1);
        text = render_visible(picker, TRUE);
        assert_contains(text, "payload-1 with rich formats");
        assert_contains(text, "https://source.test");
        /* Three searchable MIME labels represent seven original formats. */
        assert_contains(text, "7 fmt");
    }
    mux_clipboard_picker_free(picker);
}

static void
test_status_and_busy_hints(void)
{
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    g_autoptr(GPtrArray) items = test_items_new(1, FALSE);

    mux_clipboard_picker_set_status(picker, "history request failed: offline");
    {
        g_autofree gchar *text = render_visible(picker, TRUE);

        assert_contains(text, "history request failed: offline");
        assert_absent(text, "No clipboard history in this profile yet.");
        assert_absent(text, "No matches for this search.");
        assert_only_close_hint(text);
    }
    mux_clipboard_picker_set_status(picker, "loading history");
    {
        g_autofree gchar *text = render_visible(picker, FALSE);

        assert_contains(text, "loading history");
        assert_absent(text, "No clipboard history in this profile yet.");
        assert_only_close_hint(text);
    }
    mux_clipboard_picker_set_items(picker, items);
    mux_clipboard_picker_set_query(picker, "payload");
    mux_clipboard_picker_set_status(picker, "updating pinned state");
    {
        g_autofree gchar *text = render_visible(picker, FALSE);

        assert_contains(text, "updating pinned state");
        assert_only_close_hint(text);
    }
    mux_clipboard_picker_free(picker);
}

static void
terminal_init(TestTerminal *terminal, guint columns, guint rows)
{
    guint row;
    guint column;

    g_assert_cmpuint(columns, <=, TEST_COLUMNS);
    g_assert_cmpuint(rows, <=, TEST_ROWS);
    memset(terminal, 0, sizeof(*terminal));
    terminal->columns = columns;
    terminal->rows = rows;
    for (row = 0; row < TEST_ROWS; row++) {
        for (column = 0; column < TEST_COLUMNS; column++)
            terminal->cells[row][column].character = '.';
    }
}

static void
terminal_resize(TestTerminal *terminal, guint columns, guint rows)
{
    g_assert_cmpuint(columns, <=, TEST_COLUMNS);
    g_assert_cmpuint(rows, <=, TEST_ROWS);
    terminal->columns = columns;
    terminal->rows = rows;
    terminal->column = columns == 0 ? 0 : MIN(terminal->column, columns - 1);
    terminal->row = rows == 0 ? 0 : MIN(terminal->row, rows - 1);
    terminal->wrap_pending = FALSE;
    /* Hidden cells deliberately survive a resize until explicitly erased. */
}

static void
terminal_position(TestTerminal *terminal, gint row, gint column)
{
    terminal->position_commands++;
    if (row < 0 || column < 0 ||
        (guint)row >= terminal->rows ||
        (guint)column >= terminal->columns) {
        terminal->invalid_positions++;
        return;
    }
    terminal->row = (guint)row;
    terminal->column = (guint)column;
    terminal->wrap_pending = FALSE;
}

static void
terminal_save(TestTerminal *terminal)
{
    terminal->saves++;
    terminal->saved_row = terminal->row;
    terminal->saved_column = terminal->column;
    terminal->saved_background = terminal->background;
    terminal->saved_wrap_pending = terminal->wrap_pending;
}

static void
terminal_restore(TestTerminal *terminal)
{
    terminal->restores++;
    terminal->row = terminal->saved_row;
    terminal->column = terminal->saved_column;
    terminal->background = terminal->saved_background;
    terminal->wrap_pending = terminal->saved_wrap_pending;
}

static guint
terminal_argument(const guint *values, guint count, guint index, guint fallback)
{
    return index < count && values[index] != 0 ? values[index] : fallback;
}

static const gchar *
terminal_csi(TestTerminal *terminal, const gchar *start)
{
    const gchar *end = start;
    guint values[64] = { 0 };
    guint count = 0;
    guint index;
    g_autofree gchar *parameters = NULL;
    g_auto(GStrv) parts = NULL;

    while (*end != '\0' && !(*end >= '@' && *end <= '~'))
        end++;
    g_assert_cmpint(*end, !=, '\0');
    parameters = g_strndup(start, (gsize)(end - start));
    parts = g_strsplit(parameters, ";", -1);
    for (index = 0; parts[index] != NULL; index++) {
        const gchar *part = parts[index];

        g_assert_cmpuint(count, <, G_N_ELEMENTS(values));
        if (*part == '?')
            part++;
        values[count++] = (guint)g_ascii_strtoull(part, NULL, 10);
    }

    switch (*end) {
    case 'm':
        if (count == 0)
            terminal->background = FALSE;
        for (index = 0; index < count; index++) {
            guint value = values[index];

            if (value == 0 || value == 49)
                terminal->background = FALSE;
            else if ((value >= 40 && value <= 47) ||
                     (value >= 100 && value <= 107))
                terminal->background = TRUE;
            else if ((value == 38 || value == 48) && index + 1 < count) {
                guint mode = values[index + 1];

                if (value == 48)
                    terminal->background = TRUE;
                if (mode == 2)
                    index += 4;
                else if (mode == 5)
                    index += 2;
            }
        }
        break;
    case 'H':
    case 'f':
        terminal_position(terminal,
                          (gint)terminal_argument(values, count, 0, 1) - 1,
                          (gint)terminal_argument(values, count, 1, 1) - 1);
        break;
    case 'G':
        terminal_position(terminal, (gint)terminal->row,
                          (gint)terminal_argument(values, count, 0, 1) - 1);
        break;
    case 'd':
        terminal_position(terminal,
                          (gint)terminal_argument(values, count, 0, 1) - 1,
                          (gint)terminal->column);
        break;
    case 'A':
        terminal_position(terminal,
                          (gint)terminal->row -
                              (gint)terminal_argument(values, count, 0, 1),
                          (gint)terminal->column);
        break;
    case 'B':
        terminal_position(terminal,
                          (gint)terminal->row +
                              (gint)terminal_argument(values, count, 0, 1),
                          (gint)terminal->column);
        break;
    case 'C':
        terminal_position(terminal, (gint)terminal->row,
                          (gint)terminal->column +
                              (gint)terminal_argument(values, count, 0, 1));
        break;
    case 'D':
        terminal_position(terminal, (gint)terminal->row,
                          (gint)terminal->column -
                              (gint)terminal_argument(values, count, 0, 1));
        break;
    case 's':
        terminal_save(terminal);
        break;
    case 'u':
        terminal_restore(terminal);
        break;
    case 'K':
        g_assert_cmpuint(terminal_argument(values, count, 0, 0), ==, 2);
        terminal->line_erases++;
        if (terminal->background)
            terminal->nondefault_erases++;
        for (index = 0; index < terminal->columns; index++) {
            terminal->cells[terminal->row][index] = (TestCell) {
                .character = ' ',
                .opaque = terminal->background,
            };
        }
        terminal->wrap_pending = FALSE;
        break;
    case 'J':
        terminal->screen_erases++;
        break;
    case 'h':
    case 'l':
        /* Cursor visibility does not affect cell geometry. */
        break;
    default:
        g_error("Unexpected ANSI control in picker output: CSI %s%c",
                parameters, *end);
    }
    return end + 1;
}

static void
terminal_linefeed(TestTerminal *terminal)
{
    if (terminal->row + 1 >= terminal->rows)
        terminal->scrolls++;
    else
        terminal->row++;
    terminal->wrap_pending = FALSE;
}

static void
terminal_write(TestTerminal *terminal, gunichar character)
{
    guint width;
    guint index;

    if (g_unichar_iszerowidth(character))
        return;
    g_assert_true(g_unichar_isprint(character));
    width = g_unichar_iswide(character) ? 2 : 1;
    if (terminal->rows == 0 || width > terminal->columns) {
        terminal->invalid_positions++;
        return;
    }
    if (terminal->wrap_pending ||
        terminal->column + width > terminal->columns) {
        terminal->automatic_wraps++;
        terminal->column = 0;
        terminal_linefeed(terminal);
    }
    for (index = 0; index < width; index++) {
        terminal->cells[terminal->row][terminal->column + index] = (TestCell) {
            .character = index == 0 ? character : 0,
            .opaque = terminal->background,
        };
        if (!terminal->background)
            terminal->transparent_writes++;
    }
    terminal->painted_rows = MAX(terminal->painted_rows, terminal->row + 1);
    terminal->painted_columns = MAX(terminal->painted_columns,
                                    terminal->column + width);
    if (terminal->column + width == terminal->columns) {
        terminal->column = terminal->columns - 1;
        terminal->wrap_pending = TRUE;
    } else {
        terminal->column += width;
    }
}

static void
terminal_feed(TestTerminal *terminal, const gchar *output)
{
    const gchar *cursor = output;

    g_assert_nonnull(output);
    g_assert_true(g_utf8_validate(output, -1, NULL));
    terminal->painted_columns = 0;
    terminal->painted_rows = 0;
    terminal->transparent_writes = 0;
    terminal->automatic_wraps = 0;
    terminal->scrolls = 0;
    terminal->invalid_positions = 0;
    terminal->position_commands = 0;
    terminal->saves = 0;
    terminal->restores = 0;
    terminal->line_erases = 0;
    terminal->nondefault_erases = 0;
    terminal->screen_erases = 0;
    while (*cursor != '\0') {
        if (*cursor == '\033') {
            cursor++;
            if (*cursor == '[') {
                cursor = terminal_csi(terminal, cursor + 1);
            } else if (*cursor == '7') {
                terminal_save(terminal);
                cursor++;
            } else if (*cursor == '8') {
                terminal_restore(terminal);
                cursor++;
            } else {
                g_error("Unexpected escape in picker output");
            }
        } else if (*cursor == '\r') {
            terminal->column = 0;
            terminal->wrap_pending = FALSE;
            cursor++;
        } else if (*cursor == '\n') {
            terminal_linefeed(terminal);
            cursor++;
        } else {
            terminal_write(terminal, g_utf8_get_char(cursor));
            cursor = g_utf8_next_char(cursor);
        }
    }
}

static void
assert_terminal_safe(const TestTerminal *terminal)
{
    g_assert_cmpuint(terminal->invalid_positions, ==, 0);
    g_assert_cmpuint(terminal->automatic_wraps, ==, 0);
    g_assert_cmpuint(terminal->scrolls, ==, 0);
    g_assert_cmpuint(terminal->transparent_writes, ==, 0);
    g_assert_cmpuint(terminal->nondefault_erases, ==, 0);
    g_assert_cmpuint(terminal->screen_erases, ==, 0);
}

static guint
assert_panel_geometry(MuxClipboardPicker *picker, guint columns, guint rows)
{
    g_autofree TestTerminal *terminal = g_new0(TestTerminal, 1);
    g_autofree gchar *panel = mux_clipboard_picker_render_full(
        picker, columns, rows, TRUE);
    g_autofree gchar *text = visible_text(panel);
    const gchar *cursor;

    if (columns == 0 || rows == 0) {
        g_assert_cmpstr(panel, ==, "");
        return 0;
    }
    g_assert_cmpstr(text, !=, "");
    g_assert_false(g_str_has_suffix(text, "\n"));
    g_assert_false(g_str_has_suffix(text, "\r"));
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            g_assert_true(cursor > text);
            g_assert_cmpint(cursor[-1], ==, '\r');
        } else if (*cursor == '\r') {
            g_assert_cmpint(cursor[1], ==, '\n');
        }
    }
    terminal_init(terminal, columns, rows);
    terminal_feed(terminal, panel);
    assert_terminal_safe(terminal);
    g_assert_cmpuint(terminal->position_commands, ==, 0);
    g_assert_cmpuint(terminal->saves, ==, 0);
    g_assert_cmpuint(terminal->restores, ==, 0);
    g_assert_cmpuint(terminal->line_erases, ==, 0);
    g_assert_cmpuint(terminal->painted_columns, <=, MIN(columns, PANEL_COLUMNS));
    g_assert_cmpuint(terminal->painted_rows, <=, MIN(rows, PANEL_ROWS));
    return terminal->painted_rows;
}

static void
test_narrow_short_and_compact_geometry(void)
{
    static const guint columns[] = { 0, 1, 2, 7, 19, 40, 87, 88, 89, 120 };
    static const guint rows[] = { 0, 1, 2, 3, 5, 6, 13, 14, 15, 16, 24 };
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    g_autoptr(GPtrArray) items = test_items_new(24, FALSE);
    guint empty_height;
    guint populated_height;
    guint column_index;
    guint row_index;

    empty_height = assert_panel_geometry(picker, 120, 24);
    g_assert_cmpuint(empty_height, <, PANEL_ROWS);
    mux_clipboard_picker_set_items(picker, items);
    populated_height = assert_panel_geometry(picker, 120, 24);
    g_assert_cmpuint(populated_height, >, empty_height);
    for (column_index = 0; column_index < G_N_ELEMENTS(columns); column_index++) {
        for (row_index = 0; row_index < G_N_ELEMENTS(rows); row_index++)
            assert_panel_geometry(picker, columns[column_index], rows[row_index]);
    }
    mux_clipboard_picker_set_query(picker, "zzzzzz");
    g_assert_cmpuint(assert_panel_geometry(picker, 120, 24), <, populated_height);
    mux_clipboard_picker_free(picker);
}

static void
assert_surface_frame(TestTerminal *terminal,
                     MuxClipboardPickerSurface *surface,
                     MuxClipboardPicker *picker)
{
    g_autofree TestTerminal *expected = g_new0(TestTerminal, 1);
    g_autofree gchar *panel = mux_clipboard_picker_render(
        picker, terminal->columns, terminal->rows);
    g_autofree gchar *output = mux_clipboard_picker_surface_present(
        surface, panel, terminal->columns, terminal->rows);
    guint saved_row = terminal->row;
    guint saved_column = terminal->column;
    guint top = terminal->rows >= 16 ? 1 : 0;
    guint left = (terminal->columns - MIN(terminal->columns, PANEL_COLUMNS)) / 2;
    guint row;
    guint column;

    terminal_feed(terminal, output);
    assert_terminal_safe(terminal);
    g_assert_cmpuint(terminal->row, ==, saved_row);
    g_assert_cmpuint(terminal->column, ==, saved_column);
    if (terminal->columns == 0 || terminal->rows == 0) {
        g_assert_cmpstr(output, ==, "");
        return;
    }
    g_assert_cmpuint(terminal->saves, >, 0);
    g_assert_cmpuint(terminal->restores, ==, terminal->saves);
    terminal_init(expected, terminal->columns, terminal->rows);
    terminal_feed(expected, panel);
    assert_terminal_safe(expected);
    for (row = 0; row < terminal->rows; row++) {
        for (column = 0; column < terminal->columns; column++) {
            const TestCell *actual_cell = &terminal->cells[row][column];
            const TestCell *expected_cell = NULL;

            if (row >= top && column >= left)
                expected_cell = &expected->cells[row - top][column - left];
            if (expected_cell != NULL && expected_cell->opaque) {
                g_assert_true(actual_cell->opaque);
                g_assert_cmpuint(actual_cell->character,
                                 ==, expected_cell->character);
            } else {
                g_assert_false(actual_cell->opaque);
            }
        }
    }
}

static void
assert_surface_clear(TestTerminal *terminal, MuxClipboardPickerSurface *surface)
{
    g_autofree gchar *output = mux_clipboard_picker_surface_clear(
        surface, terminal->columns, terminal->rows);
    guint saved_row = terminal->row;
    guint saved_column = terminal->column;
    guint row;
    guint column;

    terminal_feed(terminal, output);
    assert_terminal_safe(terminal);
    g_assert_cmpuint(terminal->row, ==, saved_row);
    g_assert_cmpuint(terminal->column, ==, saved_column);
    if (terminal->columns == 0 || terminal->rows == 0) {
        g_assert_cmpstr(output, ==, "");
        return;
    }
    for (row = 0; row < terminal->rows; row++) {
        for (column = 0; column < terminal->columns; column++)
            g_assert_false(terminal->cells[row][column].opaque);
    }
}

static void
test_surface_shrink_grow_and_close(void)
{
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    MuxClipboardPickerSurface surface = { 0 };
    g_autofree TestTerminal *terminal = g_new0(TestTerminal, 1);
    g_autoptr(GPtrArray) many = test_items_new(24, FALSE);
    g_autoptr(GPtrArray) one = test_items_new(1, FALSE);
    g_autoptr(GPtrArray) empty = test_items_new(0, FALSE);

    terminal_init(terminal, 120, 24);
    terminal->row = 20;
    terminal->column = 110;
    mux_clipboard_picker_set_items(picker, many);
    assert_surface_frame(terminal, &surface, picker);
    g_assert_cmpuint(surface.painted_rows, >, 0);
    g_assert_cmpuint(surface.painted_columns, >, 0);

    /* Shorter content must erase the old bottom even without a resize. */
    mux_clipboard_picker_set_items(picker, one);
    assert_surface_frame(terminal, &surface, picker);
    mux_clipboard_picker_set_items(picker, many);
    assert_surface_frame(terminal, &surface, picker);

    terminal_resize(terminal, 40, 24);
    assert_surface_frame(terminal, &surface, picker);
    terminal_resize(terminal, 40, 4);
    assert_surface_frame(terminal, &surface, picker);
    terminal_resize(terminal, 0, 0);
    assert_surface_frame(terminal, &surface, picker);
    g_assert_cmpuint(surface.painted_rows, >, 0);
    g_assert_cmpuint(surface.painted_columns, >, 0);

    mux_clipboard_picker_set_items(picker, empty);
    terminal_resize(terminal, 120, 24);
    assert_surface_frame(terminal, &surface, picker);
    assert_surface_clear(terminal, &surface);
    g_assert_cmpuint(terminal->line_erases, >, 0);
    g_assert_cmpuint(surface.painted_rows, ==, 0);
    g_assert_cmpuint(surface.painted_columns, ==, 0);
    /* The surface must not erase unrelated rows below its maximum footprint. */
    g_assert_cmpuint(terminal->cells[23][0].character, ==, '.');
    mux_clipboard_picker_free(picker);
}

static void
test_surface_close_retains_hidden_dirty_extent(void)
{
    MuxClipboardPicker *picker = mux_clipboard_picker_new("work");
    MuxClipboardPickerSurface surface = { 0 };
    g_autofree TestTerminal *terminal = g_new0(TestTerminal, 1);
    g_autoptr(GPtrArray) items = test_items_new(24, FALSE);

    terminal_init(terminal, 120, 24);
    mux_clipboard_picker_set_items(picker, items);
    assert_surface_frame(terminal, &surface, picker);
    terminal_resize(terminal, 40, 3);
    /* An inherited nondefault SGR background must not color erased rows. */
    terminal->background = TRUE;
    assert_surface_clear(terminal, &surface);
    g_assert_cmpuint(surface.painted_rows, >, 0);
    g_assert_cmpuint(surface.painted_columns, >, 0);
    terminal_resize(terminal, 0, 0);
    assert_surface_clear(terminal, &surface);
    g_assert_cmpuint(surface.painted_rows, >, 0);
    g_assert_cmpuint(surface.painted_columns, >, 0);
    terminal_resize(terminal, 120, 24);
    assert_surface_clear(terminal, &surface);
    g_assert_cmpuint(surface.painted_rows, ==, 0);
    g_assert_cmpuint(surface.painted_columns, ==, 0);
    assert_surface_clear(terminal, &surface);
    g_assert_cmpuint(terminal->line_erases, ==, 0);
    g_assert_cmpuint(terminal->cells[23][0].character, ==, '.');
    mux_clipboard_picker_free(picker);
}

static gboolean
backend_list(guint64 serial, gpointer user_data, GError **error)
{
    TestBackend *backend = user_data;

    (void)error;
    backend->lists++;
    backend->serial = serial;
    return TRUE;
}

static gboolean
backend_select(guint64 serial, guint64 entry_id,
               gpointer user_data, GError **error)
{
    TestBackend *backend = user_data;

    (void)error;
    backend->selects++;
    backend->serial = serial;
    backend->entry_id = entry_id;
    return TRUE;
}

static gboolean
backend_pin(guint64 serial, guint64 entry_id, gboolean pinned,
            gpointer user_data, GError **error)
{
    TestBackend *backend = user_data;

    (void)error;
    backend->pins++;
    backend->serial = serial;
    backend->entry_id = entry_id;
    backend->pinned = pinned;
    return TRUE;
}

static gboolean
backend_delete(guint64 serial, guint64 entry_id,
               gpointer user_data, GError **error)
{
    TestBackend *backend = user_data;

    (void)error;
    backend->deletes++;
    backend->serial = serial;
    backend->entry_id = entry_id;
    return TRUE;
}

static gboolean
backend_clear(guint64 serial, gpointer user_data, GError **error)
{
    TestBackend *backend = user_data;

    (void)error;
    backend->clears++;
    backend->serial = serial;
    return TRUE;
}

static void
backend_cancel(guint64 serial, gpointer user_data)
{
    TestBackend *backend = user_data;

    backend->cancels++;
    backend->cancelled_serial = serial;
}

static void
controller_changed(MuxClipboardPickerController *controller, gpointer user_data)
{
    TestBackend *backend = user_data;

    (void)controller;
    backend->changed++;
}

static void
controller_closed(MuxClipboardPickerController *controller, gpointer user_data)
{
    TestBackend *backend = user_data;

    (void)controller;
    backend->closed++;
}

static MuxClipboardPickerController *
test_controller_new(TestBackend *backend)
{
    const MuxClipboardPickerBackend callbacks = {
        .list = backend_list,
        .select = backend_select,
        .set_pinned = backend_pin,
        .delete_entry = backend_delete,
        .clear = backend_clear,
        .cancel = backend_cancel,
    };

    return mux_clipboard_picker_controller_new(
        "work", &callbacks, backend, NULL,
        controller_changed, controller_closed, backend, NULL);
}

static gchar *
controller_visible(MuxClipboardPickerController *controller)
{
    g_autofree gchar *panel = mux_clipboard_picker_controller_render(
        controller, 120, 24);

    return visible_text(panel);
}

static void
assert_controller_busy(MuxClipboardPickerController *controller,
                       MuxClipboardPickerControllerState state)
{
    static const MuxClipboardPickerKey blocked_keys[] = {
        MUX_CLIPBOARD_PICKER_KEY_TEXT,
        MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY,
        MUX_CLIPBOARD_PICKER_KEY_UP,
        MUX_CLIPBOARD_PICKER_KEY_ENTER,
        MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN,
        MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY,
        MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY,
    };
    g_autofree gchar *text = controller_visible(controller);
    guint64 serial = mux_clipboard_picker_controller_get_active_serial(controller);
    guint index;

    g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller), ==, state);
    g_assert_cmpuint(serial, !=, 0);
    assert_only_close_hint(text);
    for (index = 0; index < G_N_ELEMENTS(blocked_keys); index++) {
        g_assert_false(mux_clipboard_picker_controller_handle_key(
            controller, blocked_keys[index], 'p'));
    }
    g_assert_cmpuint(mux_clipboard_picker_controller_get_active_serial(controller),
                     ==, serial);
    g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller), ==, state);
}

static void
test_controller_busy_and_stale_completion(void)
{
    TestBackend backend = { 0 };
    MuxClipboardPickerController *controller = test_controller_new(&backend);
    g_autoptr(GPtrArray) items = test_items_new(1, FALSE);
    g_autoptr(GPtrArray) pinned = test_items_new(1, TRUE);
    guint64 serial;

    mux_clipboard_picker_controller_open(controller);
    g_assert_cmpuint(backend.lists, ==, 1);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING);
    {
        g_autofree gchar *text = controller_visible(controller);

        assert_contains(text, "loading history");
        assert_absent(text, "No clipboard history in this profile yet.");
    }
    mux_clipboard_picker_controller_complete_list(
        controller, backend.serial, items, NULL);
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_TEXT, 'p'));
    {
        g_autofree gchar *text = controller_visible(controller);

        assert_contains(text, "Ctrl+U clear search");
        assert_contains(text, "Alt+P pin");
    }
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN, 0));
    g_assert_cmpuint(backend.pins, ==, 1);
    g_assert_cmpuint(backend.entry_id, ==, 1);
    g_assert_true(backend.pinned);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING);
    serial = backend.serial;
    mux_clipboard_picker_controller_complete_request(controller, serial + 1, NULL);
    g_assert_cmpuint(mux_clipboard_picker_controller_get_active_serial(controller),
                     ==, serial);
    mux_clipboard_picker_controller_complete_request(controller, serial, NULL);
    g_assert_cmpuint(backend.lists, ==, 2);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING);
    mux_clipboard_picker_controller_complete_list(
        controller, backend.serial, pinned, NULL);
    {
        g_autofree gchar *text = controller_visible(controller);

        assert_contains(text, "Alt+P unpin");
        assert_absent(text, "Alt+C clear unpinned");
    }
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_ENTER, 0));
    g_assert_cmpuint(backend.selects, ==, 1);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_SELECTING);
    serial = backend.serial;
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_ESCAPE, 0));
    g_assert_cmpuint(backend.cancels, ==, 1);
    g_assert_cmpuint(backend.cancelled_serial, ==, serial);
    g_assert_cmpuint(backend.closed, ==, 1);
    g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller),
                    ==, MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED);
    g_assert_null(mux_clipboard_picker_controller_render(controller, 120, 24));
    mux_clipboard_picker_controller_complete_request(controller, serial, NULL);
    g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller),
                    ==, MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED);

    mux_clipboard_picker_controller_open(controller);
    g_assert_cmpuint(backend.lists, ==, 3);
    g_assert_cmpuint(backend.serial, !=, serial);
    mux_clipboard_picker_controller_complete_list(controller, serial, pinned, NULL);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING);
    mux_clipboard_picker_controller_close(controller);
    g_assert_cmpuint(backend.cancels, ==, 2);
    g_assert_cmpuint(backend.closed, ==, 2);
    g_assert_cmpuint(backend.deletes, ==, 0);
    g_assert_cmpuint(backend.clears, ==, 0);
    mux_clipboard_picker_controller_unref(controller);
}

static void
test_controller_rejects_empty_and_all_pinned_clear(void)
{
    TestBackend backend = { 0 };
    MuxClipboardPickerController *controller = test_controller_new(&backend);
    g_autoptr(GPtrArray) empty = test_items_new(0, FALSE);
    g_autoptr(GPtrArray) pinned = test_items_new(2, TRUE);
    g_autoptr(GPtrArray) unpinned = test_items_new(1, FALSE);
    GPtrArray *blocked_lists[] = { empty, pinned };
    guint index;

    for (index = 0; index < G_N_ELEMENTS(blocked_lists); index++) {
        g_autofree gchar *text = NULL;

        mux_clipboard_picker_controller_open(controller);
        mux_clipboard_picker_controller_complete_list(
            controller, backend.serial, blocked_lists[index], NULL);
        text = controller_visible(controller);
        assert_absent(text, "Alt+C clear unpinned");
        g_assert_false(mux_clipboard_picker_controller_handle_key(
            controller, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY, 0));
        g_assert_cmpuint(backend.clears, ==, 0);
        g_assert_cmpuint(mux_clipboard_picker_controller_get_active_serial(controller),
                         ==, 0);
        g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller),
                        ==, MUX_CLIPBOARD_PICKER_CONTROLLER_READY);
        mux_clipboard_picker_controller_close(controller);
    }

    mux_clipboard_picker_controller_open(controller);
    mux_clipboard_picker_controller_complete_list(
        controller, backend.serial, unpinned, NULL);
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_TEXT, 'z'));
    {
        g_autofree gchar *text = controller_visible(controller);

        assert_contains(text, "No matches for this search.");
        assert_contains(text, "Alt+C clear unpinned");
        assert_no_selection_hints(text);
    }
    g_assert_true(mux_clipboard_picker_controller_handle_key(
        controller, MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY, 0));
    g_assert_cmpuint(backend.clears, ==, 1);
    assert_controller_busy(controller, MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING);
    mux_clipboard_picker_controller_close(controller);
    mux_clipboard_picker_controller_unref(controller);
}

static void
test_controller_error_is_not_empty_history(void)
{
    TestBackend backend = { 0 };
    MuxClipboardPickerController *controller = test_controller_new(&backend);
    g_autoptr(GError) error = g_error_new_literal(
        g_quark_from_static_string("picker-test-error"), 1, "broker unavailable");

    mux_clipboard_picker_controller_open(controller);
    mux_clipboard_picker_controller_complete_list(
        controller, backend.serial, NULL, error);
    g_assert_cmpint(mux_clipboard_picker_controller_get_state(controller),
                    ==, MUX_CLIPBOARD_PICKER_CONTROLLER_READY);
    {
        g_autofree gchar *text = controller_visible(controller);

        assert_contains(text, "history request failed: broker unavailable");
        assert_absent(text, "No clipboard history in this profile yet.");
        assert_absent(text, "No matches for this search.");
        assert_only_close_hint(text);
    }
    mux_clipboard_picker_controller_close(controller);
    mux_clipboard_picker_controller_unref(controller);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/clipboard/picker/empty-history-and-no-results",
                    test_empty_history_and_no_results);
    g_test_add_func("/clipboard/picker/contextual-actions",
                    test_contextual_actions);
    g_test_add_func("/clipboard/picker/metadata-and-multi-mime-search",
                    test_metadata_and_multi_mime_search);
    g_test_add_func("/clipboard/picker/status-and-busy-hints",
                    test_status_and_busy_hints);
    g_test_add_func("/clipboard/picker/narrow-short-and-compact-geometry",
                    test_narrow_short_and_compact_geometry);
    g_test_add_func("/clipboard/picker/surface-shrink-grow-and-close",
                    test_surface_shrink_grow_and_close);
    g_test_add_func("/clipboard/picker/surface-close-hidden-dirty-extent",
                    test_surface_close_retains_hidden_dirty_extent);
    g_test_add_func("/clipboard/picker/controller-busy-and-stale-completion",
                    test_controller_busy_and_stale_completion);
    g_test_add_func("/clipboard/picker/controller-clear-guards",
                    test_controller_rejects_empty_and_all_pinned_clear);
    g_test_add_func("/clipboard/picker/controller-error-not-empty",
                    test_controller_error_is_not_empty_history);
    return g_test_run();
}
