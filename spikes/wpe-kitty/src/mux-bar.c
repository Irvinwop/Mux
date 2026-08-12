#define _GNU_SOURCE

#include "mux-protocol.h"
#include "mux-shortcuts.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    gchar *uri;
    gchar *title;
    gchar *layer;
} BarView;

typedef struct {
    int daemon_fd;
    struct termios saved_termios;
    gboolean terminal_active;
    GByteArray *daemon_input;
    GByteArray *key_input;
    GHashTable *views;
    gchar *active_id;
    gchar *layer;
    GString *edit;
    gboolean editing;
    gboolean replace_on_type;
    guint columns;
    guint rows;
} Bar;

static volatile sig_atomic_t stop_requested;
static Bar *global_bar;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void bar_view_free(gpointer data)
{
    BarView *view = data;
    g_free(view->uri);
    g_free(view->title);
    g_free(view->layer);
    g_free(view);
}

static gboolean output_text(const gchar *text)
{
    gsize length = strlen(text);
    while (length) {
        ssize_t written = write(STDOUT_FILENO, text, length);
        if (written > 0) {
            text += written;
            length -= (gsize)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return FALSE;
    }
    return TRUE;
}

static gchar *display_text(const gchar *text)
{
    GString *clean = g_string_new(NULL);
    g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);

    for (const gchar *cursor = valid; *cursor;
         cursor = g_utf8_next_char(cursor)) {
        gunichar codepoint = g_utf8_get_char(cursor);
        GUnicodeType type = g_unichar_type(codepoint);

        if (!g_unichar_isprint(codepoint) ||
            type == G_UNICODE_FORMAT ||
            type == G_UNICODE_LINE_SEPARATOR ||
            type == G_UNICODE_PARAGRAPH_SEPARATOR)
            continue;
        g_string_append_unichar(clean, codepoint);
    }
    return g_string_free(clean, FALSE);
}

static guint codepoint_columns(gunichar codepoint)
{
    if (g_unichar_combining_class(codepoint) != 0)
        return 0;
    return g_unichar_iswide(codepoint) ? 2 : 1;
}

static BarView *active_view(Bar *bar)
{
    return bar->active_id
        ? g_hash_table_lookup(bar->views, bar->active_id)
        : NULL;
}

static guint append_padded(
    GString *output,
    const gchar *prefix,
    const gchar *value,
    guint columns)
{
    gchar *clean = display_text(value);
    guint used = 0;
    for (const gchar *cursor = prefix; *cursor && used < columns; cursor++, used++)
        g_string_append_c(output, *cursor);
    for (const gchar *cursor = clean; *cursor;) {
        gunichar codepoint = g_utf8_get_char(cursor);
        guint width = codepoint_columns(codepoint);
        if (width > columns - used)
            break;
        g_string_append_unichar(output, codepoint);
        used += width;
        cursor = g_utf8_next_char(cursor);
    }
    guint content_columns = used;
    while (used++ < columns)
        g_string_append_c(output, ' ');
    g_free(clean);
    return content_columns;
}

static void redraw(Bar *bar)
{
    struct winsize size = { 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    bar->columns = size.ws_col ? size.ws_col : 80;
    bar->rows = size.ws_row ? size.ws_row : 2;

    BarView *view = active_view(bar);
    const gchar *uri = view && view->uri ? view->uri : "no active view";
    const gchar *title = view && view->title && *view->title
        ? view->title
        : "No active page";
    gchar *header = g_strdup_printf(
        " MUX/%s [%u]  Super-L:url  Super-Shift-P:cmd  Super-Shift-V:clip | %s",
        bar->layer ? bar->layer : "main",
        g_hash_table_size(bar->views),
        title);
    guint edit_columns = 0;
    guint rendered_rows = 1;

    GString *output = g_string_new(
        "\033[H\033[48;2;8;22;19m\033[38;2;133;220;170m");
    append_padded(output, "", header, bar->columns);
    if (bar->rows > 1) {
        g_string_append(
            output,
            "\r\n\033[48;2;17;34;29m\033[38;2;222;246;232m");
        edit_columns = append_padded(
            output,
            bar->editing ? " URL> " : " URL  ",
            bar->editing ? bar->edit->str : uri,
            bar->columns);
        rendered_rows = 2;
    }
    if (bar->rows > 2) {
        g_string_append(
            output,
            "\r\n\033[48;2;8;22;19m\033[38;2;154;179;168m");
        append_padded(
            output,
            " ",
            "Super-D bookmark  Super-Shift-Enter right  Super-Alt-Enter down  Super-Shift-T layer  Alt-HJKL pane",
            bar->columns);
        rendered_rows = 3;
    }
    for (guint row = rendered_rows; row < bar->rows; row++) {
        g_string_append(output, "\r\n");
        append_padded(output, "", "", bar->columns);
    }
    if (bar->editing && bar->rows > 1) {
        guint cursor_column = edit_columns < bar->columns
            ? edit_columns + 1
            : bar->columns;
        g_string_append_printf(
            output,
            "\033[2;%uH\033[?25h",
            cursor_column);
    } else {
        g_string_append(output, "\033[?25l");
    }
    output_text(output->str);

    g_string_free(output, TRUE);
    g_free(header);
}

static gboolean terminal_enable(Bar *bar)
{
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &bar->saved_termios) < 0)
        return FALSE;
    struct termios raw = bar->saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return FALSE;
    bar->terminal_active = TRUE;
    output_text("\033[?1049h\033[2J\033[H\033[?25l\033[>31u");
    return TRUE;
}

