#include "mux-bar-render.h"

#include <string.h>

static gchar *
plain_text(const gchar *ansi)
{
    GString *text = g_string_new(NULL);

    for (const gchar *cursor = ansi; *cursor;) {
        if (cursor[0] == '\033' && cursor[1] == '[') {
            cursor += 2;
            while (*cursor && !(*cursor >= 0x40 && *cursor <= 0x7e))
                cursor++;
            if (*cursor)
                cursor++;
        } else {
            g_string_append_c(text, *cursor++);
        }
    }
    return g_string_free(text, FALSE);
}

static void
assert_grid(const gchar *ansi, guint columns, guint rows)
{
    g_autofree gchar *text = plain_text(ansi);
    g_auto(GStrv) lines = g_strsplit(text, "\r\n", -1);

    g_assert_true(g_utf8_validate(text, -1, NULL));
    g_assert_cmpuint(g_strv_length(lines), ==, rows);
    for (guint row = 0; lines[row]; row++) {
        guint width = 0;
        for (const gchar *cursor = lines[row]; *cursor;
             cursor = g_utf8_next_char(cursor)) {
            gunichar character = g_utf8_get_char(cursor);
            if (g_unichar_combining_class(character) == 0)
                width += g_unichar_iswide(character) ? 2u : 1u;
        }
        g_assert_cmpuint(width, ==, columns);
    }
}

static void
test_navigation_hierarchy(void)
{
    MuxBarRenderState state = {
        .title = "Example page", .uri = "https://example.com/path",
        .columns = 100, .rows = 2,
    };
    g_autofree gchar *rendered = mux_bar_render(&state);
    g_autofree gchar *text = plain_text(rendered);

    g_assert_nonnull(strstr(text, "Example page"));
    g_assert_nonnull(strstr(text, "https://example.com/path"));
    g_assert_nonnull(strstr(text, "Super-L address"));
    g_assert_null(strstr(text, "Super-W"));
    g_assert_null(strstr(text, "MUX/"));
    assert_grid(rendered, state.columns, state.rows);
}

static void
test_editing_feedback(void)
{
    MuxBarRenderState state = {
        .edit = "https://example.com/path", .columns = 80, .rows = 2,
        .editing = TRUE, .replace_on_type = TRUE,
    };
    g_autofree gchar *rendered = mux_bar_render(&state);
    g_autofree gchar *text = plain_text(rendered);

    g_assert_nonnull(strstr(text, "Replace address"));
    g_assert_nonnull(strstr(text, "Enter open  Esc cancel"));
    g_assert_nonnull(strstr(text, "URL>"));
    g_assert_nonnull(strstr(rendered, "\033[7m"));
    g_assert_nonnull(strstr(rendered, "\033[?25h"));
    assert_grid(rendered, state.columns, state.rows);
}

static void
test_long_edit_keeps_tail_and_value(void)
{
    const gchar *address = "https://example.com/a/very/long/path/to/the-end";
    MuxBarRenderState state = {
        .edit = address, .columns = 22, .rows = 2, .editing = TRUE,
    };
    g_autofree gchar *rendered = mux_bar_render(&state);
    g_autofree gchar *text = plain_text(rendered);

    g_assert_nonnull(strstr(text, "URL> <"));
    g_assert_nonnull(strstr(text, "the-end"));
    g_assert_true(state.edit == address);
    g_assert_nonnull(strstr(rendered, "\033[2;22H"));
    assert_grid(rendered, state.columns, state.rows);
}

static void
test_readonly_keeps_origin(void)
{
    MuxBarRenderState state = {
        .uri = "https://example.com/a/very/long/path/that/does/not/fit",
        .columns = 36, .rows = 1,
    };
    g_autofree gchar *rendered = mux_bar_render(&state);
    g_autofree gchar *text = plain_text(rendered);

    g_assert_nonnull(strstr(text, "https://example.com/"));
    g_assert_nonnull(strstr(text, "..."));
    assert_grid(rendered, state.columns, state.rows);
}

static void
test_small_and_unicode_sizes(void)
{
    for (guint columns = 1; columns <= 100; columns++) {
        for (guint rows = 1; rows <= 4; rows++) {
            for (guint editing = 0; editing <= 1; editing++) {
                MuxBarRenderState state = {
                    .title = "Example \xe7\x95\x8c e\xcc\x81 page",
                    .uri = "https://example.com/\xe7\x95\x8c/e\xcc\x81/path",
                    .edit = "https://example.com/\xe7\x95\x8c/e\xcc\x81/path",
                    .columns = columns, .rows = rows,
                    .editing = editing, .replace_on_type = editing,
                };
                g_autofree gchar *rendered = mux_bar_render(&state);
                assert_grid(rendered, columns, rows);
            }
        }
    }
}

static void
test_untrusted_terminal_text(void)
{
    MuxBarRenderState state = {
        .title = "Page\033]52;c;payload\a\n\xe2\x80\xae title",
        .uri = "https://example.com/\tpath",
        .columns = 100, .rows = 2,
    };
    g_autofree gchar *rendered = mux_bar_render(&state);

    g_assert_null(strstr(rendered, "\033]52"));
    g_assert_null(strchr(rendered, '\a'));
    g_assert_null(strchr(rendered, '\t'));
    g_assert_null(strstr(rendered, "\xe2\x80\xae"));
    assert_grid(rendered, state.columns, state.rows);
}

static void
test_zero_geometry(void)
{
    MuxBarRenderState state = { .columns = 0, .rows = 2 };
    g_autofree gchar *zero_columns = mux_bar_render(&state);
    g_assert_cmpstr(zero_columns, ==, "");
    state.columns = 80;
    state.rows = 0;
    g_autofree gchar *zero_rows = mux_bar_render(&state);
    g_assert_cmpstr(zero_rows, ==, "");
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/bar/navigation-hierarchy", test_navigation_hierarchy);
    g_test_add_func("/bar/editing-feedback", test_editing_feedback);
    g_test_add_func("/bar/long-edit", test_long_edit_keeps_tail_and_value);
    g_test_add_func("/bar/origin", test_readonly_keeps_origin);
    g_test_add_func("/bar/small-unicode", test_small_and_unicode_sizes);
    g_test_add_func("/bar/untrusted-text", test_untrusted_terminal_text);
    g_test_add_func("/bar/zero-geometry", test_zero_geometry);
    return g_test_run();
}
