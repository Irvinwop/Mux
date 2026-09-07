#include "mux-ui-pane.h"

#include <string.h>

typedef struct {
    gunichar character;
    guint background;
} Cell;

typedef struct {
    guint columns;
    guint rows;
    guint column;
    guint row;
    guint saved_column;
    guint saved_row;
    guint background;
    guint wraps;
    guint invalid_positions;
    gboolean wrap_pending;
    Cell *cells;
} Terminal;

typedef struct {
    Terminal terminal;
    MuxUiPaneBridge *bridge;
    gboolean dispose_on_write;
    gboolean dispose_on_send;
    guint writes;
    guint sends;
    guint destroyed;
} Fixture;

static void
terminal_resize(Terminal *terminal, guint columns, guint rows)
{
    Cell *cells = g_new0(Cell, columns * rows);
    guint row;
    guint column;

    for (row = 0; row < rows; row++) {
        for (column = 0; column < columns; column++) {
            Cell *cell = &cells[row * columns + column];

            cell->character = ' ';
            if (row < terminal->rows && column < terminal->columns)
                *cell = terminal->cells[row * terminal->columns + column];
        }
    }
    g_free(terminal->cells);
    terminal->cells = cells;
    terminal->columns = columns;
    terminal->rows = rows;
    terminal->row = MIN(terminal->row, rows - 1);
    terminal->column = MIN(terminal->column, columns - 1);
    terminal->wrap_pending = FALSE;
}

static void
terminal_csi(Terminal *terminal, const gchar *parameters, gchar command)
{
    g_auto(GStrv) values = g_strsplit(parameters, ";", -1);

    switch (command) {
    case 'H': {
        guint row = g_ascii_strtoull(values[0], NULL, 10);
        guint column = values[1]
            ? g_ascii_strtoull(values[1], NULL, 10) : 1;

        if (!row || row > terminal->rows ||
            !column || column > terminal->columns)
            terminal->invalid_positions++;
        terminal->row = CLAMP(row, 1, terminal->rows) - 1;
        terminal->column = CLAMP(column, 1, terminal->columns) - 1;
        terminal->wrap_pending = FALSE;
        break;
    }
    case 'K': {
        guint column;

        g_assert_cmpstr(parameters, ==, "2");
        for (column = 0; column < terminal->columns; column++) {
            Cell *cell = &terminal->cells[
                terminal->row * terminal->columns + column];

            cell->character = ' ';
            cell->background = terminal->background;
        }
        break;
    }
    case 'm': {
        guint i;

        for (i = 0; values[i]; i++) {
            guint code = g_ascii_strtoull(values[i], NULL, 10);

            if (!code || code == 49)
                terminal->background = 0;
            else if (code == 38 || code == 48) {
                g_assert_nonnull(values[i + 1]);
                g_assert_nonnull(values[i + 2]);
                g_assert_cmpstr(values[i + 1], ==, "5");
                if (code == 48)
                    terminal->background =
                        g_ascii_strtoull(values[i + 2], NULL, 10);
                i += 2;
            }
        }
        break;
    }
    case 's':
        terminal->saved_row = terminal->row;
        terminal->saved_column = terminal->column;
        break;
    case 'u':
        terminal->row = MIN(terminal->saved_row, terminal->rows - 1);
        terminal->column = MIN(terminal->saved_column, terminal->columns - 1);
        terminal->wrap_pending = FALSE;
        break;
    case 'h':
    case 'l':
        g_assert_true(g_str_has_prefix(parameters, "?"));
        break;
    default:
        g_assert_not_reached();
    }
}

/* Only the ANSI operations emitted by the renderer are needed here. */
static void
terminal_write(Terminal *terminal, const gchar *sequence)
{
    const gchar *cursor = sequence;

    while (*cursor) {
        gunichar character;
        guint width;

        if (*cursor == '\x1b') {
            const gchar *parameters;
            g_autofree gchar *copy = NULL;

            g_assert_cmpint(cursor[1], ==, '[');
            parameters = cursor + 2;
            cursor = parameters;
            while (*cursor && (*cursor < 0x40 || *cursor > 0x7e))
                cursor++;
            g_assert_cmpint(*cursor, !=, '\0');
            copy = g_strndup(parameters, cursor - parameters);
            terminal_csi(terminal, copy, *cursor++);
            continue;
        }
        character = g_utf8_get_char(cursor);
        width = g_unichar_iswide(character) ? 2 : 1;
        g_assert_true(g_unichar_isprint(character));
        if (terminal->wrap_pending ||
            terminal->column + width > terminal->columns) {
            terminal->wraps++;
            terminal->column = 0;
            terminal->row = MIN(terminal->row + 1, terminal->rows - 1);
            terminal->wrap_pending = FALSE;
        }
        terminal->cells[terminal->row * terminal->columns + terminal->column] =
            (Cell) { character, terminal->background };
        terminal->column += width;
        if (terminal->column >= terminal->columns) {
            terminal->column = terminal->columns - 1;
            terminal->wrap_pending = TRUE;
        }
        cursor = g_utf8_next_char(cursor);
    }
}