static void terminal_restore(Bar *bar)
{
    if (!bar || !bar->terminal_active)
        return;
    output_text("\033[<u\033[?25h\033[0m\033[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &bar->saved_termios);
    bar->terminal_active = FALSE;
}

static void restore_at_exit(void)
{
    terminal_restore(global_bar);
}

static void begin_edit(Bar *bar)
{
    BarView *view = active_view(bar);
    g_string_assign(bar->edit, view && view->uri ? view->uri : "");
    bar->editing = TRUE;
    bar->replace_on_type = TRUE;
    redraw(bar);
}

static void cancel_edit(Bar *bar)
{
    bar->editing = FALSE;
    bar->replace_on_type = FALSE;
    mux_send_line(bar->daemon_fd, "CANCEL");
    redraw(bar);
}

static void submit_edit(Bar *bar)
{
    gchar *encoded = mux_encode(bar->edit->str);
    mux_send_line(bar->daemon_fd, "OPEN\t%s", encoded);
    g_free(encoded);
    bar->editing = FALSE;
    bar->replace_on_type = FALSE;
    redraw(bar);
}

static void append_codepoint(Bar *bar, gunichar codepoint)
{
    if (!bar->editing || !g_unichar_validate(codepoint) || g_unichar_iscntrl(codepoint))
        return;
    if (bar->replace_on_type) {
        g_string_truncate(bar->edit, 0);
        bar->replace_on_type = FALSE;
    }
    gchar bytes[7] = { 0 };
    gint length = g_unichar_to_utf8(codepoint, bytes);
    g_string_append_len(bar->edit, bytes, length);
    redraw(bar);
}

static void backspace(Bar *bar)
{
    if (!bar->editing)
        return;
    if (bar->replace_on_type) {
        g_string_truncate(bar->edit, 0);
        bar->replace_on_type = FALSE;
    } else if (bar->edit->len) {
        gchar *previous = g_utf8_find_prev_char(
            bar->edit->str,
            bar->edit->str + bar->edit->len);
        if (previous)
            g_string_truncate(bar->edit, (gsize)(previous - bar->edit->str));
    }
    redraw(bar);
}

static guint parse_uint(const gchar *text, guint fallback)
{
    if (!text || !*text)
        return fallback;
    return (guint)g_ascii_strtoull(text, NULL, 10);
}

static void handle_key(
    Bar *bar,
    guint key,
    guint encoded_modifiers,
    guint event_type,
    const gchar *text)
{
    guint modifiers =
        mux_shortcut_modifiers_from_kitty(encoded_modifiers);
    MuxShortcut shortcut = mux_shortcut_match_bar(modifiers, key);

    if (mux_shortcut_key_is_kitty_functional(key))
        return;
    if (shortcut != MUX_SHORTCUT_NONE) {
        if (event_type == MUX_SHORTCUT_EVENT_PRESS) {
            if (shortcut == MUX_SHORTCUT_LOCATION)
                begin_edit(bar);
            else if (shortcut == MUX_SHORTCUT_BAR_CLEAR && bar->editing) {
                g_string_truncate(bar->edit, 0);
                bar->replace_on_type = FALSE;
                redraw(bar);
            }
        }
        return;
    }
    if (event_type == MUX_SHORTCUT_EVENT_RELEASE)
        return;
    if (!bar->editing)
        return;
    if (key == 13) {
        submit_edit(bar);
        return;
    }
    if (key == 27) {
        cancel_edit(bar);
        return;
    }
    if (key == 127) {
        backspace(bar);
        return;
    }
    if (!(modifiers & (MUX_SHORTCUT_MODIFIER_CONTROL |
                       MUX_SHORTCUT_MODIFIER_ALT |
                       MUX_SHORTCUT_MODIFIER_META)) &&
        text && *text) {
        gchar **codepoints = g_strsplit(text, ":", -1);
        for (guint i = 0; codepoints[i]; i++)
            append_codepoint(bar, parse_uint(codepoints[i], 0));
        g_strfreev(codepoints);
    }
}

static void handle_csi(Bar *bar, const guint8 *data, gsize length, gchar final)
{
    if (final != 'u')
        return;
    gchar *parameters = g_strndup((const gchar *)data, length);
    if (parameters[0] == '?' || parameters[0] == '>' || parameters[0] == '<') {
        g_free(parameters);
        return;
    }

    gchar **fields = g_strsplit(parameters, ";", 3);
    gchar **keys = g_strsplit(fields[0] ? fields[0] : "", ":", 3);
    gchar **mods = g_strsplit(fields[1] ? fields[1] : "", ":", 2);
    handle_key(
        bar,
        parse_uint(keys[0], 0),
        parse_uint(mods[0], 1),
        parse_uint(mods[1], 1),
        fields[2] ? fields[2] : "");
    g_strfreev(mods);
    g_strfreev(keys);
    g_strfreev(fields);
    g_free(parameters);
}

static gboolean parse_one_key(Bar *bar)
{
    GByteArray *input = bar->key_input;
    if (!input->len)
        return FALSE;

    if (input->data[0] == 0x1b) {
        if (input->len == 1)
            return FALSE;
        if (input->data[1] == '[') {
            for (guint i = 2; i < input->len; i++) {
                if (input->data[i] >= 0x40 && input->data[i] <= 0x7e) {
                    handle_csi(bar, input->data + 2, i - 2, input->data[i]);
                    g_byte_array_remove_range(input, 0, i + 1);
                    return TRUE;
                }
            }
            return FALSE;
        }
        g_byte_array_remove_range(input, 0, 1);
        return TRUE;
    }

    gunichar codepoint = g_utf8_get_char_validated(
        (const gchar *)input->data,
        input->len);
    if (codepoint == (gunichar)-2)
        return FALSE;
    if (codepoint == (gunichar)-1) {
        g_byte_array_remove_range(input, 0, 1);
        return TRUE;
    }
    guint length = (guint)(g_utf8_next_char((const gchar *)input->data) -
        (const gchar *)input->data);
    append_codepoint(bar, codepoint);
    g_byte_array_remove_range(input, 0, length);
    return TRUE;
}

static void upsert_view(
    Bar *bar,
    const gchar *id_field,
    const gchar *layer_field,
    const gchar *uri_field,
    const gchar *title_field)
{
    gchar *id = mux_decode(id_field);
    BarView *view = g_hash_table_lookup(bar->views, id);
    if (!view) {
        view = g_new0(BarView, 1);
        g_hash_table_insert(bar->views, g_strdup(id), view);
    }
    g_free(view->layer);
    g_free(view->uri);
    g_free(view->title);
    view->layer = mux_decode(layer_field);
    view->uri = mux_decode(uri_field);
    view->title = mux_decode(title_field);
    g_free(id);
}

static void handle_daemon_line(Bar *bar, const gchar *line)
{
    gchar **fields = g_strsplit(line, "\t", -1);
    guint count = g_strv_length(fields);

    if (count >= 4 && g_strcmp0(fields[0], "BEGIN") == 0) {
        g_free(bar->active_id);
        g_free(bar->layer);
        bar->active_id = mux_decode(fields[2]);
        bar->layer = mux_decode(fields[3]);
    } else if (count >= 8 && g_strcmp0(fields[0], "VIEW") == 0) {
        upsert_view(bar, fields[1], fields[2], fields[3], fields[4]);
    } else if (count >= 10 && g_strcmp0(fields[0], "EVENT") == 0 &&
               g_strcmp0(fields[2], "UPSERT") == 0) {
        upsert_view(bar, fields[3], fields[4], fields[5], fields[6]);
    } else if (count >= 4 && g_strcmp0(fields[0], "EVENT") == 0 &&
               g_strcmp0(fields[2], "REMOVE") == 0) {
        gchar *id = mux_decode(fields[3]);
        g_hash_table_remove(bar->views, id);
        g_free(id);
    } else if (count >= 4 && g_strcmp0(fields[0], "EVENT") == 0 &&
               g_strcmp0(fields[2], "ACTIVE") == 0) {
        g_free(bar->active_id);
        bar->active_id = mux_decode(fields[3]);
    } else if (count >= 4 && g_strcmp0(fields[0], "EVENT") == 0 &&
               g_strcmp0(fields[2], "LAYER") == 0) {
        g_free(bar->layer);
        bar->layer = mux_decode(fields[3]);
    } else if (count >= 2 && g_strcmp0(fields[0], "DO") == 0 &&
               g_strcmp0(fields[1], "EDIT") == 0) {
        begin_edit(bar);
    }

    redraw(bar);
    g_strfreev(fields);
}

static gboolean parse_daemon_lines(Bar *bar)
{
    while (TRUE) {
        guint newline = 0;
        gboolean found = FALSE;
        for (; newline < bar->daemon_input->len; newline++) {
            if (bar->daemon_input->data[newline] == '\n') {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return TRUE;
        gchar *line = g_strndup((const gchar *)bar->daemon_input->data, newline);
        g_strchomp(line);
        handle_daemon_line(bar, line);
        g_free(line);
        g_byte_array_remove_range(bar->daemon_input, 0, newline + 1);
    }
}

static gboolean register_bar(Bar *bar)
{
    gchar *uuid = g_uuid_string_random();
    gchar *id = mux_encode(uuid);
    gchar *window = mux_encode(g_getenv("KITTY_WINDOW_ID"));
    gchar *socket = mux_encode(g_getenv("KITTY_LISTEN_ON"));
    gchar *layer = mux_encode(g_getenv("MUX_LAYER"));
    gboolean result = mux_send_line(
        bar->daemon_fd,
        "BAR\t%s\t%ld\t%s\t%s\t%s",
        id,
        (long)getpid(),
        window,
        socket,
        layer);
    g_free(layer);
    g_free(socket);
    g_free(window);
    g_free(id);
    g_free(uuid);
    return result;
}

int main(void)
{
    Bar bar = {
        .daemon_fd = -1,
        .daemon_input = g_byte_array_new(),
        .key_input = g_byte_array_new(),
        .views = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, bar_view_free),
        .layer = g_strdup(g_getenv("MUX_LAYER") ? g_getenv("MUX_LAYER") : "main"),
        .edit = g_string_new(NULL),
    };
    global_bar = &bar;
    atexit(restore_at_exit);
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);

    bar.daemon_fd = mux_connect_socket();
    if (bar.daemon_fd < 0 || !register_bar(&bar) || !terminal_enable(&bar)) {
        int saved_errno = errno;

        g_printerr("mux-bar: initialization failed: %s\n",
                   g_strerror(saved_errno));
        terminal_restore(&bar);
        if (bar.daemon_fd >= 0)
            close(bar.daemon_fd);
        g_byte_array_unref(bar.daemon_input);
        g_byte_array_unref(bar.key_input);
        g_hash_table_unref(bar.views);
        g_string_free(bar.edit, TRUE);
        g_free(bar.active_id);
        g_free(bar.layer);
        global_bar = NULL;
        return EXIT_FAILURE;
    }
    redraw(&bar);

    while (!stop_requested) {
        struct pollfd poll_fds[2] = {
            { .fd = bar.daemon_fd, .events = POLLIN },
            { .fd = STDIN_FILENO, .events = POLLIN },
        };
        int result;
        do {
            result = poll(poll_fds, 2, -1);
        } while (result < 0 && errno == EINTR && !stop_requested);
        if (result <= 0)
            continue;

        if (poll_fds[0].revents & POLLIN) {
            guint8 buffer[8192];
            ssize_t count = read(bar.daemon_fd, buffer, sizeof(buffer));
            if (count <= 0)
                break;
            g_byte_array_append(bar.daemon_input, buffer, (guint)count);
            parse_daemon_lines(&bar);
        }
        if (poll_fds[0].revents & (POLLHUP | POLLERR))
            break;

        if (poll_fds[1].revents & POLLIN) {
            guint8 buffer[4096];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                g_byte_array_append(bar.key_input, buffer, (guint)count);
                while (parse_one_key(&bar))
                    ;
            }
        }
    }

    mux_send_line(bar.daemon_fd, "BYE");
    terminal_restore(&bar);
    close(bar.daemon_fd);
    g_byte_array_unref(bar.daemon_input);
    g_byte_array_unref(bar.key_input);
    g_hash_table_unref(bar.views);
    g_string_free(bar.edit, TRUE);
    g_free(bar.active_id);
    g_free(bar.layer);
    global_bar = NULL;
    return EXIT_SUCCESS;
}