static MuxUiRequest *
new_request(void)
{
    MuxUiRequest *request = mux_ui_request_new(MUX_UI_REQUEST_DIALOG_ALERT);

    request->request_id = 17;
    request->origin = g_strdup("https://example.test");
    request->message = g_strdup("A page asks for your attention");
    return request;
}

static gboolean
write_ansi(const guint8 *data,
           gsize length,
           gpointer user_data,
           GError **error)
{
    Fixture *fixture = user_data;
    g_autofree gchar *sequence = g_strndup((const gchar *)data, length);

    (void)error;
    fixture->writes++;
    terminal_write(&fixture->terminal, sequence);
    if (fixture->dispose_on_write) {
        fixture->dispose_on_write = FALSE;
        g_clear_pointer(&fixture->bridge, mux_ui_pane_bridge_free);
        g_assert_cmpuint(fixture->destroyed, ==, 0);
    }
    return TRUE;
}

static gboolean
send_response(GBytes *payload, gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;
    g_autoptr(MuxUiResponse) response = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(payload, &length);

    g_assert_true(mux_ui_response_decode(data, length, &response, error));
    g_assert_cmpuint(response->request_id, ==, 17);
    g_assert_cmpint(response->action, ==, MUX_UI_ACTION_ACKNOWLEDGE);
    fixture->sends++;
    if (fixture->dispose_on_send) {
        fixture->dispose_on_send = FALSE;
        g_clear_pointer(&fixture->bridge, mux_ui_pane_bridge_free);
        g_assert_cmpuint(fixture->destroyed, ==, 0);
    }
    return TRUE;
}

static void
fixture_destroy(gpointer user_data)
{
    Fixture *fixture = user_data;

    fixture->destroyed++;
}

static void
fixture_init(Fixture *fixture)
{
    terminal_resize(&fixture->terminal, 80, 10);
    fixture->bridge = mux_ui_pane_bridge_new(
        send_response, write_ansi, fixture, fixture_destroy);
    g_assert_true(mux_ui_pane_bridge_set_size(fixture->bridge, 80, 10, NULL));
}

static gboolean
fixture_show(Fixture *fixture)
{
    g_autoptr(MuxUiRequest) request = new_request();
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) encoded = mux_ui_request_encode(request, &error);
    gsize length;
    const guint8 *data;

    g_assert_no_error(error);
    data = g_bytes_get_data(encoded, &length);
    return mux_ui_pane_bridge_handle_payload(fixture->bridge,
                                             data, length, &error);
}

static void
assert_clear_rows(const Terminal *terminal, guint first, guint last)
{
    guint row;
    guint column;

    for (row = first; row <= last; row++) {
        for (column = 0; column < terminal->columns; column++) {
            const Cell *cell = &terminal->cells[
                (row - 1) * terminal->columns + column];

            g_assert_cmpuint(cell->character, ==, ' ');
            g_assert_cmpuint(cell->background, ==, 0);
        }
    }
}

static void
fixture_clear(Fixture *fixture)
{
    g_clear_pointer(&fixture->bridge, mux_ui_pane_bridge_free);
    g_assert_cmpuint(fixture->destroyed, ==, 1);
    g_assert_cmpuint(fixture->terminal.wraps, ==, 0);
    g_assert_cmpuint(fixture->terminal.invalid_positions, ==, 0);
    g_free(fixture->terminal.cells);
}

static void
test_tiny_terminal_does_not_wrap(void)
{
    g_autoptr(MuxPaneOverlay) overlay = mux_pane_overlay_new();
    guint columns;
    guint rows;

    g_assert_true(mux_pane_overlay_push(overlay, new_request(), NULL));
    for (columns = 1; columns <= 4; columns++) {
        for (rows = 1; rows <= MUX_PANE_OVERLAY_ROWS; rows++) {
            Terminal terminal = { 0 };
            g_autofree gchar *sequence = mux_pane_overlay_render(
                overlay, columns, rows, g_get_monotonic_time());

            terminal_resize(&terminal, columns, rows);
            terminal_write(&terminal, sequence);
            g_assert_cmpuint(terminal.wraps, ==, 0);
            g_assert_cmpuint(terminal.invalid_positions, ==, 0);
            g_free(terminal.cells);
        }
    }
}

static void
test_prompt_background_covers_every_cell(void)
{
    const guint backgrounds[] = { 166, 236, 236, 238, 236 };
    Fixture fixture = { 0 };
    guint row;
    guint column;

    fixture_init(&fixture);
    g_assert_true(fixture_show(&fixture));
    for (row = 0; row < G_N_ELEMENTS(backgrounds); row++) {
        for (column = 0; column < fixture.terminal.columns; column++) {
            const Cell *cell = &fixture.terminal.cells[
                (5 + row) * fixture.terminal.columns + column];

            g_assert_cmpuint(cell->background, ==, backgrounds[row]);
        }
    }
    fixture_clear(&fixture);
}

static void
test_growing_clears_previous_prompt_position(void)
{
    Fixture fixture = { 0 };

    fixture_init(&fixture);
    g_assert_true(fixture_show(&fixture));
    terminal_resize(&fixture.terminal, 80, 16);
    g_assert_true(mux_ui_pane_bridge_set_size(fixture.bridge, 80, 16, NULL));
    assert_clear_rows(&fixture.terminal, 6, 10);
    g_assert_true(mux_ui_pane_bridge_is_active(fixture.bridge));
    fixture_clear(&fixture);
}

static void
test_shrinking_never_addresses_rows_outside_terminal(void)
{
    Fixture fixture = { 0 };

    fixture_init(&fixture);
    g_assert_true(fixture_show(&fixture));
    terminal_resize(&fixture.terminal, 80, 8);
    g_assert_true(mux_ui_pane_bridge_set_size(fixture.bridge, 80, 8, NULL));
    fixture_clear(&fixture);
}

static void
test_zero_size_preserves_pending_cleanup(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GBytes) cancel = mux_ui_cancel_encode(
        17, MUX_UI_CANCEL_NAVIGATION, NULL);
    gsize length;
    const guint8 *data = g_bytes_get_data(cancel, &length);
    guint writes;

    fixture_init(&fixture);
    g_assert_true(fixture_show(&fixture));
    writes = fixture.writes;
    g_assert_true(mux_ui_pane_bridge_set_size(fixture.bridge, 0, 0, NULL));
    g_assert_true(mux_ui_pane_bridge_handle_payload(
        fixture.bridge, data, length, NULL));
    g_assert_cmpuint(fixture.writes, ==, writes);
    terminal_resize(&fixture.terminal, 80, 16);
    g_assert_true(mux_ui_pane_bridge_set_size(fixture.bridge, 80, 16, NULL));
    assert_clear_rows(&fixture.terminal, 1, 16);
    g_assert_false(mux_ui_pane_bridge_is_active(fixture.bridge));
    fixture_clear(&fixture);
}

static void
test_disposal_during_first_write_clears_prompt(void)
{
    Fixture fixture = { 0 };

    fixture_init(&fixture);
    fixture.dispose_on_write = TRUE;
    g_assert_false(fixture_show(&fixture));
    g_assert_null(fixture.bridge);
    g_assert_cmpuint(fixture.destroyed, ==, 1);
    assert_clear_rows(&fixture.terminal, 1, 10);
    fixture_clear(&fixture);
}

static void
test_disposal_during_send_does_not_outlive_user_data(void)
{
    Fixture fixture = { 0 };
    gboolean consumed = FALSE;

    fixture_init(&fixture);
    g_assert_true(fixture_show(&fixture));
    fixture.dispose_on_send = TRUE;
    g_assert_false(mux_ui_pane_bridge_handle_key(
        fixture.bridge, MUX_PANE_OVERLAY_KEY_ENTER, 0, &consumed, NULL));
    g_assert_true(consumed);
    g_assert_null(fixture.bridge);
    g_assert_cmpuint(fixture.sends, ==, 1);
    g_assert_cmpuint(fixture.destroyed, ==, 1);
    assert_clear_rows(&fixture.terminal, 1, 10);
    fixture_clear(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/pane-overlay/render/tiny-terminal",
                    test_tiny_terminal_does_not_wrap);
    g_test_add_func("/pane-overlay/render/opaque-background",
                    test_prompt_background_covers_every_cell);
    g_test_add_func("/pane-overlay/resize/grow",
                    test_growing_clears_previous_prompt_position);
    g_test_add_func("/pane-overlay/resize/shrink",
                    test_shrinking_never_addresses_rows_outside_terminal);
    g_test_add_func("/pane-overlay/resize/zero-size",
                    test_zero_size_preserves_pending_cleanup);
    g_test_add_func("/pane-overlay/lifetime/dispose-during-write",
                    test_disposal_during_first_write_clears_prompt);
    g_test_add_func("/pane-overlay/lifetime/dispose-during-send",
                    test_disposal_during_send_does_not_outlive_user_data);
    return g_test_run();
}
