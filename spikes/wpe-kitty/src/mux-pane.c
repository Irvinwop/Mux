#define _GNU_SOURCE

#include "mux-engine-protocol.h"
#include "mux-shortcuts.h"
#include "mux-kitty-chooser.h"
#include "mux-protocol.h"
#include "mux-pane-clipboard.h"
#include "mux-ui-pane.h"
#include "mux-notification-pane.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WPE_MODIFIER_CONTROL (1u << 0)
#define WPE_MODIFIER_SHIFT (1u << 1)
#define WPE_MODIFIER_ALT (1u << 2)
#define WPE_MODIFIER_META (1u << 3)
#define WPE_MODIFIER_CAPS_LOCK (1u << 4)
#define WPE_MODIFIER_BUTTON1 (1u << 8)
#define WPE_MODIFIER_BUTTON2 (1u << 9)
#define WPE_MODIFIER_BUTTON3 (1u << 10)

#define KEY_BACKSPACE 0xff08u
#define KEY_TAB 0xff09u
#define KEY_RETURN 0xff0du
#define KEY_ESCAPE 0xff1bu
#define KEY_HOME 0xff50u
#define KEY_LEFT 0xff51u
#define KEY_UP 0xff52u
#define KEY_RIGHT 0xff53u
#define KEY_DOWN 0xff54u
#define KEY_PAGE_UP 0xff55u
#define KEY_PAGE_DOWN 0xff56u
#define KEY_END 0xff57u
#define KEY_INSERT 0xff63u
#define KEY_DELETE 0xffffu
#define KEY_F1 0xffbeu

#define RECONNECT_INITIAL_MS 100u
#define RECONNECT_MAX_MS 5000u
#define ACTIVE_MAINTENANCE_MS 50
#define IDLE_MAINTENANCE_MS 250
#define CLOSE_TIMEOUT_MS 120000
#define KITTY_FRAME_RESPONSE_CAPACITY 32u
#define TERMINAL_OUTPUT_CAP_BYTES (4u * 1024u * 1024u)
#define TERMINAL_OUTPUT_FRAME_RESERVE_BYTES (128u * 1024u)
#define TERMINAL_OUTPUT_DRAIN_TIMEOUT_MS 250
#define ENGINE_ERROR_DETAIL_MAX_BYTES 4096u
#define ENGINE_ERROR_WIRE_MAX_BYTES (8u + ENGINE_ERROR_DETAIL_MAX_BYTES)
#define KITTY_GRAPHICS_RESPONSE_MAX_BYTES 2048u
#define KITTY_BROWSER_Z_INDEX (-1)

typedef enum {
    KITTY_GRAPHICS_RESPONSE_IGNORE,
    KITTY_GRAPHICS_RESPONSE_SUCCESS,
    KITTY_GRAPHICS_RESPONSE_ERROR,
} KittyGraphicsResponseResult;

static gboolean
schedule_retry_at(gint64 now_us, gint64 *retry_us, guint *backoff_ms)
{
    guint delay_ms;

    if (*retry_us)
        return FALSE;
    delay_ms = *backoff_ms ? *backoff_ms : RECONNECT_INITIAL_MS;
    *retry_us = now_us + (gint64)delay_ms * 1000;
    *backoff_ms = MIN(delay_ms * 2, RECONNECT_MAX_MS);
    return TRUE;
}

static gchar *
sanitize_engine_diagnostic(const gchar *detail)
{
    GString *safe = g_string_sized_new(strlen(detail));
    const gchar *cursor = detail;

    while (*cursor) {
        gunichar character = g_utf8_get_char(cursor);
        GUnicodeType type = g_unichar_type(character);

        if (g_unichar_iscntrl(character) || type == G_UNICODE_FORMAT)
            g_string_append_c(safe, '?');
        else
            g_string_append_unichar(safe, character);
        cursor = g_utf8_next_char(cursor);
    }
    if (!safe->len)
        g_string_append(safe, "engine error");
    return g_string_free(safe, FALSE);
}

static gboolean
decode_engine_error_payload(GBytes *payload,
                            guint32 *code,
                            gchar **safe_detail)
{
    MuxEngineCursor cursor;
    g_autofree gchar *detail = NULL;
    guint32 decoded_code = 0;
    gsize payload_size;

    g_return_val_if_fail(code != NULL, FALSE);
    g_return_val_if_fail(safe_detail != NULL, FALSE);
    *code = 0;
    *safe_detail = NULL;
    if (!payload)
        return FALSE;
    payload_size = g_bytes_get_size(payload);
    if (payload_size < 8 || payload_size > ENGINE_ERROR_WIRE_MAX_BYTES)
        return FALSE;

    mux_engine_cursor_init(&cursor, payload);
    if (!mux_engine_cursor_get_u32(&cursor, &decoded_code) ||
        !decoded_code ||
        !mux_engine_cursor_get_string(&cursor, &detail) ||
        !mux_engine_cursor_done(&cursor) ||
        strlen(detail) > ENGINE_ERROR_DETAIL_MAX_BYTES ||
        !g_utf8_validate(detail, -1, NULL))
        return FALSE;

    *code = decoded_code;
    *safe_detail = sanitize_engine_diagnostic(detail);
    return TRUE;
}

static gchar *
sanitize_kitty_graphics_detail(const gchar *detail)
{
    g_autofree gchar *valid = g_utf8_make_valid(detail ? detail : "", -1);
    GString *safe = g_string_sized_new(strlen(valid));
    const gchar *cursor;

    for (cursor = valid; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        GUnicodeType type = g_unichar_type(character);

        if (g_unichar_iscntrl(character) || type == G_UNICODE_FORMAT)
            g_string_append_c(safe, '?');
        else
            g_string_append_unichar(safe, character);
    }
    if (safe->len > MUX_ENGINE_MAX_FRAME_REJECTION_BYTES) {
        gsize length = MUX_ENGINE_MAX_FRAME_REJECTION_BYTES;

        while (length && !g_utf8_validate(safe->str, length, NULL))
            length--;
        g_string_truncate(safe, length);
    }
    if (!safe->len)
        g_string_append(safe, "Kitty graphics error");
    return g_string_free(safe, FALSE);
}

static KittyGraphicsResponseResult
parse_kitty_graphics_response(const guint8 *sequence,
                              gsize length,
                              guint *image_id,
                              gchar **detail)
{
    g_autofree gchar *response = NULL;
    g_auto(GStrv) parameters = NULL;
    gchar *start;
    gchar *status;
    gchar *semicolon;
    gchar *terminator;
    guint64 parsed_id = 0;
    gboolean found_id = FALSE;

    g_return_val_if_fail(image_id != NULL,
                         KITTY_GRAPHICS_RESPONSE_IGNORE);
    g_return_val_if_fail(detail != NULL,
                         KITTY_GRAPHICS_RESPONSE_IGNORE);
    *image_id = 0;
    *detail = NULL;
    if (!sequence || !length ||
        length > KITTY_GRAPHICS_RESPONSE_MAX_BYTES)
        return KITTY_GRAPHICS_RESPONSE_IGNORE;

    response = g_strndup((const gchar *)sequence, length);
    start = response;
    if (length >= 3 && start[0] == '\033' && start[1] == '_' &&
        start[2] == 'G')
        start += 3;
    semicolon = strchr(start, ';');
    if (!semicolon)
        return KITTY_GRAPHICS_RESPONSE_IGNORE;
    *semicolon = '\0';
    status = semicolon + 1;
    terminator = strpbrk(status, "\033\a");
    if (terminator)
        *terminator = '\0';
    if (!*status)
        return KITTY_GRAPHICS_RESPONSE_IGNORE;

    parameters = g_strsplit(start, ",", -1);
    for (guint index = 0; parameters[index]; index++) {
        gchar *end = NULL;
        guint64 value;

        if (!g_str_has_prefix(parameters[index], "i="))
            continue;
        if (found_id || !parameters[index][2])
            return KITTY_GRAPHICS_RESPONSE_IGNORE;
        errno = 0;
        value = g_ascii_strtoull(parameters[index] + 2, &end, 10);
        if (errno || !end || *end || !value || value > G_MAXUINT)
            return KITTY_GRAPHICS_RESPONSE_IGNORE;
        parsed_id = value;
        found_id = TRUE;
    }
    if (!found_id)
        return KITTY_GRAPHICS_RESPONSE_IGNORE;

    *image_id = (guint)parsed_id;
    if (g_str_equal(status, "OK"))
        return KITTY_GRAPHICS_RESPONSE_SUCCESS;
    *detail = sanitize_kitty_graphics_detail(status);
    return KITTY_GRAPHICS_RESPONSE_ERROR;
}

static gchar *
build_kitty_frame_command(gboolean image_present,
                          guint32 message_flags,
                          guint32 width,
                          guint32 height,
                          guint32 x,
                          guint32 y,
                          guint32 rectangle_width,
                          guint32 rectangle_height,
                          guint64 shm_size,
                          guint image_id,
                          const gchar *encoded_name)
{
    if (!image_present ||
        (message_flags & MUX_ENGINE_FLAG_FULL_DAMAGE)) {
        return g_strdup_printf(
            "\033[H\033_Ga=T,f=32,t=s,s=%u,v=%u,S=%" G_GUINT64_FORMAT
            ",i=%u,z=%d,q=0,C=1;%s\033\\",
            width,
            height,
            shm_size,
            image_id,
            KITTY_BROWSER_Z_INDEX,
            encoded_name);
    }

    /*
     * a=f edits the root frame in place, so it retains the z-index from the
     * a=T placement. For a=f, Kitty defines z as an animation frame delay.
     */
    return g_strdup_printf(
        "\033_Ga=f,f=32,t=s,s=%u,v=%u,S=%" G_GUINT64_FORMAT
        ",i=%u,r=1,x=%u,y=%u,X=1,q=0;%s\033\\",
        rectangle_width,
        rectangle_height,
        shm_size,
        image_id,
        x,
        y,
        encoded_name);
}

static gchar *
find_overlay_label(const gchar *query,
                   MuxEngineFindStatus status,
                   guint matches,
                   guint columns)
{
    static const gchar prefix[] = "MUX FIND | ";
    g_autofree gchar *status_text = NULL;
    GString *label = g_string_sized_new(MIN(columns, 256u));
    const gchar *cursor;
    guint status_width;
    guint prefix_budget;
    guint query_budget;
    guint query_width = 0;

    if (status == MUX_ENGINE_FIND_IDLE)
        status_text = g_strdup("type to search");
    else if (status == MUX_ENGINE_FIND_PENDING)
        status_text = g_strdup("searching");
    else if (status == MUX_ENGINE_FIND_FOUND)
        status_text = g_strdup_printf("%u match%s",
                                      matches,
                                      matches == 1 ? "" : "es");
    else if (status == MUX_ENGINE_FIND_NOT_FOUND)
        status_text = g_strdup("no matches");
    else
        status_text = g_strdup("closed");

    status_width = strlen(status_text);
    if (!columns)
        return g_string_free(label, FALSE);
    if (columns <= status_width) {
        g_string_append_len(label, status_text, columns);
        return g_string_free(label, FALSE);
    }

    /* Preserve the complete status before allocating any space to query. */
    prefix_budget = columns - status_width - 1u;
    if (prefix_budget) {
        guint prefix_width = MIN(prefix_budget, (guint)strlen(prefix));

        g_string_append_len(label, prefix, prefix_width);
        prefix_budget -= prefix_width;
    }
    query_budget = prefix_budget;
    cursor = query ? query : "";
    while (*cursor && query_width < query_budget) {
        gunichar character = g_utf8_get_char_validated(cursor, -1);
        guint width;

        if (character == (gunichar)-1 || character == (gunichar)-2) {
            character = '?';
            cursor++;
        } else {
            cursor = g_utf8_next_char(cursor);
            if (!g_unichar_isprint(character))
                character = '?';
        }
        width = g_unichar_iszerowidth(character)
            ? 0
            : g_unichar_iswide(character) ? 2 : 1;
        if (query_width + width > query_budget)
            break;
        g_string_append_unichar(label, character);
        query_width += width;
    }
    if (label->len)
        g_string_append_c(label, ' ');
    g_string_append(label, status_text);
    return g_string_free(label, FALSE);
}

#ifdef MUX_PANE_LOGIC_TEST

gboolean
mux_pane_test_schedule_retry_at(gint64 now_us,
                                gint64 *retry_us,
                                guint *backoff_ms)
{
    return schedule_retry_at(now_us, retry_us, backoff_ms);
}

gboolean
mux_pane_test_decode_engine_error(GBytes *payload,
                                  guint32 *code,
                                  gchar **safe_detail)
{
    return decode_engine_error_payload(payload, code, safe_detail);
}

gint
mux_pane_test_parse_kitty_graphics_response(const guint8 *sequence,
                                            gsize length,
                                            guint *image_id,
                                            gchar **detail)
{
    return parse_kitty_graphics_response(sequence,
                                         length,
                                         image_id,
                                         detail);
}

gchar *
mux_pane_test_build_kitty_frame_command(gboolean image_present,
                                         guint32 message_flags,
                                         guint32 width,
                                         guint32 height,
                                         guint32 x,
                                         guint32 y,
                                         guint32 rectangle_width,
                                         guint32 rectangle_height,
                                         guint64 shm_size,
                                         guint image_id,
                                         const gchar *encoded_name)
{
    return build_kitty_frame_command(image_present,
                                     message_flags,
                                     width,
                                     height,
                                     x,
                                     y,
                                     rectangle_width,
                                     rectangle_height,
                                     shm_size,
                                     image_id,
                                     encoded_name);
}

gchar *
mux_pane_test_find_overlay_label(const gchar *query,
                                 MuxEngineFindStatus status,
                                 guint matches,
                                 guint columns)
{
    return find_overlay_label(query, status, matches, columns);
}

#else

/*
 * The PTY queue is capped at 4 MiB. Clipboard and UI output stop at the
 * 128 KiB reserve so one complete Kitty graphics command can still enter the
 * same ordered FIFO. Queue admission is all-or-nothing for every command.
 */

typedef struct {
    GBytes *bytes;
    gsize offset;
} TerminalOutputChunk;

typedef struct {
    guint16 event_type;
    guint modifiers;
    guint keyval;
} DelayedPasteKey;

typedef struct {
    guint image_id;
    guint64 frame_serial;
    gboolean retired;
} KittyFrameResponse;

typedef struct {
    int engine_fd;
    int control_fd;
    int events_fd;
    guint64 view_id;
    guint64 next_serial;
    guint64 pending_frame_serial;
    guint64 close_request_serial;
    guint64 retired_close_serial;
    gint64 close_deadline_us;
    guint image_id;
    guint next_image_id;
    guint width;
    guint height;
    guint scale_milli;
    guint64 find_generation;
    guint find_matches;
    MuxEngineFindStatus find_status;
    guint pointer_modifiers;
    gboolean image_present;
    gboolean frame_waiting;
    gboolean engine_visibility_known;
    gboolean engine_visible;
    gboolean engine_handshake_complete;
    gboolean terminal_active;
    gboolean visible;
    gboolean focused;
    gboolean ephemeral;
    gboolean find_active;
    gboolean find_overlay_visible;
    gboolean shutting_down;
    gboolean quit;
    struct termios saved_terminal;
    int saved_terminal_output_flags;
    GQueue terminal_output;
    gsize terminal_output_bytes;
    gboolean terminal_output_failed;
    gboolean terminal_failure_reported;
    GError *terminal_output_error;
    gboolean clipboard_write_active;
    gint64 clipboard_tick_due_us;
    DelayedPasteKey delayed_paste_keys[
        MUX_PANE_CLIPBOARD_MAX_PENDING_PASTES];
    guint delayed_paste_count;
    guint64 next_fresh_paste_request_id;
    guint64 fresh_paste_request_id;
    GByteArray *input;
    GByteArray *control_input;
    GByteArray *events_input;
    gchar *profile;
    gchar *socket_path;
    gchar *layer;
    gchar *uri;
    gchar *title;
    gchar *control_id;
    gchar *active_layer;
    gchar *popup_token;
    gchar *find_query;
    guint engine_backoff_ms;
    guint control_backoff_ms;
    guint events_backoff_ms;
    gint64 engine_retry_us;
    gint64 control_retry_us;
    gint64 events_retry_us;
    GMainContext *main_context;
    MuxPaneClipboard *clipboard;
    MuxKittyChooser *chooser;
    MuxUiPaneBridge *ui_bridge;
    MuxNotificationPane *notifications;
    KittyFrameResponse frame_responses[KITTY_FRAME_RESPONSE_CAPACITY];
    guint frame_response_head;
    guint frame_response_count;
} Pane;

static volatile sig_atomic_t resize_requested;
static volatile sig_atomic_t quit_requested;

static void find_overlay_repaint(Pane *pane);
static void find_overlay_hide(Pane *pane);

static void send_navigation(Pane *pane,
                            MuxEngineNavigationAction action,
                            const gchar *uri);
static void delete_image(Pane *pane);
static void send_resize(Pane *pane);
static void acknowledge_graphics_response(Pane *pane, guint image_id);
static void redraw_clipboard_picker(Pane *pane);
static void disconnect_engine(Pane *pane);
static void retire_close_request(Pane *pane);
static void handle_runtime_engine_error(Pane *pane,
                                        const MuxEngineMessage *message);

static void
signal_resize(int signal_number)
{
    (void)signal_number;
    resize_requested = 1;
}

static void
signal_quit(int signal_number)
{
    (void)signal_number;
    quit_requested = 1;
}

static gboolean
write_all(int fd, const void *data, gsize length)
{
    const guint8 *bytes = data;

    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        bytes += written;
        length -= (gsize)written;
    }
    return TRUE;
}

static void
terminal_output_chunk_free(TerminalOutputChunk *chunk)
{
    if (chunk == NULL)
        return;
    g_bytes_unref(chunk->bytes);
    g_free(chunk);
}

static void
terminal_output_clear(Pane *pane)
{
    TerminalOutputChunk *chunk;

    while ((chunk = g_queue_pop_head(&pane->terminal_output)) != NULL)
        terminal_output_chunk_free(chunk);
    pane->terminal_output_bytes = 0;
}

static void
terminal_output_copy_error(Pane *pane, GError **error)
{
    if (error != NULL && *error == NULL && pane->terminal_output_error != NULL)
        *error = g_error_copy(pane->terminal_output_error);
}

static void
terminal_output_fail(Pane *pane, gint error_number, const gchar *operation)
{
    if (pane->terminal_output_failed)
        return;
    pane->terminal_output_failed = TRUE;
    pane->terminal_output_error = g_error_new(
        G_IO_ERROR,
        g_io_error_from_errno(error_number),
        "%s: %s",
        operation,
        g_strerror(error_number));
    terminal_output_clear(pane);
}

static gboolean
terminal_output_flush(Pane *pane, GError **error)
{
    if (pane->terminal_output_failed) {
        terminal_output_copy_error(pane, error);
        return FALSE;
    }

    while (!g_queue_is_empty(&pane->terminal_output)) {
        TerminalOutputChunk *chunk = g_queue_peek_head(&pane->terminal_output);
        gsize length = 0;
        const guint8 *data = g_bytes_get_data(chunk->bytes, &length);
        ssize_t written;

        if (chunk->offset >= length) {
            g_queue_pop_head(&pane->terminal_output);
            terminal_output_chunk_free(chunk);
            continue;
        }
        written = write(STDOUT_FILENO,
                        data + chunk->offset,
                        length - chunk->offset);
        if (written > 0) {
            chunk->offset += (gsize)written;
            pane->terminal_output_bytes -= (gsize)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return TRUE;
        terminal_output_fail(pane,
                             written == 0 ? EIO : errno,
                             "write pane terminal output");
        terminal_output_copy_error(pane, error);
        return FALSE;
    }
    return TRUE;
}

static gboolean
terminal_output_enqueue_bytes(Pane *pane,
                              GBytes *bytes,
                              gsize reserve_bytes,
                              GError **error)
{
    TerminalOutputChunk *chunk;
    gsize length;
    gsize limit;

    g_return_val_if_fail(bytes != NULL, FALSE);
    if (!terminal_output_flush(pane, error))
        return FALSE;

    length = g_bytes_get_size(bytes);
    limit = TERMINAL_OUTPUT_CAP_BYTES - reserve_bytes;
    if (length > limit || pane->terminal_output_bytes > limit - length) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_WOULD_BLOCK,
                    "terminal output queue is full (%" G_GSIZE_FORMAT
                    "/%u bytes)",
                    pane->terminal_output_bytes,
                    TERMINAL_OUTPUT_CAP_BYTES);
        return FALSE;
    }
    if (length == 0)
        return TRUE;

    chunk = g_new0(TerminalOutputChunk, 1);
    chunk->bytes = g_bytes_ref(bytes);
    g_queue_push_tail(&pane->terminal_output, chunk);
    pane->terminal_output_bytes += length;
    return terminal_output_flush(pane, error);
}

static gboolean
terminal_output_enqueue(Pane *pane,
                        const void *data,
                        gsize length,
                        gsize reserve_bytes,
                        GError **error)
{
    GBytes *bytes = g_bytes_new(data, length);
    gboolean result = terminal_output_enqueue_bytes(pane,
                                                    bytes,
                                                    reserve_bytes,
                                                    error);

    g_bytes_unref(bytes);
    return result;
}

static gboolean
terminal_output_drain(Pane *pane, guint timeout_ms, GError **error)
{
    gint64 deadline_us = g_get_monotonic_time() +
        (gint64)timeout_ms * 1000;

    while (!g_queue_is_empty(&pane->terminal_output)) {
        struct pollfd descriptor = {
            .fd = STDOUT_FILENO,
            .events = POLLOUT,
        };
        gint64 remaining_us;
        gint wait_ms;
        int ready;

        if (!terminal_output_flush(pane, error))
            return FALSE;
        if (g_queue_is_empty(&pane->terminal_output))
            return TRUE;
        remaining_us = deadline_us - g_get_monotonic_time();
        if (remaining_us <= 0) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "timed out draining pane terminal output");
            return FALSE;
        }
        wait_ms = (gint)MIN((remaining_us + 999) / 1000,
                            (gint64)G_MAXINT);
        ready = poll(&descriptor, 1, wait_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            if (ready == 0)
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_TIMED_OUT,
                                    "timed out draining pane terminal output");
            else
                g_set_error(error,
                            G_IO_ERROR,
                            g_io_error_from_errno(errno),
                            "poll pane terminal output: %s",
                            g_strerror(errno));
            return FALSE;
        }
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            terminal_output_fail(pane,
                                 EPIPE,
                                 "poll pane terminal output");
            terminal_output_copy_error(pane, error);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
terminal_write(Pane *pane, const gchar *text)
{
    g_autoptr(GError) error = NULL;

    if (terminal_output_enqueue(pane,
                                text,
                                strlen(text),
                                TERMINAL_OUTPUT_FRAME_RESERVE_BYTES,
                                &error))
        return TRUE;
    g_warning("pane terminal output rejected: %s",
              error != NULL ? error->message : "unknown error");
    return FALSE;
}

static void
find_overlay_write(Pane *pane, const gchar *label)
{
    struct winsize window = { 0 };
    g_autoptr(GString) command = NULL;
    g_autoptr(GError) error = NULL;

    if (!pane->terminal_active ||
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) < 0 ||
        !window.ws_row)
        return;
    command = g_string_new("\033[?2026h\0337");
    g_string_append_printf(command,
                           "\033[%u;1H\033[2K",
                           (guint)window.ws_row);
    if (label && *label) {
        g_string_append(command, "\033[1;30;46m");
        g_string_append(command, label);
        g_string_append(command, "\033[0m");
    }
    g_string_append(command, "\0338\033[?2026l");
    if (!terminal_output_enqueue(pane,
                                 command->str,
                                 command->len,
                                 TERMINAL_OUTPUT_FRAME_RESERVE_BYTES,
                                 &error))
        g_warning("find overlay output failed: %s",
                  error ? error->message : "unknown error");
}

static void
find_overlay_hide(Pane *pane)
{
    if (!pane->find_overlay_visible)
        return;
    find_overlay_write(pane, NULL);
    pane->find_overlay_visible = FALSE;
}

static void
find_overlay_repaint(Pane *pane)
{
    struct winsize window = { 0 };
    g_autofree gchar *label = NULL;

    if (!pane->find_active) {
        find_overlay_hide(pane);
        return;
    }
    if (!pane->terminal_active || !pane->visible ||
        (pane->ui_bridge &&
         mux_ui_pane_bridge_is_active(pane->ui_bridge)) ||
        (pane->clipboard &&
         mux_pane_clipboard_picker_is_open(pane->clipboard))) {
        pane->find_overlay_visible = FALSE;
        return;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) < 0 ||
        !window.ws_col)
        return;
    label = find_overlay_label(pane->find_query,
                               pane->find_status,
                               pane->find_matches,
                               window.ws_col);
    find_overlay_write(pane, label);
    pane->find_overlay_visible = TRUE;
}

static gboolean
terminal_write_critical(Pane *pane, const gchar *text)
{
    g_autoptr(GError) error = NULL;

    if (terminal_output_enqueue(pane,
                                text,
                                strlen(text),
                                0,
                                &error))
        return TRUE;
    g_warning("critical pane terminal output rejected: %s",
              error != NULL ? error->message : "unknown error");
    return FALSE;
}

static gboolean
decode_engine_error_message(const MuxEngineMessage *message,
                            guint32 *code,
                            gchar **safe_detail)
{
    return message->flags == MUX_ENGINE_FLAG_NONE &&
        decode_engine_error_payload(message->payload, code, safe_detail);
}

static void
show_engine_diagnostic(Pane *pane,
                       guint32 code,
                       const gchar *safe_detail,
                       gboolean reconnecting)
{
    g_autofree gchar *overlay = NULL;
    const gchar *next_step = reconnecting
        ? "The pane is reconnecting with bounded backoff."
        : "Use the URL bar, reload, or close the pane.";

    g_printerr("mux-pane: engine error %u: %s\n", code, safe_detail);
    if (!pane->terminal_active)
        return;
    delete_image(pane);
    overlay = g_strdup_printf(
        "\033[2J\033[H\033[1;1HMux engine error (%u)\r\n\r\n%s\r\n\r\n%s\033[?25l",
        code,
        safe_detail,
        next_step);
    (void)terminal_write_critical(pane, overlay);
}

static gboolean
clipboard_terminal_output(MuxPaneClipboard *clipboard,
                          GBytes *packet,
                          gpointer user_data,
                          GError **error)
{
    Pane *pane = user_data;
    gsize length;
    gconstpointer data = g_bytes_get_data(packet, &length);
    gint64 now_us = g_get_monotonic_time();

    (void) clipboard;
    if (g_strstr_len(data, length, "type=write") != NULL) {
        gint64 active_due_us = now_us +
            (gint64)ACTIVE_MAINTENANCE_MS * 1000;

        pane->clipboard_write_active = TRUE;
        if (!pane->clipboard_tick_due_us ||
            pane->clipboard_tick_due_us > active_due_us)
            pane->clipboard_tick_due_us = active_due_us;
    }
    return terminal_output_enqueue_bytes(
        pane,
        packet,
        TERMINAL_OUTPUT_FRAME_RESERVE_BYTES,
        error);
}

static guint32
event_time(void)
{
    struct timespec now;
    guint64 milliseconds;

    clock_gettime(CLOCK_MONOTONIC, &now);
    milliseconds = (guint64)now.tv_sec * 1000 +
        (guint64)now.tv_nsec / 1000000;
    return (guint32)milliseconds;
}

static gchar *
runtime_directory(void)
{
    const gchar *xdg_runtime = g_getenv("XDG_RUNTIME_DIR");

    if (xdg_runtime && g_path_is_absolute(xdg_runtime))
        return g_build_filename(xdg_runtime, "mux", NULL);
    return g_strdup_printf("/tmp/mux-%u", (guint)getuid());
}

static gchar *
engine_socket_path(const gchar *profile)
{
    const gchar *override = g_getenv("MUX_ENGINE_SOCKET");
    gchar *directory;
    gchar *filename;
    gchar *path;

    if (override && *override)
        return g_strdup(override);
    directory = runtime_directory();
    filename = g_strdup_printf("mux-engine-v%u-%s.sock",
                               MUX_ENGINE_VERSION,
                               profile);
    path = g_build_filename(directory, filename, NULL);
    g_free(filename);
    g_free(directory);
    return path;
}

static gchar *
muxd_socket_path(void)
{
    gchar *directory = runtime_directory();
    gchar *path = g_build_filename(directory, "muxd.sock", NULL);

    g_free(directory);
    return path;
}

static void
queue_retry(gint64 *retry_us, guint *backoff_ms)
{
    (void)schedule_retry_at(g_get_monotonic_time(),
                            retry_us,
                            backoff_ms);
}

static void
reset_retry(gint64 *retry_us, guint *backoff_ms)
{
    *retry_us = 0;
    *backoff_ms = RECONNECT_INITIAL_MS;
}

static void
retire_frame_responses(Pane *pane)
{
    for (guint offset = 0;
         offset < pane->frame_response_count;
         offset++) {
        guint index = (pane->frame_response_head + offset) %
            KITTY_FRAME_RESPONSE_CAPACITY;

        pane->frame_responses[index].retired = TRUE;
    }
    pane->frame_waiting = FALSE;
    pane->pending_frame_serial = 0;
}

static void
retire_close_request(Pane *pane)
{
    if (pane->close_request_serial > pane->retired_close_serial)
        pane->retired_close_serial = pane->close_request_serial;
    pane->close_request_serial = 0;
    pane->close_deadline_us = 0;
}

static void
handle_runtime_engine_error(Pane *pane,
                            const MuxEngineMessage *message)
{
    g_autofree gchar *safe_detail = NULL;
    guint32 code = 0;
    gboolean state_lost;

    if (!decode_engine_error_message(message, &code, &safe_detail)) {
        code = MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE;
        safe_detail = g_strdup("malformed engine error response");
        state_lost = TRUE;
    } else {
        state_lost = message->view_id == 0 ||
            message->view_id != pane->view_id ||
            code == MUX_ENGINE_REMOTE_ERROR_NOT_FOUND ||
            code == MUX_ENGINE_REMOTE_ERROR_NOT_OWNER;
    }
    if (message->serial &&
        message->serial == pane->close_request_serial)
        retire_close_request(pane);
    show_engine_diagnostic(pane, code, safe_detail, state_lost);
    if (state_lost)
        disconnect_engine(pane);
}

static void
disconnect_engine(Pane *pane)
{
    find_overlay_hide(pane);
    pane->find_active = FALSE;
    pane->find_generation = 0;
    pane->find_status = MUX_ENGINE_FIND_CLOSED;
    pane->find_matches = 0;
    g_clear_pointer(&pane->find_query, g_free);
    if (pane->engine_fd >= 0)
        close(pane->engine_fd);
    pane->engine_fd = -1;
    pane->view_id = 0;
    pane->engine_visibility_known = FALSE;
    pane->engine_handshake_complete = FALSE;
    retire_frame_responses(pane);
    delete_image(pane);
    retire_close_request(pane);
    if (!pane->shutting_down)
        queue_retry(&pane->engine_retry_us,
                    &pane->engine_backoff_ms);
}

static void
disconnect_control(Pane *pane)
{
    if (pane->control_fd >= 0)
        close(pane->control_fd);
    pane->control_fd = -1;
    if (pane->control_input != NULL)
        g_byte_array_set_size(pane->control_input, 0);
    if (!pane->shutting_down)
        queue_retry(&pane->control_retry_us,
                    &pane->control_backoff_ms);
}

static void
disconnect_events(Pane *pane)
{
    if (pane->events_fd >= 0)
        close(pane->events_fd);
    pane->events_fd = -1;
    if (pane->events_input != NULL)
        g_byte_array_set_size(pane->events_input, 0);
    if (!pane->shutting_down)
        queue_retry(&pane->events_retry_us,
                    &pane->events_backoff_ms);
}

static gboolean
read_window_size(guint *width, guint *height)
{
    struct winsize size = { 0 };

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0)
        return FALSE;
    *width = size.ws_xpixel
        ? size.ws_xpixel
        : MAX((guint)size.ws_col * 10, 1u);
    *height = size.ws_ypixel
        ? size.ws_ypixel
        : MAX((guint)size.ws_row * 20, 1u);
    return TRUE;
}

static gboolean
connect_engine(Pane *pane)
{
    struct sockaddr_un address = { 0 };
    struct timeval timeout = { .tv_sec = 5 };

    pane->engine_fd = socket(AF_UNIX,
                             SOCK_SEQPACKET | SOCK_CLOEXEC,
                             0);
    if (pane->engine_fd < 0)
        return FALSE;
    if (strlen(pane->socket_path) >= sizeof(address.sun_path)) {
        close(pane->engine_fd);
        pane->engine_fd = -1;
        errno = ENAMETOOLONG;
        return FALSE;
    }
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path,
              pane->socket_path,
              sizeof(address.sun_path));
    if (connect(pane->engine_fd,
                (const struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(pane->engine_fd);
        pane->engine_fd = -1;
        return FALSE;
    }
    (void)setsockopt(pane->engine_fd,
                     SOL_SOCKET,
                     SO_RCVTIMEO,
                     &timeout,
                     sizeof(timeout));
    (void)setsockopt(pane->engine_fd,
                     SOL_SOCKET,
                     SO_SNDTIMEO,
                     &timeout,
                     sizeof(timeout));
    return TRUE;
}

static gboolean
ensure_engine(Pane *pane)
{
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *directory = self ? g_path_get_dirname(self) : NULL;
    gchar *binary = directory
        ? g_build_filename(directory, "mux-engine", NULL)
        : g_strdup("mux-engine");
    gboolean search_path =
        !g_file_test(binary, G_FILE_TEST_IS_EXECUTABLE);
    gchar *arguments[] = {
        search_path ? (gchar *)"mux-engine" : binary,
        (gchar *)"--ensure",
        (gchar *)"--profile",
        pane->profile,
        (gchar *)"--socket",
        pane->socket_path,
        NULL,
    };
    GError *error = NULL;
    gint wait_status = 0;
    gboolean spawned;

    spawned = g_spawn_sync(NULL,
                           arguments,
                           NULL,
                           search_path ? G_SPAWN_SEARCH_PATH : 0,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           &wait_status,
                           &error);
    if (!spawned || !g_spawn_check_wait_status(wait_status, &error)) {
        g_printerr("mux-pane: start mux-engine: %s\n",
                   error ? error->message : "unknown failure");
        g_clear_error(&error);
        g_free(binary);
        g_free(directory);
        g_free(self);
        return FALSE;
    }
    g_free(binary);
    g_free(directory);
    g_free(self);
    return connect_engine(pane);
}

static gboolean
ensure_muxd(void)
{
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *directory = self ? g_path_get_dirname(self) : NULL;
    gchar *binary = directory
        ? g_build_filename(directory, "muxd", NULL)
        : g_strdup("muxd");
    gboolean search_path =
        !g_file_test(binary, G_FILE_TEST_IS_EXECUTABLE);
    gchar *arguments[] = {
        search_path ? (gchar *)"muxd" : binary,
        (gchar *)"--ensure",
        NULL,
    };
    GError *error = NULL;
    gint wait_status = 0;
    gboolean spawned;

    spawned = g_spawn_sync(NULL,
                           arguments,
                           NULL,
                           search_path ? G_SPAWN_SEARCH_PATH : 0,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           &wait_status,
                           &error);
    if (!spawned || !g_spawn_check_wait_status(wait_status, &error)) {
        g_clear_error(&error);
        g_free(binary);
        g_free(directory);
        g_free(self);
        return FALSE;
    }
    g_free(binary);
    g_free(directory);
    g_free(self);
    return TRUE;
}

static gboolean
control_write(Pane *pane, const gchar *format, ...)
{
    va_list arguments;
    gchar *body;
    gchar *line;
    gboolean result;

    if (pane->control_fd < 0)
        return FALSE;
    va_start(arguments, format);
    body = g_strdup_vprintf(format, arguments);
    va_end(arguments);
    line = g_strconcat(body, "\n", NULL);
    result = write_all(pane->control_fd, line, strlen(line));
    g_free(line);
    g_free(body);
    if (!result)
        disconnect_control(pane);
    return result;
}

static gboolean
connect_control(Pane *pane, const gchar *initial_uri)
{
    gchar *path = muxd_socket_path();
    struct sockaddr_un address = { 0 };
    const gchar *kitty_window = g_getenv("KITTY_WINDOW_ID");
    const gchar *kitty_socket = g_getenv("KITTY_LISTEN_ON");
    gchar *encoded_id;
    gchar *encoded_window;
    gchar *encoded_socket;
    gchar *encoded_layer;
    gchar *encoded_uri;

    pane->control_fd = socket(AF_UNIX,
                              SOCK_STREAM | SOCK_CLOEXEC,
                              0);
    if (pane->control_fd < 0 ||
        strlen(path) >= sizeof(address.sun_path)) {
        if (pane->control_fd >= 0)
            close(pane->control_fd);
        pane->control_fd = -1;
        g_free(path);
        return FALSE;
    }
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    if (connect(pane->control_fd,
                (const struct sockaddr *)&address,
                sizeof(address)) < 0) {
        close(pane->control_fd);
        pane->control_fd = -1;
        g_free(path);
        return FALSE;
    }
    g_free(path);

    if (pane->control_input != NULL)
        g_byte_array_set_size(pane->control_input, 0);
    if (pane->control_id == NULL)
        pane->control_id =
            g_strdup_printf("pane-%ld", (long)getpid());
    encoded_id = mux_encode(pane->control_id);
    encoded_window = mux_encode(kitty_window ? kitty_window : "");
    encoded_socket = mux_encode(kitty_socket ? kitty_socket : "");
    encoded_layer = mux_encode(pane->layer);
    encoded_uri = mux_encode(initial_uri);
    control_write(pane,
                  "VIEW\t%s\t%ld\t%s\t%s\t%s\t%s",
                  encoded_id,
                  (long)getpid(),
                  encoded_window,
                  encoded_socket,
                  encoded_layer,
                  encoded_uri);
    g_free(encoded_id);
    g_free(encoded_window);
    g_free(encoded_socket);
    g_free(encoded_layer);
    g_free(encoded_uri);
    return pane->control_fd >= 0;
}

static gboolean
connect_events(Pane *pane)
{
    gchar *path = muxd_socket_path();
    struct sockaddr_un address = { 0 };

    pane->events_fd = socket(AF_UNIX,
                             SOCK_STREAM | SOCK_CLOEXEC,
                             0);
    if (pane->events_fd < 0 ||
        strlen(path) >= sizeof(address.sun_path)) {
        if (pane->events_fd >= 0)
            close(pane->events_fd);
        pane->events_fd = -1;
        g_free(path);
        return FALSE;
    }
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    if (connect(pane->events_fd,
                (const struct sockaddr *)&address,
                sizeof(address)) < 0 ||
        !write_all(pane->events_fd, "SUB\n", 4)) {
        close(pane->events_fd);
        pane->events_fd = -1;
        g_free(path);
        return FALSE;
    }
    g_free(path);
    g_byte_array_set_size(pane->events_input, 0);
    return TRUE;
}

static void
control_report_state(Pane *pane)
{
    gchar *uri;
    gchar *title;

    if (pane->control_fd < 0)
        return;
    uri = mux_encode(pane->uri ? pane->uri : "");
    title = mux_encode(pane->title ? pane->title : "");
    control_write(pane, "STATE\t%s\t%s", uri, title);
    g_free(uri);
    g_free(title);
}

static void
control_report_focus(Pane *pane, gboolean focused)
{
    control_write(pane, "FOCUS\t%d", focused ? 1 : 0);
}

static void
handle_control_line(Pane *pane, gchar *line)
{
    gchar **fields;
    guint field_count;

    g_strchomp(line);
    fields = g_strsplit(line, "\t", -1);
    field_count = g_strv_length(fields);
    if (field_count >= 3 && g_strcmp0(fields[0], "OK") == 0) {
        g_autofree gchar *assigned_id = mux_decode(fields[2]);

        if (assigned_id != NULL && *assigned_id != '\0' &&
            strlen(assigned_id) <= 128 &&
            g_utf8_validate(assigned_id, -1, NULL) &&
            (g_str_has_prefix(assigned_id, "view-") ||
             g_str_has_prefix(assigned_id, "transient-"))) {
            g_free(pane->control_id);
            pane->control_id = g_steal_pointer(&assigned_id);
        }
    } else if (g_strcmp0(fields[0], "DO") == 0 && fields[1]) {
        if (g_strcmp0(fields[1], "OPEN") == 0 && fields[2]) {
            gchar *uri = mux_decode(fields[2]);
            send_navigation(pane, MUX_ENGINE_NAVIGATE_LOAD, uri);
            g_free(uri);
        } else if (g_strcmp0(fields[1], "BACK") == 0)
            send_navigation(pane, MUX_ENGINE_NAVIGATE_BACK, NULL);
        else if (g_strcmp0(fields[1], "FORWARD") == 0)
            send_navigation(pane, MUX_ENGINE_NAVIGATE_FORWARD, NULL);
        else if (g_strcmp0(fields[1], "RELOAD") == 0)
            send_navigation(pane, MUX_ENGINE_NAVIGATE_RELOAD, NULL);
        else if (g_strcmp0(fields[1], "QUIT") == 0)
            pane->quit = TRUE;
    }
    g_strfreev(fields);
}

static void
read_control(Pane *pane)
{
    guint8 buffer[4096];
    ssize_t count = read(pane->control_fd, buffer, sizeof(buffer));

    if (count <= 0) {
        if (count < 0 && errno == EINTR)
            return;
        disconnect_control(pane);
        return;
    }
    g_byte_array_append(pane->control_input, buffer, (guint)count);
    for (;;) {
        guint8 *newline = memchr(pane->control_input->data,
                                 '\n',
                                 pane->control_input->len);
        gsize length;
        gchar *line;

        if (!newline)
            break;
        length = (gsize)(newline - pane->control_input->data);
        line = g_strndup((const gchar *)pane->control_input->data,
                         length);
        g_byte_array_remove_range(pane->control_input,
                                  0,
                                  length + 1);
        handle_control_line(pane, line);
        g_free(line);
    }
    if (pane->control_input->len > 65536)
        g_byte_array_set_size(pane->control_input, 0);
}

static gboolean
send_message(Pane *pane,
             guint16 type,
             guint32 flags,
             guint64 view_id,
             guint64 serial,
             GBytes *payload)
{
    MuxEngineMessage message = {
        .type = type,
        .flags = flags,
        .view_id = view_id,
        .serial = serial,
        .payload = payload,
    };
    GError *error = NULL;

    if (pane->engine_fd < 0)
        return FALSE;
    if (mux_engine_send_message(pane->engine_fd, &message, &error))
        return TRUE;
    g_printerr("mux-pane: engine connection lost: %s\n",
               error ? error->message : "unknown error");
    g_clear_error(&error);
    disconnect_engine(pane);
    return FALSE;
}

static gboolean
clipboard_wire_output(MuxPaneClipboard *clipboard,
                      GBytes *packet,
                      gpointer user_data,
                      GError **error)
{
    Pane *pane = user_data;
    MuxEngineMessage message = {
        .type = MUX_ENGINE_MESSAGE_EXTENSION,
        .flags = MUX_ENGINE_FLAG_NONE,
        .view_id = pane->view_id,
        .serial = ++pane->next_serial,
        .payload = packet,
    };

    (void) clipboard;
    if (pane->engine_fd < 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_CONNECTED,
                            "mux-engine is reconnecting");
        return FALSE;
    }
    if (mux_engine_send_message(pane->engine_fd, &message, error))
        return TRUE;
    disconnect_engine(pane);
    return FALSE;
}

static gboolean
send_empty(Pane *pane,
           guint16 type,
           guint64 view_id,
           guint64 serial)
{
    GBytes *payload = g_bytes_new(NULL, 0);
    gboolean result = send_message(pane,
                                   type,
                                   MUX_ENGINE_FLAG_NONE,
                                   view_id,
                                   serial,
                                   payload);
    g_bytes_unref(payload);
    return result;
}

static void
request_close(Pane *pane)
{
    guint64 serial;

    if (pane->close_request_serial) {
        pane->quit = TRUE;
        return;
    }
    if (pane->engine_fd < 0 || !pane->view_id) {
        pane->quit = TRUE;
        return;
    }

    serial = ++pane->next_serial;
    if (!serial)
        serial = ++pane->next_serial;
    if (!send_empty(pane,
                    MUX_ENGINE_MESSAGE_REQUEST_CLOSE,
                    pane->view_id,
                    serial)) {
        pane->quit = TRUE;
        return;
    }
    pane->close_request_serial = serial;
    pane->close_deadline_us = g_get_monotonic_time() +
        (gint64)CLOSE_TIMEOUT_MS * 1000;
}

static void
cancel_close_request(Pane *pane)
{
    MuxEngineBuilder builder;
    GBytes *payload;
    guint64 close_serial = pane->close_request_serial;

    if (!close_serial)
        return;
    retire_close_request(pane);
    if (pane->engine_fd < 0 || !pane->view_id)
        return;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u64(&builder, close_serial);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_CANCEL_CLOSE,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static gboolean
send_hello(Pane *pane, const gchar *initial_uri)
{
    MuxEngineBuilder builder;
    GBytes *payload;
    const gchar *kitty_window = g_getenv("KITTY_WINDOW_ID");
    gboolean result;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, (guint32)getpid());
    mux_engine_builder_put_string(&builder,
                                  kitty_window ? kitty_window : "");
    mux_engine_builder_put_string(&builder, pane->layer);
    mux_engine_builder_put_u32(&builder, pane->width);
    mux_engine_builder_put_u32(&builder, pane->height);
    mux_engine_builder_put_u32(&builder, pane->scale_milli);
    mux_engine_builder_put_string(&builder, initial_uri);
    payload = mux_engine_builder_finish(&builder);
    result = send_message(pane,
                          MUX_ENGINE_MESSAGE_HELLO,
                          MUX_ENGINE_FLAG_NONE,
                          0,
                          ++pane->next_serial,
                          payload);
    g_bytes_unref(payload);
    return result;
}

static gboolean
receive_message(Pane *pane, MuxEngineMessage *message)
{
    GError *error = NULL;

    if (pane->engine_fd >= 0 &&
        mux_engine_receive_message(pane->engine_fd,
                                   message,
                                   &error))
        return TRUE;
    if (!pane->engine_handshake_complete &&
        g_error_matches(error,
                        MUX_ENGINE_ERROR,
                        MUX_ENGINE_ERROR_PROTOCOL)) {
        g_printerr("mux-pane: incompatible mux-engine protocol; expected v%u\n",
                   MUX_ENGINE_VERSION);
        pane->quit = TRUE;
    }
    g_printerr("mux-pane: engine connection lost: %s\n",
               error ? error->message : "disconnected");
    g_clear_error(&error);
    disconnect_engine(pane);
    return FALSE;
}

static gboolean
wait_for_message(Pane *pane,
                  guint16 expected,
                  guint64 expected_serial,
                  MuxEngineMessage *message)
{
    for (;;) {
        if (!receive_message(pane, message))
            return FALSE;
        if (message->type == expected) {
            if (message->serial == expected_serial)
                return TRUE;
            g_printerr("mux-pane: out-of-order engine startup response\n");
            mux_engine_message_clear(message);
            disconnect_engine(pane);
            return FALSE;
        }
        if (message->type == MUX_ENGINE_MESSAGE_ERROR) {
            g_autofree gchar *safe_detail = NULL;
            guint32 code = 0;

            if (message->serial == expected_serial &&
                message->view_id == 0 &&
                decode_engine_error_message(message,
                                            &code,
                                            &safe_detail)) {
                g_printerr("mux-pane: engine rejected startup (%u): %s\n",
                           code,
                           safe_detail);
            } else {
                g_printerr("mux-pane: invalid engine startup error\n");
            }
            mux_engine_message_clear(message);
            disconnect_engine(pane);
            return FALSE;
        }
        mux_engine_message_clear(message);
    }
}

static gboolean
start_view(Pane *pane, const gchar *initial_uri)
{
    MuxEngineMessage response = { 0 };
    GBytes *payload;
    guint64 request_serial;

    if (!send_hello(pane, initial_uri))
        return FALSE;
    request_serial = pane->next_serial;
    if (!wait_for_message(pane,
                          MUX_ENGINE_MESSAGE_WELCOME,
                          request_serial,
                          &response))
        return FALSE;
    pane->engine_handshake_complete = TRUE;
    mux_engine_message_clear(&response);

    if (pane->popup_token && *pane->popup_token) {
        MuxEngineBuilder builder;

        mux_engine_builder_init(&builder);
        mux_engine_builder_put_u32(&builder, pane->width);
        mux_engine_builder_put_u32(&builder, pane->height);
        mux_engine_builder_put_u32(&builder, pane->scale_milli);
        mux_engine_builder_put_string(&builder, pane->layer);
        mux_engine_builder_put_string(&builder, initial_uri);
        mux_engine_builder_put_string(&builder, pane->popup_token);
        payload = mux_engine_builder_finish(&builder);
    } else {
        payload = g_bytes_new(NULL, 0);
    }
    request_serial = ++pane->next_serial;
    if (!send_message(pane,
                      MUX_ENGINE_MESSAGE_CREATE_VIEW,
                      pane->ephemeral ? MUX_ENGINE_FLAG_EPHEMERAL
                                      : MUX_ENGINE_FLAG_NONE,
                      0,
                      request_serial,
                      payload)) {
        g_bytes_unref(payload);
        return FALSE;
    }
    g_bytes_unref(payload);
    if (!wait_for_message(pane,
                          MUX_ENGINE_MESSAGE_VIEW_CREATED,
                          request_serial,
                          &response))
        return FALSE;
    if (!response.view_id) {
        g_printerr("mux-pane: engine returned an invalid view id\n");
        mux_engine_message_clear(&response);
        disconnect_engine(pane);
        return FALSE;
    }
    pane->view_id = response.view_id;
    pane->find_generation = 0;
    pane->find_active = FALSE;
    pane->find_status = MUX_ENGINE_FIND_CLOSED;
    pane->find_matches = 0;
    g_clear_pointer(&pane->find_query, g_free);
    pane->next_image_id++;
    if (!pane->next_image_id)
        pane->next_image_id++;
    pane->image_id = pane->next_image_id;
    mux_engine_message_clear(&response);
    g_clear_pointer(&pane->popup_token, g_free);
    return TRUE;
}

static gboolean
terminal_enable(Pane *pane)
{
    struct termios raw;
    g_autoptr(GError) error = NULL;
    const gchar *enable =
        "\033[?1049h\033[2J\033[H\033[?25l"
        "\033[>31u"
        "\033[?1000h\033[?1003h\033[?1006h"
        "\033[?1016h\033[?1004h";

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        tcgetattr(STDIN_FILENO, &pane->saved_terminal) < 0)
        return FALSE;
    pane->saved_terminal_output_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (pane->saved_terminal_output_flags < 0 ||
        fcntl(STDOUT_FILENO,
              F_SETFL,
              pane->saved_terminal_output_flags | O_NONBLOCK) < 0)
        return FALSE;
    raw = pane->saved_terminal;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        fcntl(STDOUT_FILENO,
              F_SETFL,
              pane->saved_terminal_output_flags);
        pane->saved_terminal_output_flags = -1;
        return FALSE;
    }
    terminal_output_clear(pane);
    pane->terminal_output_failed = FALSE;
    pane->terminal_failure_reported = FALSE;
    g_clear_error(&pane->terminal_output_error);
    pane->terminal_active = TRUE;
    if (!terminal_output_enqueue(pane,
                                 enable,
                                 strlen(enable),
                                 0,
                                 &error)) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &pane->saved_terminal);
        fcntl(STDOUT_FILENO,
              F_SETFL,
              pane->saved_terminal_output_flags);
        pane->saved_terminal_output_flags = -1;
        pane->terminal_active = FALSE;
        terminal_output_clear(pane);
        g_warning("enable pane terminal: %s",
                  error != NULL ? error->message : "unknown error");
        return FALSE;
    }
    return TRUE;
}

static void
delete_image(Pane *pane)
{
    gchar *command;

    if (!pane->image_present)
        return;
    command = g_strdup_printf(
        "\033_Ga=d,d=I,i=%u,q=2\033\\",
        pane->image_id);
    if (!terminal_write_critical(pane, command)) {
        g_free(command);
        return;
    }
    g_free(command);
    pane->image_present = FALSE;
}

static gboolean
terminal_disable(Pane *pane, GError **error)
{
    g_autoptr(GError) local_error = NULL;
    gboolean result = TRUE;

    if (!pane->terminal_active)
        return TRUE;
    if (!terminal_output_drain(pane,
                               TERMINAL_OUTPUT_DRAIN_TIMEOUT_MS,
                               &local_error)) {
        result = FALSE;
    } else {
        delete_image(pane);
        if (!pane->terminal_output_failed &&
            terminal_write_critical(
                pane,
                "\033[?1004l\033[?1016l\033[?1006l"
                "\033[?1003l\033[?1000l"
                "\033[<u\033[?25h\033[?1049l")) {
            if (!terminal_output_drain(pane,
                                       TERMINAL_OUTPUT_DRAIN_TIMEOUT_MS,
                                       &local_error))
                result = FALSE;
        } else {
            result = FALSE;
        }
    }
    if (tcsetattr(STDIN_FILENO,
                  TCSAFLUSH,
                  &pane->saved_terminal) < 0 && result) {
        g_set_error(&local_error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "restore pane terminal input: %s",
                    g_strerror(errno));
        result = FALSE;
    }
    if (pane->saved_terminal_output_flags >= 0 &&
        fcntl(STDOUT_FILENO,
              F_SETFL,
              pane->saved_terminal_output_flags) < 0 && result) {
        g_set_error(&local_error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "restore pane terminal output: %s",
                    g_strerror(errno));
        result = FALSE;
    }
    pane->saved_terminal_output_flags = -1;
    pane->terminal_active = FALSE;
    terminal_output_clear(pane);
    if (!result && error != NULL && *error == NULL) {
        if (local_error != NULL)
            *error = g_steal_pointer(&local_error);
        else if (pane->terminal_output_error != NULL)
            *error = g_error_copy(pane->terminal_output_error);
        else
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "pane terminal output could not be drained");
    }
    return result;
}

static guint
kitty_modifiers_to_wpe(guint encoded)
{
    guint modifiers = mux_shortcut_modifiers_from_kitty(encoded);
    guint kitty = encoded ? encoded - 1 : 0;

    if (kitty & 64)
        modifiers |= WPE_MODIFIER_CAPS_LOCK;
    return modifiers;
}

static guint
unicode_keyval(guint32 codepoint)
{
    if (codepoint <= 0xff)
        return codepoint;
    return 0x01000000u | codepoint;
}

static guint
key_number_to_keyval(guint32 key)
{
    switch (key) {
    case 9:
        return KEY_TAB;
    case 13:
        return KEY_RETURN;
    case 27:
        return KEY_ESCAPE;
    case 127:
        return KEY_BACKSPACE;
    case 57348:
        return KEY_INSERT;
    case 57349:
        return KEY_DELETE;
    case 57350:
        return KEY_LEFT;
    case 57351:
        return KEY_RIGHT;
    case 57352:
        return KEY_UP;
    case 57353:
        return KEY_DOWN;
    case 57354:
        return KEY_PAGE_UP;
    case 57355:
        return KEY_PAGE_DOWN;
    case 57356:
        return KEY_HOME;
    case 57357:
        return KEY_END;
    case 57358:
        return 0xffe5u;
    case 57359:
        return 0xff14u;
    case 57360:
        return 0xff7fu;
    case 57361:
        return 0xff61u;
    case 57362:
        return 0xff13u;
    case 57363:
        return 0xff67u;
    default:
        if (key >= 57376 && key <= 57387)
            return KEY_F1 + 12 + (key - 57376);
        if (key <= 0x10ffff)
            return unicode_keyval(key);
        return 0;
    }
}

static void
send_key(Pane *pane,
         guint16 event_type,
         guint modifiers,
         guint keyval)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (!keyval)
        return;
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u16(&builder, event_type);
    mux_engine_builder_put_u32(&builder, event_time());
    mux_engine_builder_put_u32(&builder, modifiers);
    mux_engine_builder_put_u32(&builder, 0);
    mux_engine_builder_put_u32(&builder, keyval);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_INPUT_KEY,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static void
send_text_commit(Pane *pane,
                 guint keyval,
                 const gchar *text,
                 gsize length)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (!keyval || !text || !length ||
        length > MUX_ENGINE_MAX_TEXT_BYTES ||
        memchr(text, '\0', length) ||
        !g_utf8_validate(text, length, NULL))
        return;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, keyval);
    mux_engine_builder_put_u32(&builder, (guint32)length);
    mux_engine_builder_put_bytes(&builder,
                                 (const guint8 *)text,
                                 length);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_TEXT_COMMIT,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static void
send_key_pair(Pane *pane, guint modifiers, guint keyval)
{
    if (pane->clipboard != NULL &&
        mux_pane_clipboard_picker_is_open(pane->clipboard))
        return;

    send_key(pane, MUX_ENGINE_KEY_PRESS, modifiers, keyval);
    send_key(pane, MUX_ENGINE_KEY_RELEASE, modifiers, keyval);
}

static void
send_pointer(Pane *pane,
             guint16 event_type,
             guint modifiers,
             guint button,
             gint x_milli,
             gint y_milli,
             gint delta_x_milli,
             gint delta_y_milli,
             gboolean precise,
             gboolean stop)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u16(&builder, event_type);
    mux_engine_builder_put_u32(&builder, event_time());
    mux_engine_builder_put_u32(&builder, modifiers);
    mux_engine_builder_put_u32(&builder, button);
    mux_engine_builder_put_u32(&builder, (guint32)x_milli);
    mux_engine_builder_put_u32(&builder, (guint32)y_milli);
    mux_engine_builder_put_u32(&builder, (guint32)delta_x_milli);
    mux_engine_builder_put_u32(&builder, (guint32)delta_y_milli);
    mux_engine_builder_put_u32(&builder, precise);
    mux_engine_builder_put_u32(&builder, stop);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_INPUT_POINTER,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static void
send_navigation(Pane *pane,
                MuxEngineNavigationAction action,
                const gchar *uri)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u16(&builder, action);
    if (action == MUX_ENGINE_NAVIGATE_LOAD)
        mux_engine_builder_put_string(&builder, uri ? uri : "");
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_NAVIGATE,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static void
send_engine_focus(Pane *pane, gboolean focused)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (pane->engine_fd < 0 || !pane->view_id)
        return;
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, focused);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_SET_FOCUS,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static gboolean
pane_page_is_visible(const Pane *pane)
{
    return pane->visible &&
        (pane->clipboard == NULL ||
         !mux_pane_clipboard_picker_is_open(pane->clipboard));
}

static void
send_engine_visibility(Pane *pane)
{
    MuxEngineBuilder builder;
    GBytes *payload;
    gboolean visible;

    if (pane->engine_fd < 0 || !pane->view_id)
        return;
    visible = pane_page_is_visible(pane);
    if (pane->engine_visibility_known &&
        pane->engine_visible == visible)
        return;
    if (!visible)
        retire_frame_responses(pane);
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, visible);
    payload = mux_engine_builder_finish(&builder);
    if (send_message(pane,
                     MUX_ENGINE_MESSAGE_SET_VISIBILITY,
                     MUX_ENGINE_FLAG_NONE,
                     pane->view_id,
                     ++pane->next_serial,
                     payload)) {
        pane->engine_visibility_known = TRUE;
        pane->engine_visible = visible;
    }
    g_bytes_unref(payload);
}

static void
send_engine_layer(Pane *pane)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (pane->engine_fd < 0 || !pane->view_id || !pane->layer ||
        !*pane->layer)
        return;
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_string(&builder, pane->layer);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_SET_LAYER,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static void
send_focus(Pane *pane, gboolean focused)
{
    pane->focused = focused;
    send_engine_focus(pane,
                      focused && pane_page_is_visible(pane));
    control_report_focus(pane, focused);
}

static void
send_resize(Pane *pane)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (!pane_page_is_visible(pane) || !pane->terminal_active ||
        pane->engine_fd < 0 || !pane->view_id)
        return;
    retire_frame_responses(pane);
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, pane->width);
    mux_engine_builder_put_u32(&builder, pane->height);
    mux_engine_builder_put_u32(&builder, pane->scale_milli);
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_RESIZE,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 ++pane->next_serial,
                 payload);
    g_bytes_unref(payload);
}

static gboolean
read_terminal_cells(guint *columns, guint *rows)
{
    struct winsize size = { 0 };

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0)
        return FALSE;
    *columns = MAX((guint) size.ws_col, 1u);
    *rows = MAX((guint) size.ws_row, 1u);
    return TRUE;
}

static void
redraw_clipboard_picker(Pane *pane)
{
    guint columns;
    guint rows;
    gchar *panel;

    if (pane->clipboard == NULL ||
        !mux_pane_clipboard_picker_is_open(pane->clipboard) ||
        !read_terminal_cells(&columns, &rows))
        return;
    delete_image(pane);
    panel = mux_pane_clipboard_render_picker(pane->clipboard,
                                            columns,
                                            rows);
    terminal_write(pane, "\033[H\033[2J");
    if (panel != NULL) {
        terminal_write(pane, panel);
        g_free(panel);
    }
}

static void
clipboard_changed(MuxPaneClipboard *clipboard, gpointer user_data)
{
    Pane *pane = user_data;

    (void) clipboard;
    send_engine_visibility(pane);
    send_engine_focus(pane,
                      pane->focused && pane_page_is_visible(pane));
    redraw_clipboard_picker(pane);
}

static void
clipboard_closed(MuxPaneClipboard *clipboard, gpointer user_data)
{
    Pane *pane = user_data;

    (void) clipboard;
    terminal_write(pane, "\033[H\033[2J");
    retire_frame_responses(pane);
    send_engine_visibility(pane);
    delete_image(pane);
    send_resize(pane);
    send_engine_focus(pane,
                      pane->focused && pane_page_is_visible(pane));
}

static void
clipboard_failure(MuxPaneClipboard *clipboard,
                  const gchar *operation,
                  const GError *error,
                  gpointer user_data)
{
    Pane *pane = user_data;

    (void) clipboard;
    if (operation != NULL &&
        strstr(operation, "clipboard-write") != NULL)
        pane->clipboard_write_active = FALSE;
    g_printerr("mux-pane: clipboard %s: %s\n",
               operation != NULL ? operation : "operation",
               error != NULL ? error->message : "unspecified failure");
}

static gboolean
queue_frame_response(Pane *pane, guint64 frame_serial)
{
    guint index;

    if (pane->frame_waiting ||
        pane->frame_response_count >= KITTY_FRAME_RESPONSE_CAPACITY)
        return FALSE;
    index = (pane->frame_response_head + pane->frame_response_count) %
        KITTY_FRAME_RESPONSE_CAPACITY;
    pane->frame_responses[index].image_id = pane->image_id;
    pane->frame_responses[index].frame_serial = frame_serial;
    pane->frame_responses[index].retired = FALSE;
    pane->frame_response_count++;
    pane->frame_waiting = TRUE;
    pane->pending_frame_serial = frame_serial;
    return TRUE;
}

static void
send_frame_rejected(Pane *pane,
                    guint64 frame_serial,
                    MuxEngineFrameRejection reason,
                    const gchar *detail)
{
    MuxEngineBuilder builder;
    GBytes *payload;

    if (pane->engine_fd < 0 || !pane->view_id)
        return;
    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, reason);
    mux_engine_builder_put_string(&builder,
                                  detail ? detail : "frame rejected");
    payload = mux_engine_builder_finish(&builder);
    send_message(pane,
                 MUX_ENGINE_MESSAGE_FRAME_REJECTED,
                 MUX_ENGINE_FLAG_NONE,
                 pane->view_id,
                 frame_serial,
                 payload);
    g_bytes_unref(payload);
}

static gboolean
take_graphics_response(Pane *pane,
                       guint image_id,
                       KittyFrameResponse *response)
{
    if (!pane->frame_response_count)
        return FALSE;
    *response = pane->frame_responses[pane->frame_response_head];
    if (response->image_id != image_id)
        return FALSE;
    pane->frame_response_head = (pane->frame_response_head + 1) %
        KITTY_FRAME_RESPONSE_CAPACITY;
    pane->frame_response_count--;
    return TRUE;
}

static void
acknowledge_graphics_response(Pane *pane, guint image_id)
{
    KittyFrameResponse response;

    if (!take_graphics_response(pane, image_id, &response))
        return;
    if (response.retired)
        return;
    if (!pane->frame_waiting ||
        pane->pending_frame_serial != response.frame_serial)
        return;

    if (pane->engine_fd >= 0 && pane->view_id)
        send_empty(pane,
                   MUX_ENGINE_MESSAGE_FRAME_ACK,
                   pane->view_id,
                   response.frame_serial);
    pane->frame_waiting = FALSE;
    pane->pending_frame_serial = 0;
}

static void
reject_graphics_response(Pane *pane,
                         guint image_id,
                         const gchar *detail)
{
    KittyFrameResponse response;

    if (!take_graphics_response(pane, image_id, &response))
        return;
    if (response.retired)
        return;
    if (!pane->frame_waiting ||
        pane->pending_frame_serial != response.frame_serial)
        return;

    pane->frame_waiting = FALSE;
    pane->pending_frame_serial = 0;
    send_frame_rejected(pane,
                        response.frame_serial,
                        MUX_ENGINE_FRAME_REJECTED_KITTY,
                        detail);
}

static gboolean
handle_frame(Pane *pane, const MuxEngineMessage *message)
{
    MuxEngineCursor cursor;
    guint32 width;
    guint32 height;
    guint32 stride;
    guint32 format;
    guint32 rectangle_count;
    guint32 x;
    guint32 y;
    guint32 rectangle_width;
    guint32 rectangle_height;
    guint64 shm_size;
    gchar *shm_name = NULL;
    gchar *encoded_name;
    gchar *command;

    mux_engine_cursor_init(&cursor, message->payload);
    if (!mux_engine_cursor_get_u32(&cursor, &width) ||
        !mux_engine_cursor_get_u32(&cursor, &height) ||
        !mux_engine_cursor_get_u32(&cursor, &stride) ||
        !mux_engine_cursor_get_u32(&cursor, &format) ||
        !mux_engine_cursor_get_u32(&cursor, &rectangle_count) ||
        rectangle_count != 1 ||
        !mux_engine_cursor_get_u32(&cursor, &x) ||
        !mux_engine_cursor_get_u32(&cursor, &y) ||
        !mux_engine_cursor_get_u32(&cursor, &rectangle_width) ||
        !mux_engine_cursor_get_u32(&cursor, &rectangle_height) ||
        !mux_engine_cursor_get_u64(&cursor, &shm_size) ||
        !mux_engine_cursor_get_string(&cursor, &shm_name) ||
        !mux_engine_cursor_done(&cursor) ||
        format != MUX_ENGINE_PIXEL_RGBA8888 ||
        !rectangle_width || !rectangle_height ||
        x + rectangle_width > width ||
        y + rectangle_height > height ||
        stride != rectangle_width * 4 ||
        shm_size != (guint64)stride * rectangle_height ||
        !g_str_has_prefix(shm_name, "/")) {
        g_printerr("mux-pane: invalid FRAME payload\n");
        g_free(shm_name);
        return FALSE;
    }

    if (!pane_page_is_visible(pane) || !pane->terminal_active) {
        pane->width = width;
        pane->height = height;
        send_frame_rejected(pane,
                            message->serial,
                            MUX_ENGINE_FRAME_REJECTED_NOT_VISIBLE,
                            "pane is not visible");
        g_free(shm_name);
        return TRUE;
    }

    if (pane->frame_waiting) {
        g_printerr("mux-pane: FRAME arrived while Kitty response is pending\n");
        g_free(shm_name);
        return FALSE;
    }
    if (pane->frame_response_count >= KITTY_FRAME_RESPONSE_CAPACITY) {
        send_frame_rejected(pane,
                            message->serial,
                            MUX_ENGINE_FRAME_REJECTED_BACKPRESSURE,
                            "Kitty response queue is full");
        g_free(shm_name);
        return TRUE;
    }

    encoded_name = g_base64_encode((const guchar *)shm_name,
                                   strlen(shm_name));
    command = build_kitty_frame_command(pane->image_present,
                                        message->flags,
                                        width,
                                        height,
                                        x,
                                        y,
                                        rectangle_width,
                                        rectangle_height,
                                        shm_size,
                                        pane->image_id,
                                        encoded_name);
    {
        g_autoptr(GError) output_error = NULL;
        GBytes *command_bytes = g_bytes_new(command, strlen(command));
        gboolean queued = terminal_output_enqueue_bytes(pane,
                                                        command_bytes,
                                                        0,
                                                        &output_error);

        g_bytes_unref(command_bytes);
        if (!queued) {
            g_printerr("mux-pane: frame output backpressure: %s\n",
                       output_error != NULL
                           ? output_error->message
                           : "unknown error");
            g_free(command);
            g_free(encoded_name);
            g_free(shm_name);
            return FALSE;
        }
    }
    if (!queue_frame_response(pane, message->serial)) {
        g_printerr("mux-pane: could not track Kitty FRAME response\n");
        g_free(command);
        g_free(encoded_name);
        g_free(shm_name);
        return FALSE;
    }
    if (!pane->image_present ||
        (message->flags & MUX_ENGINE_FLAG_FULL_DAMAGE))
        pane->image_present = TRUE;
    pane->width = width;
    pane->height = height;
    g_free(command);
    g_free(encoded_name);
    g_free(shm_name);
    return TRUE;
}

static void
set_title(Pane *pane, const gchar *title)
{
    gchar *safe = g_strdup(title && *title ? title : "Mux");
    gchar *command;

    for (gchar *cursor = safe; *cursor; cursor++) {
        if ((guchar)*cursor < 0x20 || *cursor == 0x7f)
            *cursor = ' ';
    }
    command = g_strdup_printf("\033]2;%s\033\\", safe);
    terminal_write(pane, command);
    g_free(command);
    g_free(safe);
}

static void
append_title_indicator(GString *title, gboolean enabled, const gchar *label)
{
    if (enabled)
        g_string_append_printf(title, "[%s] ", label);
}

static gchar *
trusted_title(const gchar *page_title, guint32 flags)
{
    GString *title = g_string_new("MUX ");
    gboolean audio_playing = flags & MUX_ENGINE_METADATA_AUDIO_PLAYING;
    gboolean audio_muted = flags & MUX_ENGINE_METADATA_AUDIO_MUTED;

    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_FULLSCREEN,
                           "FULLSCREEN");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_DISPLAY_ACTIVE,
                           "DISPLAY");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_DISPLAY_MUTED,
                           "DISPLAY:MUTED");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_CAMERA_ACTIVE,
                           "CAMERA");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_CAMERA_MUTED,
                           "CAMERA:MUTED");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_MICROPHONE_ACTIVE,
                           "MIC");
    append_title_indicator(title,
                           flags & MUX_ENGINE_METADATA_MICROPHONE_MUTED,
                           "MIC:MUTED");
    append_title_indicator(title,
                           audio_playing,
                           audio_muted ? "AUDIO:MUTED" : "AUDIO");
    append_title_indicator(title,
                           audio_muted && !audio_playing,
                           "MUTED");
    g_string_append(title, "| ");
    g_string_append(title, page_title && *page_title ? page_title : "Mux");
    return g_string_free(title, FALSE);
}

static gboolean
handle_metadata(Pane *pane, const MuxEngineMessage *message)
{
    MuxEngineCursor cursor;
    gchar *uri = NULL;
    gchar *title = NULL;
    gchar *layer = NULL;
    guint32 value;
    guint32 state_flags = 0;
    g_autofree gchar *display_title = NULL;

    mux_engine_cursor_init(&cursor, message->payload);
    if (!mux_engine_cursor_get_string(&cursor, &uri) ||
        !mux_engine_cursor_get_string(&cursor, &title) ||
        !mux_engine_cursor_get_string(&cursor, &layer)) {
        g_free(uri);
        g_free(title);
        g_free(layer);
        return FALSE;
    }
    for (guint i = 0; i < 7; i++) {
        if (!mux_engine_cursor_get_u32(&cursor, &value)) {
            g_free(uri);
            g_free(title);
            g_free(layer);
            return FALSE;
        }
    }
    if (!mux_engine_cursor_done(&cursor) &&
        !mux_engine_cursor_get_u32(&cursor, &state_flags)) {
        g_free(uri);
        g_free(title);
        g_free(layer);
        return FALSE;
    }
    if (!mux_engine_cursor_done(&cursor)) {
        g_free(uri);
        g_free(title);
        g_free(layer);
        return FALSE;
    }
    g_free(pane->uri);
    g_free(pane->title);
    pane->uri = uri;
    pane->title = title;
    display_title = trusted_title(title, state_flags);
    set_title(pane, display_title);
    control_report_state(pane);
    g_free(layer);
    return TRUE;
}

static gboolean
handle_find_state(Pane *pane, const MuxEngineMessage *message)
{
    MuxEngineCursor cursor;
    guint32 active;
    guint32 status;
    guint32 matches;
    guint64 generation;
    g_autofree gchar *query = NULL;
    gsize query_length;

    mux_engine_cursor_init(&cursor, message->payload);
    if (message->flags != MUX_ENGINE_FLAG_NONE || !message->serial ||
        !mux_engine_cursor_get_u32(&cursor, &active) || active > 1 ||
        !mux_engine_cursor_get_u32(&cursor, &status) ||
        status > MUX_ENGINE_FIND_NOT_FOUND ||
        !mux_engine_cursor_get_u32(&cursor, &matches) ||
        matches > MUX_ENGINE_MAX_FIND_MATCHES ||
        !mux_engine_cursor_get_u64(&cursor, &generation) || !generation ||
        !mux_engine_cursor_get_string(&cursor, &query) ||
        !mux_engine_cursor_done(&cursor))
        return FALSE;
    query_length = strlen(query);
    if (query_length > MUX_ENGINE_MAX_FIND_TEXT_BYTES ||
        !g_utf8_validate(query, query_length, NULL) ||
        (!active && (status != MUX_ENGINE_FIND_CLOSED ||
                     matches || query_length)) ||
        (active && status == MUX_ENGINE_FIND_CLOSED) ||
        (status == MUX_ENGINE_FIND_FOUND && !matches) ||
        (status != MUX_ENGINE_FIND_FOUND && matches))
        return FALSE;
    if (generation < pane->find_generation)
        return TRUE;

    pane->find_generation = generation;
    pane->find_active = active != 0;
    pane->find_status = (MuxEngineFindStatus)status;
    pane->find_matches = matches;
    g_free(pane->find_query);
    pane->find_query = g_steal_pointer(&query);
    find_overlay_repaint(pane);
    return TRUE;
}

static gboolean
handle_chooser_payload(Pane *pane,
                       MuxUiRecordType record_type,
                       const guint8 *packet,
                       gsize packet_length,
                       gboolean *consumed,
                       GError **error)
{
    g_return_val_if_fail(consumed, FALSE);
    *consumed = FALSE;
    if (pane->chooser == NULL)
        return TRUE;

    if (record_type == MUX_UI_RECORD_REQUEST) {
        g_autoptr(MuxUiRequest) request = NULL;

        if (!mux_ui_request_decode(packet,
                                   packet_length,
                                   &request,
                                   error))
            return FALSE;
        if (request->kind != MUX_UI_REQUEST_FILE_CHOOSER)
            return TRUE;
        *consumed = TRUE;
        return mux_kitty_chooser_handle_request(pane->chooser,
                                                request,
                                                error);
    }

    if (record_type == MUX_UI_RECORD_CANCEL) {
        guint64 request_id;
        MuxUiCancelReason reason;

        if (!mux_ui_cancel_decode(packet,
                                  packet_length,
                                  &request_id,
                                  &reason,
                                  error))
            return FALSE;
        (void)reason;
        *consumed = mux_kitty_chooser_cancel(pane->chooser,
                                             request_id);
    }
    return TRUE;
}

static void
handle_engine_message(Pane *pane)
{
    MuxEngineMessage message = { 0 };

    if (!receive_message(pane, &message))
        return;
    if (message.type != MUX_ENGINE_MESSAGE_ERROR &&
        message.view_id && message.view_id != pane->view_id) {
        mux_engine_message_clear(&message);
        return;
    }

    switch (message.type) {
    case MUX_ENGINE_MESSAGE_FRAME:
        if (!handle_frame(pane, &message))
            pane->quit = TRUE;
        break;
    case MUX_ENGINE_MESSAGE_METADATA:
        handle_metadata(pane, &message);
        break;
    case MUX_ENGINE_MESSAGE_FIND_STATE:
        if (!handle_find_state(pane, &message)) {
            g_printerr("mux-pane: invalid FIND_STATE\n");
            disconnect_engine(pane);
        }
        break;
    case MUX_ENGINE_MESSAGE_CLOSE_READY:
        if (message.flags != MUX_ENGINE_FLAG_NONE ||
            g_bytes_get_size(message.payload) != 0) {
            g_printerr("mux-pane: invalid or out-of-order CLOSE_READY\n");
            disconnect_engine(pane);
        } else if (message.serial &&
                   message.serial <= pane->retired_close_serial) {
            break;
        } else if (!pane->close_request_serial ||
                   message.serial != pane->close_request_serial) {
            g_printerr("mux-pane: invalid or out-of-order CLOSE_READY\n");
            disconnect_engine(pane);
        } else {
            pane->close_request_serial = 0;
            pane->close_deadline_us = 0;
            pane->quit = TRUE;
        }
        break;
    case MUX_ENGINE_MESSAGE_CLOSE_CANCELLED:
        if (message.flags != MUX_ENGINE_FLAG_NONE ||
            !message.serial ||
            g_bytes_get_size(message.payload) != 0) {
            g_printerr("mux-pane: invalid CLOSE_CANCELLED\n");
            disconnect_engine(pane);
        } else if (message.serial <= pane->retired_close_serial) {
            break;
        } else if (!pane->close_request_serial ||
                   message.serial != pane->close_request_serial) {
            g_printerr("mux-pane: out-of-order CLOSE_CANCELLED\n");
            disconnect_engine(pane);
        } else {
            retire_close_request(pane);
        }
        break;
    case MUX_ENGINE_MESSAGE_EXTENSION: {
        gsize packet_length;
        const guint8 *packet =
            g_bytes_get_data(message.payload, &packet_length);
        g_autoptr(GError) error = NULL;
        g_autoptr(GError) probe_error = NULL;
        MuxUiRecordType record_type;

        if ((pane->ui_bridge != NULL || pane->notifications != NULL) &&
            mux_ui_record_type(packet,
                               packet_length,
                               &record_type,
                               &probe_error)) {
            gboolean consumed = FALSE;

            if (pane->notifications != NULL &&
                !mux_notification_pane_handle_payload(
                    pane->notifications,
                    packet,
                    packet_length,
                    &consumed,
                    &error))
                g_warning("notification payload rejected: %s",
                          error ? error->message : "unknown error");
            else if (!consumed &&
                     !handle_chooser_payload(pane,
                                             record_type,
                                             packet,
                                             packet_length,
                                             &consumed,
                                             &error))
                g_warning("file chooser payload rejected: %s",
                          error ? error->message : "unknown error");
            else if (!consumed && pane->ui_bridge != NULL &&
                     !mux_ui_pane_bridge_handle_payload(pane->ui_bridge,
                                                        packet,
                                                        packet_length,
                                                        &error))
                g_warning("pane UI payload rejected: %s",
                          error ? error->message : "unknown error");
            if (pane->ui_bridge &&
                mux_ui_pane_bridge_is_active(pane->ui_bridge))
                pane->find_overlay_visible = FALSE;
        } else if (pane->clipboard != NULL &&
                   !mux_pane_clipboard_handle_engine_packet(
                       pane->clipboard,
                       packet,
                       packet_length,
                       &error)) {
            clipboard_failure(pane->clipboard,
                              "engine-wire",
                              error,
                              pane);
        }
        break;
    }
    case MUX_ENGINE_MESSAGE_ERROR:
        handle_runtime_engine_error(pane, &message);
        break;
    default:
        break;
    }
    mux_engine_message_clear(&message);
}

static gboolean
ui_wire_output(GBytes *payload, gpointer data, GError **error)
{
    Pane *pane = data;
    gboolean sent;

    sent = send_message(pane,
                        MUX_ENGINE_MESSAGE_EXTENSION,
                        MUX_ENGINE_FLAG_NONE,
                        pane->view_id,
                        ++pane->next_serial,
                        payload);
    if (sent)
        return TRUE;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_BROKEN_PIPE,
                        "UI response could not be sent to mux-engine");
    return FALSE;
}

static gboolean
ui_terminal_output(const guint8 *data,
                   gsize length,
                   gpointer user_data,
                   GError **error)
{
    Pane *pane = user_data;

    return terminal_output_enqueue(pane,
                                   data,
                                   length,
                                   TERMINAL_OUTPUT_FRAME_RESERVE_BYTES,
                                   error);
}

static gboolean
ui_update_size(Pane *pane, GError **error)
{
    struct winsize window = { 0 };

    if (!pane->ui_bridge)
        return TRUE;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) < 0 ||
        !window.ws_col || !window.ws_row) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(errno),
                    "read pane UI dimensions: %s",
                    g_strerror(errno));
        return FALSE;
    }
    return mux_ui_pane_bridge_set_size(pane->ui_bridge,
                                       window.ws_col,
                                       window.ws_row,
                                       error);
}

static gboolean
chooser_suspend(gpointer user_data, GError **error)
{
    Pane *pane = user_data;

    if (pane->shutting_down) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "pane is shutting down");
        return FALSE;
    }
    if (!pane->terminal_active) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "pane terminal is already suspended");
        return FALSE;
    }
    find_overlay_hide(pane);
    g_byte_array_set_size(pane->input, 0);
    return terminal_disable(pane, error);
}

static gboolean
chooser_resume(gpointer user_data, GError **error)
{
    Pane *pane = user_data;
    guint width;
    guint height;

    if (pane->shutting_down)
        return TRUE;
    if (!pane->terminal_active && !terminal_enable(pane)) {
        pane->quit = TRUE;
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "could not restore pane terminal after chooser");
        return FALSE;
    }
    g_byte_array_set_size(pane->input, 0);
    if (read_window_size(&width, &height)) {
        pane->width = width;
        pane->height = height;
    }
    retire_frame_responses(pane);
    delete_image(pane);
    send_resize(pane);
    if (!ui_update_size(pane, error))
        return FALSE;
    if (pane->clipboard != NULL &&
        mux_pane_clipboard_picker_is_open(pane->clipboard))
        redraw_clipboard_picker(pane);
    find_overlay_repaint(pane);
    return TRUE;
}

static gboolean
handle_ui_key(Pane *pane,
              guint event_type,
              guint modifiers,
              guint keyval,
              gunichar text)
{
    MuxPaneOverlayKey overlay_key;
    gboolean mapped = TRUE;
    gboolean consumed = FALSE;
    GError *error = NULL;

    if (!pane->ui_bridge ||
        !mux_ui_pane_bridge_is_active(pane->ui_bridge))
        return FALSE;
    if (event_type == 3)
        return TRUE;

    if (keyval == KEY_ESCAPE)
        overlay_key = MUX_PANE_OVERLAY_KEY_ESCAPE;
    else if (keyval == KEY_RETURN)
        overlay_key = MUX_PANE_OVERLAY_KEY_ENTER;
    else if (keyval == KEY_BACKSPACE)
        overlay_key = MUX_PANE_OVERLAY_KEY_BACKSPACE;
    else if (keyval == KEY_UP)
        overlay_key = MUX_PANE_OVERLAY_KEY_UP;
    else if (keyval == KEY_DOWN)
        overlay_key = MUX_PANE_OVERLAY_KEY_DOWN;
    else if (keyval == KEY_TAB)
        overlay_key = MUX_PANE_OVERLAY_KEY_TAB;
    else if ((modifiers & (WPE_MODIFIER_CONTROL |
                            WPE_MODIFIER_ALT |
                            WPE_MODIFIER_META)) == 0 &&
             text <= 0x10ffffu &&
             g_unichar_isprint(text))
        overlay_key = MUX_PANE_OVERLAY_KEY_TEXT;
    else
        mapped = FALSE;

    if (mapped) {
        (void)ui_update_size(pane, NULL);
        if (!mux_ui_pane_bridge_handle_key(
                pane->ui_bridge,
                overlay_key,
                overlay_key == MUX_PANE_OVERLAY_KEY_TEXT ? text : 0,
                &consumed,
                &error)) {
            g_warning("pane UI key failed: %s",
                      error ? error->message : "unknown error");
            g_clear_error(&error);
        }
        if (!mux_ui_pane_bridge_is_active(pane->ui_bridge))
            find_overlay_repaint(pane);
    }
    return TRUE;
}

static void
handle_graphics_response(Pane *pane,
                         const guint8 *sequence,
                         gsize length)
{
    g_autofree gchar *detail = NULL;
    guint image_id = 0;
    KittyGraphicsResponseResult result =
        parse_kitty_graphics_response(sequence,
                                      length,
                                      &image_id,
                                      &detail);

    if (result == KITTY_GRAPHICS_RESPONSE_SUCCESS)
        acknowledge_graphics_response(pane, image_id);
    else if (result == KITTY_GRAPHICS_RESPONSE_ERROR) {
        g_printerr("mux-pane: Kitty graphics rejected frame: %s\n",
                   detail);
        reject_graphics_response(pane, image_id, detail);
    }
}

static guint
legacy_modifiers(const gchar *parameters)
{
    const gchar *semicolon = strchr(parameters, ';');

    if (!semicolon)
        return 0;
    return kitty_modifiers_to_wpe(
        (guint)g_ascii_strtoull(semicolon + 1, NULL, 10));
}

static gchar *
kitty_committed_text(const gchar *field, guint32 fallback)
{
    GString *text = g_string_new(NULL);

    if (field && *field) {
        g_auto(GStrv) codepoints = g_strsplit(field, ":", -1);
        guint i;

        for (i = 0; codepoints[i]; i++) {
            gchar *end = NULL;
            guint64 parsed;

            errno = 0;
            parsed = g_ascii_strtoull(codepoints[i], &end, 10);
            if (errno || end == codepoints[i] || *end ||
                !g_unichar_validate((gunichar)parsed) || !parsed) {
                g_string_free(text, TRUE);
                return NULL;
            }
            g_string_append_unichar(text, (gunichar)parsed);
        }
    } else if (fallback <= 0x10ffffu &&
               g_unichar_type((gunichar)fallback) !=
                   G_UNICODE_PRIVATE_USE &&
               g_unichar_isprint((gunichar)fallback)) {
        g_string_append_unichar(text, (gunichar)fallback);
    }
    return g_string_free(text, FALSE);
}

static guint16
engine_key_event_type(guint kitty_event_type)
{
    if (kitty_event_type == 3)
        return MUX_ENGINE_KEY_RELEASE;
    if (kitty_event_type == 2)
        return MUX_ENGINE_KEY_REPEAT;
    return MUX_ENGINE_KEY_PRESS;
}

static void
forward_delayed_paste_keys(Pane *pane)
{
    guint index;

    for (index = 0; index < pane->delayed_paste_count; index++) {
        DelayedPasteKey *key = &pane->delayed_paste_keys[index];

        send_key(pane, key->event_type, key->modifiers, key->keyval);
    }
    pane->delayed_paste_count = 0;
}

static void
fresh_paste_ready(MuxPaneClipboard *clipboard,
                  guint64 request_id,
                  gboolean fresh,
                  gpointer user_data)
{
    Pane *pane = user_data;
    guint key_count;

    (void)clipboard;
    if (request_id != pane->fresh_paste_request_id)
        return;
    key_count = pane->delayed_paste_count;
    pane->fresh_paste_request_id = 0;
    mux_clipboard_smoke_trace(
        MUX_CLIPBOARD_TRACE_DELAYED_PASTE,
        &(MuxClipboardTraceFields) {
            .request_id = request_id,
            .key_count = key_count,
            .fresh = fresh
        });
    forward_delayed_paste_keys(pane);
}

static gboolean
defer_page_paste_key(Pane *pane,
                     guint kitty_event_type,
                     guint modifiers,
                     guint keyval)
{
    g_autoptr(GError) error = NULL;
    gint64 active_due_us;

    if (kitty_event_type == 3 && !pane->fresh_paste_request_id)
        return FALSE;
    if (pane->delayed_paste_count >=
        MUX_PANE_CLIPBOARD_MAX_PENDING_PASTES)
        forward_delayed_paste_keys(pane);

    pane->delayed_paste_keys[pane->delayed_paste_count++] =
        (DelayedPasteKey) {
            .event_type = engine_key_event_type(kitty_event_type),
            .modifiers = modifiers,
            .keyval = keyval,
        };
    if (pane->fresh_paste_request_id)
        return TRUE;

    pane->next_fresh_paste_request_id++;
    if (!pane->next_fresh_paste_request_id)
        pane->next_fresh_paste_request_id++;
    pane->fresh_paste_request_id = pane->next_fresh_paste_request_id;
    if (!mux_pane_clipboard_request_fresh_paste(
            pane->clipboard,
            pane->fresh_paste_request_id,
            fresh_paste_ready,
            pane,
            &error)) {
        g_warning("fresh clipboard paste unavailable: %s",
                  error != NULL ? error->message : "unknown error");
        pane->fresh_paste_request_id = 0;
        forward_delayed_paste_keys(pane);
        return TRUE;
    }

    active_due_us = g_get_monotonic_time() +
        (gint64)ACTIVE_MAINTENANCE_MS * 1000;
    if (!pane->clipboard_tick_due_us ||
        pane->clipboard_tick_due_us > active_due_us)
        pane->clipboard_tick_due_us = active_due_us;
    return TRUE;
}

static void
handle_kitty_key(Pane *pane, const gchar *parameters)
{
    gchar **fields = g_strsplit(parameters, ";", 4);
    gsize field_count = g_strv_length(fields);
    gchar **key_fields;
    gchar **modifier_fields = NULL;
    g_autofree gchar *committed_text = NULL;
    guint32 key_number;
    guint32 text_codepoint = 0;
    guint encoded_modifiers = 1;
    guint event_type = 1;
    guint modifiers;
    guint keyval;
    MuxShortcut owned_shortcut;
    gboolean execute_owned_shortcut = FALSE;
    gboolean owned_shortcut_handled;

    if (!fields[0] || !*fields[0]) {
        g_strfreev(fields);
        return;
    }
    key_fields = g_strsplit(fields[0], ":", 3);
    key_number = (guint32)g_ascii_strtoull(key_fields[0], NULL, 10);
    if (field_count > 1 && fields[1] && *fields[1]) {
        modifier_fields = g_strsplit(fields[1], ":", 2);
        encoded_modifiers =
            (guint)g_ascii_strtoull(modifier_fields[0], NULL, 10);
        if (modifier_fields[1])
            event_type =
                (guint)g_ascii_strtoull(modifier_fields[1], NULL, 10);
    }
    committed_text = kitty_committed_text(
        field_count > 2 ? fields[2] : NULL,
        key_number);
    if (committed_text && *committed_text)
        text_codepoint = g_utf8_get_char(committed_text);

    modifiers = kitty_modifiers_to_wpe(encoded_modifiers);
    keyval = key_number_to_keyval(key_number);
    owned_shortcut = mux_shortcut_match_pane(modifiers, key_number);
    owned_shortcut_handled = mux_shortcut_handle_event(
        owned_shortcut,
        event_type,
        &execute_owned_shortcut);
    if (handle_ui_key(pane,
                      event_type,
                      modifiers,
                      keyval,
                      text_codepoint)) {
    } else if (pane->find_active) {
        if (event_type != MUX_ENGINE_KEY_RELEASE &&
            committed_text && *committed_text &&
            !(modifiers & (WPE_MODIFIER_CONTROL |
                           WPE_MODIFIER_ALT |
                           WPE_MODIFIER_META)))
            send_text_commit(pane,
                             keyval,
                             committed_text,
                             strlen(committed_text));
        send_key(pane,
                 engine_key_event_type(event_type),
                 modifiers,
                 keyval);
    } else if (pane->clipboard != NULL &&
               owned_shortcut == MUX_SHORTCUT_CLIPBOARD_HISTORY) {
        if (execute_owned_shortcut) {
            mux_pane_clipboard_open_picker(pane->clipboard);
            send_engine_visibility(pane);
            send_engine_focus(pane,
                              pane->focused &&
                                  pane_page_is_visible(pane));
        }
    } else if (pane->clipboard != NULL &&
               mux_pane_clipboard_picker_is_open(pane->clipboard)) {
        MuxClipboardPickerKey picker_key;
        MuxShortcut picker_shortcut =
            mux_shortcut_match_picker(modifiers, key_number);
        gboolean mapped = TRUE;

        if (event_type == 3) {
            mapped = TRUE;
        } else if (keyval == KEY_ESCAPE) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_ESCAPE;
        } else if (keyval == KEY_RETURN) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_ENTER;
        } else if (keyval == KEY_BACKSPACE) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_BACKSPACE;
        } else if (keyval == KEY_UP) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_UP;
        } else if (keyval == KEY_DOWN) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_DOWN;
        } else if (keyval == KEY_PAGE_UP) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_PAGE_UP;
        } else if (keyval == KEY_PAGE_DOWN) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_PAGE_DOWN;
        } else if (keyval == KEY_HOME) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_HOME;
        } else if (keyval == KEY_END) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_END;
        } else if (picker_shortcut ==
                   MUX_SHORTCUT_PICKER_DELETE_WORD) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_DELETE_WORD;
        } else if (picker_shortcut ==
                   MUX_SHORTCUT_PICKER_CLEAR_QUERY) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY;
        } else if (picker_shortcut ==
                   MUX_SHORTCUT_PICKER_TOGGLE_PIN) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN;
        } else if (picker_shortcut ==
                   MUX_SHORTCUT_PICKER_DELETE_ENTRY) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY;
        } else if (picker_shortcut ==
                   MUX_SHORTCUT_PICKER_CLEAR_HISTORY) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY;
        } else if ((modifiers & (WPE_MODIFIER_CONTROL |
                                 WPE_MODIFIER_ALT |
                                 WPE_MODIFIER_META)) == 0 &&
                   text_codepoint <= 0x10ffff &&
                   g_unichar_isprint(text_codepoint)) {
            picker_key = MUX_CLIPBOARD_PICKER_KEY_TEXT;
        } else {
            mapped = FALSE;
        }
        if (event_type != 3 && mapped)
            mux_pane_clipboard_handle_picker_key(
                pane->clipboard,
                picker_key,
                picker_key == MUX_CLIPBOARD_PICKER_KEY_TEXT
                    ? text_codepoint
                    : 0);
    } else if (pane->clipboard != NULL &&
               (key_number == 'v' || key_number == 'V') &&
               (mux_shortcut_is_page_paste(modifiers, key_number) ||
                (event_type == MUX_SHORTCUT_EVENT_RELEASE &&
                 pane->fresh_paste_request_id != 0)) &&
               defer_page_paste_key(pane,
                                    event_type,
                                    modifiers,
                                    keyval)) {
    } else if (owned_shortcut_handled) {
        if (execute_owned_shortcut) {
            if (owned_shortcut == MUX_SHORTCUT_CLOSE)
                request_close(pane);
            else if (owned_shortcut == MUX_SHORTCUT_LOCATION)
                control_write(pane, "PROMPT");
            else if (owned_shortcut == MUX_SHORTCUT_RELOAD)
                send_navigation(pane,
                                MUX_ENGINE_NAVIGATE_RELOAD,
                                NULL);
        }
    } else {
        if (event_type != 3 && committed_text && *committed_text &&
            !(modifiers & (WPE_MODIFIER_CONTROL |
                           WPE_MODIFIER_ALT |
                           WPE_MODIFIER_META)))
            send_text_commit(pane,
                             keyval,
                             committed_text,
                             strlen(committed_text));
        send_key(pane,
                 event_type == 3
                     ? MUX_ENGINE_KEY_RELEASE
                     : event_type == 2
                         ? MUX_ENGINE_KEY_REPEAT
                         : MUX_ENGINE_KEY_PRESS,
                 modifiers,
                 keyval);
    }

    g_strfreev(modifier_fields);
    g_strfreev(key_fields);
    g_strfreev(fields);
}

static void
handle_mouse(Pane *pane,
             const gchar *parameters,
             gchar final_byte)
{
    guint encoded;
    guint x;
    guint y;
    guint modifiers = pane->pointer_modifiers;
    guint button_code;
    guint button = 0;
    gint x_milli;
    gint y_milli;

    if (pane->clipboard != NULL &&
        mux_pane_clipboard_picker_is_open(pane->clipboard))
        return;
    if (sscanf(parameters, "<%u;%u;%u", &encoded, &x, &y) != 3)
        return;
    if (encoded & 4)
        modifiers |= WPE_MODIFIER_SHIFT;
    if (encoded & 8)
        modifiers |= WPE_MODIFIER_ALT;
    if (encoded & 16)
        modifiers |= WPE_MODIFIER_CONTROL;
    x_milli = (gint)(x ? x - 1 : 0) * 1000;
    y_milli = (gint)(y ? y - 1 : 0) * 1000;

    if (encoded & 64) {
        gint delta_x = 0;
        gint delta_y = 0;
        button_code = encoded & 3;
        if (button_code < 2)
            delta_y = button_code ? 40000 : -40000;
        else
            delta_x = button_code == 2 ? -40000 : 40000;
        send_pointer(pane,
                     MUX_ENGINE_POINTER_SCROLL,
                     modifiers,
                     0,
                     x_milli,
                     y_milli,
                     delta_x,
                     delta_y,
                     FALSE,
                     FALSE);
        return;
    }

    button_code = encoded & 3;
    if (button_code == 0)
        button = 1;
    else if (button_code == 1)
        button = 2;
    else if (button_code == 2)
        button = 3;

    if ((encoded & 32) || button_code == 3) {
        send_pointer(pane,
                     MUX_ENGINE_POINTER_MOVE,
                     modifiers,
                     0,
                     x_milli,
                     y_milli,
                     0,
                     0,
                     FALSE,
                     FALSE);
    } else if (final_byte == 'M') {
        guint button_modifier = button == 1
            ? WPE_MODIFIER_BUTTON1
            : button == 2
                ? WPE_MODIFIER_BUTTON2
                : WPE_MODIFIER_BUTTON3;
        pane->pointer_modifiers |= button_modifier;
        send_pointer(pane,
                     MUX_ENGINE_POINTER_DOWN,
                     modifiers | button_modifier,
                     button,
                     x_milli,
                     y_milli,
                     0,
                     0,
                     FALSE,
                     FALSE);
    } else {
        guint button_modifier = button == 1
            ? WPE_MODIFIER_BUTTON1
            : button == 2
                ? WPE_MODIFIER_BUTTON2
                : WPE_MODIFIER_BUTTON3;
        send_pointer(pane,
                     MUX_ENGINE_POINTER_UP,
                     modifiers,
                     button,
                     x_milli,
                     y_milli,
                     0,
                     0,
                     FALSE,
                     FALSE);
        pane->pointer_modifiers &= ~button_modifier;
    }
}

static void
handle_csi(Pane *pane,
           const guint8 *body,
           gsize body_length,
           gchar final_byte)
{
    gchar *parameters = g_strndup((const gchar *)body, body_length);
    guint modifiers = legacy_modifiers(parameters);
    guint keyval = 0;

    if ((final_byte == 'I' || final_byte == 'O') && !body_length) {
        send_focus(pane, final_byte == 'I');
        g_free(parameters);
        return;
    }
    if ((final_byte == 'M' || final_byte == 'm') &&
        parameters[0] == '<') {
        handle_mouse(pane, parameters, final_byte);
        g_free(parameters);
        return;
    }
    if (final_byte == 'u') {
        handle_kitty_key(pane, parameters);
        g_free(parameters);
        return;
    }

    switch (final_byte) {
    case 'A':
        keyval = KEY_UP;
        break;
    case 'B':
        keyval = KEY_DOWN;
        break;
    case 'C':
        keyval = KEY_RIGHT;
        break;
    case 'D':
        keyval = KEY_LEFT;
        break;
    case 'H':
        keyval = KEY_HOME;
        break;
    case 'F':
        keyval = KEY_END;
        break;
    case 'P':
        keyval = KEY_F1;
        break;
    case 'Q':
        keyval = KEY_F1 + 1;
        break;
    case 'R':
        keyval = KEY_F1 + 2;
        break;
    case 'S':
        keyval = KEY_F1 + 3;
        break;
    case '~': {
        guint number =
            (guint)g_ascii_strtoull(parameters, NULL, 10);
        if (number == 2)
            keyval = KEY_INSERT;
        else if (number == 3)
            keyval = KEY_DELETE;
        else if (number == 5)
            keyval = KEY_PAGE_UP;
        else if (number == 6)
            keyval = KEY_PAGE_DOWN;
        else if (number >= 11 && number <= 15)
            keyval = KEY_F1 + number - 11;
        else if (number >= 17 && number <= 21)
            keyval = KEY_F1 + number - 12;
        else if (number == 23 || number == 24)
            keyval = KEY_F1 + number - 13;
        break;
    }
    default:
        break;
    }
    if (keyval) {
        MuxShortcut navigation_shortcut =
            mux_shortcut_match_engine(modifiers, keyval);

        if (pane->find_active)
            send_key_pair(pane, modifiers, keyval);
        else if (pane->clipboard != NULL &&
            mux_pane_clipboard_picker_is_open(pane->clipboard)) {
            MuxClipboardPickerKey picker_key;

            if (keyval == KEY_UP)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_UP;
            else if (keyval == KEY_DOWN)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_DOWN;
            else if (keyval == KEY_PAGE_UP)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_PAGE_UP;
            else if (keyval == KEY_PAGE_DOWN)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_PAGE_DOWN;
            else if (keyval == KEY_HOME)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_HOME;
            else if (keyval == KEY_END)
                picker_key = MUX_CLIPBOARD_PICKER_KEY_END;
            else {
                g_free(parameters);
                return;
            }
            mux_pane_clipboard_handle_picker_key(pane->clipboard,
                                                 picker_key,
                                                 0);
        } else if (navigation_shortcut == MUX_SHORTCUT_HISTORY_BACK)
            send_navigation(pane, MUX_ENGINE_NAVIGATE_BACK, NULL);
        else if (navigation_shortcut == MUX_SHORTCUT_HISTORY_FORWARD)
            send_navigation(pane,
                            MUX_ENGINE_NAVIGATE_FORWARD,
                            NULL);
        else
            send_key_pair(pane, modifiers, keyval);
    }
    g_free(parameters);
}

static void
process_terminal_input(Pane *pane)
{
    while (pane->input->len) {
        guint8 *data = pane->input->data;
        gsize length = pane->input->len;
        gsize consumed = 0;

        if (data[0] == 0x1b) {
            if (length < 2)
                break;
            if (data[1] == '_') {
                gsize end;
                for (end = 2; end + 1 < length; end++) {
                    if (data[end] == 0x1b &&
                        data[end + 1] == '\\')
                        break;
                }
                if (end + 1 >= length)
                    break;
                if (end >= 3 && data[2] == 'G')
                    handle_graphics_response(pane,
                                             data + 3,
                                             end - 3);
                consumed = end + 2;
            } else if (data[1] == '[') {
                gsize end;
                for (end = 2; end < length; end++) {
                    if (data[end] >= 0x40 && data[end] <= 0x7e)
                        break;
                }
                if (end >= length)
                    break;
                if (pane->clipboard != NULL && data[end] == 'y' &&
                    end >= 10 &&
                    memcmp(data, "\033[?5522;", 8) == 0 &&
                    data[end - 1] == '$') {
                    g_autoptr(GError) error = NULL;

                    if (!mux_pane_clipboard_handle_support(
                            pane->clipboard,
                            data,
                            end + 1,
                            &error))
                        clipboard_failure(pane->clipboard,
                                          "support-response",
                                          error,
                                          pane);
                } else {
                    handle_csi(pane,
                               data + 2,
                               end - 2,
                               (gchar)data[end]);
                }
                consumed = end + 1;
            } else if (data[1] == ']') {
                gsize end;
                gboolean terminated = FALSE;

                for (end = 2; end < length; end++) {
                    if (data[end] == '\a') {
                        end++;
                        terminated = TRUE;
                        break;
                    }
                    if (data[end] == 0x1b && end + 1 < length &&
                        data[end + 1] == '\\') {
                        end += 2;
                        terminated = TRUE;
                        break;
                    }
                }
                if (!terminated)
                    break;
                if (pane->notifications != NULL && end >= 5 &&
                    memcmp(data, "\033]99;", 5) == 0) {
                    g_autoptr(GError) error = NULL;

                    if (!mux_notification_pane_handle_osc(
                            pane->notifications,
                            data,
                            end,
                            &error))
                        g_warning("notification response rejected: %s",
                                  error ? error->message
                                        : "unknown error");
                } else if (pane->clipboard != NULL && end >= 7 &&
                    memcmp(data, "\033]5522;", 7) == 0) {
                    g_autoptr(GError) error = NULL;

                    gboolean handled = mux_pane_clipboard_handle_osc(
                        pane->clipboard,
                        data,
                        end,
                        &error);

                    if (!handled)
                        clipboard_failure(pane->clipboard,
                                          "osc-response",
                                          error,
                                          pane);
                    else if (g_strstr_len((const gchar *)data,
                                          end,
                                          "type=write") != NULL)
                        pane->clipboard_write_active = FALSE;
                }
                consumed = end;
            } else {
                gunichar codepoint;
                gint character_length =
                    g_utf8_get_char_validated(
                        (const gchar *)data + 1,
                        length - 1);
                if (character_length == (gint)-2)
                    break;
                if (character_length == (gint)-1) {
                    send_key_pair(pane, 0, KEY_ESCAPE);
                    consumed = 1;
                } else {
                    codepoint =
                        g_utf8_get_char((const gchar *)data + 1);
                    if (pane->clipboard != NULL &&
                        mux_pane_clipboard_picker_is_open(
                            pane->clipboard) &&
                        (codepoint == 'p' || codepoint == 'P' ||
                         codepoint == 'd' || codepoint == 'D' ||
                         codepoint == 'c' || codepoint == 'C')) {
                        MuxClipboardPickerKey picker_key =
                            codepoint == 'p' || codepoint == 'P'
                                ? MUX_CLIPBOARD_PICKER_KEY_TOGGLE_PIN
                                : codepoint == 'd' || codepoint == 'D'
                                    ? MUX_CLIPBOARD_PICKER_KEY_DELETE_ENTRY
                                    : MUX_CLIPBOARD_PICKER_KEY_CLEAR_HISTORY;
                        mux_pane_clipboard_handle_picker_key(
                            pane->clipboard,
                            picker_key,
                            0);
                    } else {
                        send_key_pair(pane,
                                      WPE_MODIFIER_ALT,
                                      unicode_keyval(codepoint));
                    }
                    consumed = 1 +
                        (gsize)g_utf8_next_char(
                            (const gchar *)data + 1) -
                        (gsize)((const gchar *)data + 1);
                }
            }
        } else if (data[0] < 0x20 || data[0] == 0x7f) {
            guint byte = data[0];
            if (pane->find_active) {
                if (byte == 9)
                    send_key_pair(pane, 0, KEY_TAB);
                else if (byte == 13 || byte == 10)
                    send_key_pair(pane, 0, KEY_RETURN);
                else if (byte == 127)
                    send_key_pair(pane, 0, KEY_BACKSPACE);
                else if (byte >= 1 && byte <= 26)
                    send_key_pair(pane,
                                  WPE_MODIFIER_CONTROL,
                                  'a' + byte - 1);
            } else if (pane->clipboard != NULL &&
                mux_pane_clipboard_picker_is_open(pane->clipboard) &&
                (byte == 13 || byte == 10 || byte == 127 ||
                 byte == 21 || byte == 23)) {
                MuxClipboardPickerKey picker_key =
                    byte == 13 || byte == 10
                        ? MUX_CLIPBOARD_PICKER_KEY_ENTER
                        : byte == 127
                            ? MUX_CLIPBOARD_PICKER_KEY_BACKSPACE
                            : byte == 21
                                ? MUX_CLIPBOARD_PICKER_KEY_CLEAR_QUERY
                                : MUX_CLIPBOARD_PICKER_KEY_DELETE_WORD;
                mux_pane_clipboard_handle_picker_key(pane->clipboard,
                                                     picker_key,
                                                     0);
            } else if (byte == 17)
                request_close(pane);
            else if (byte == 18)
                send_navigation(pane,
                                MUX_ENGINE_NAVIGATE_RELOAD,
                                NULL);
            else if (byte == 12)
                control_write(pane, "PROMPT");
            else if (byte == 9)
                send_key_pair(pane, 0, KEY_TAB);
            else if (byte == 13 || byte == 10)
                send_key_pair(pane, 0, KEY_RETURN);
            else if (byte == 127)
                send_key_pair(pane, 0, KEY_BACKSPACE);
            else if (byte >= 1 && byte <= 26)
                send_key_pair(pane,
                              WPE_MODIFIER_CONTROL,
                              'a' + byte - 1);
            consumed = 1;
        } else {
            gunichar codepoint =
                g_utf8_get_char_validated((const gchar *)data,
                                          length);
            if (codepoint == (gunichar)-2)
                break;
            if (codepoint == (gunichar)-1)
                consumed = 1;
            else {
                const gchar *next =
                    g_utf8_next_char((const gchar *)data);
                if (handle_ui_key(pane,
                                  1,
                                  0,
                                  unicode_keyval(codepoint),
                                  codepoint)) {
                } else if (pane->clipboard != NULL &&
                    mux_pane_clipboard_picker_is_open(pane->clipboard))
                    mux_pane_clipboard_handle_picker_key(
                        pane->clipboard,
                        MUX_CLIPBOARD_PICKER_KEY_TEXT,
                        codepoint);
                else {
                    send_text_commit(pane,
                                     unicode_keyval(codepoint),
                                     (const gchar *)data,
                                     next - (const gchar *)data);
                    send_key_pair(pane, 0, unicode_keyval(codepoint));
                }
                consumed = (gsize)(next - (const gchar *)data);
            }
        }

        if (!consumed)
            break;
        g_byte_array_remove_range(pane->input, 0, consumed);
    }
    if (pane->input->len > 65536)
        g_byte_array_set_size(pane->input, 0);
}

static void
read_terminal(Pane *pane)
{
    guint8 buffer[8192];
    ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));

    if (count <= 0) {
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        pane->quit = TRUE;
        return;
    }
    g_byte_array_append(pane->input, buffer, (guint)count);
    process_terminal_input(pane);
}

static void
handle_resize(Pane *pane)
{
    guint width;
    guint height;

    if (!read_window_size(&width, &height) ||
        (width == pane->width && height == pane->height))
        return;
    pane->width = width;
    pane->height = height;
    delete_image(pane);
    send_resize(pane);
    (void)ui_update_size(pane, NULL);
    find_overlay_repaint(pane);
}

static void
set_visibility(Pane *pane, gboolean visible)
{
    guint width;
    guint height;

    if (pane->visible == visible)
        return;
    pane->visible = visible;
    if (!visible)
        find_overlay_hide(pane);
    send_engine_visibility(pane);
    if (!visible) {
        send_engine_focus(pane, FALSE);
        delete_image(pane);
        return;
    }
    if (!pane->terminal_active)
        return;
    if (read_window_size(&width, &height)) {
        pane->width = width;
        pane->height = height;
    }
    find_overlay_repaint(pane);
    if (pane->clipboard != NULL &&
        mux_pane_clipboard_picker_is_open(pane->clipboard)) {
        send_engine_focus(pane, FALSE);
        redraw_clipboard_picker(pane);
        return;
    }
    retire_frame_responses(pane);
    delete_image(pane);
    send_resize(pane);
    send_engine_focus(pane, pane->focused);
    (void)ui_update_size(pane, NULL);
}

static void
update_visibility(Pane *pane)
{
    if (pane->active_layer != NULL)
        set_visibility(pane,
                       g_strcmp0(pane->layer,
                                 pane->active_layer) == 0);
}

static void
update_active_layer(Pane *pane, const gchar *encoded_layer)
{
    gchar *layer = mux_decode(encoded_layer);

    g_free(pane->active_layer);
    pane->active_layer = layer;
    update_visibility(pane);
}

static void
update_own_layer(Pane *pane,
                 const gchar *encoded_id,
                 const gchar *encoded_layer)
{
    gchar *id = mux_decode(encoded_id);

    if (g_strcmp0(id, pane->control_id) == 0) {
        gchar *layer = mux_decode(encoded_layer);

        if (g_strcmp0(layer, pane->layer) != 0) {
            g_free(pane->layer);
            pane->layer = layer;
            layer = NULL;
            send_engine_layer(pane);
        }
        g_free(layer);
        update_visibility(pane);
    }
    g_free(id);
}

static void
handle_events_line(Pane *pane, gchar *line)
{
    gchar **fields;
    guint count;

    g_strchomp(line);
    fields = g_strsplit(line, "\t", -1);
    count = g_strv_length(fields);
    if (count >= 4 && g_strcmp0(fields[0], "BEGIN") == 0)
        update_active_layer(pane, fields[3]);
    else if (count >= 3 && g_strcmp0(fields[0], "VIEW") == 0)
        update_own_layer(pane, fields[1], fields[2]);
    else if (count >= 5 && g_strcmp0(fields[0], "EVENT") == 0 &&
             g_strcmp0(fields[2], "UPSERT") == 0)
        update_own_layer(pane, fields[3], fields[4]);
    else if (count >= 4 && g_strcmp0(fields[0], "EVENT") == 0 &&
             g_strcmp0(fields[2], "LAYER") == 0)
        update_active_layer(pane, fields[3]);
    g_strfreev(fields);
}

static void
read_events(Pane *pane)
{
    guint8 buffer[8192];
    ssize_t count = read(pane->events_fd, buffer, sizeof(buffer));

    if (count <= 0) {
        if (count < 0 && errno == EINTR)
            return;
        disconnect_events(pane);
        return;
    }
    g_byte_array_append(pane->events_input, buffer, (guint)count);
    for (;;) {
        guint8 *newline = memchr(pane->events_input->data,
                                 '\n',
                                 pane->events_input->len);
        gsize length;
        gchar *line;

        if (!newline)
            break;
        length = (gsize)(newline - pane->events_input->data);
        line = g_strndup((const gchar *)pane->events_input->data,
                         length);
        g_byte_array_remove_range(pane->events_input,
                                  0,
                                  length + 1);
        handle_events_line(pane, line);
        g_free(line);
    }
    if (pane->events_input->len > 65536)
        disconnect_events(pane);
}

static void
retry_engine(Pane *pane)
{
    const gchar *uri = pane->uri && *pane->uri
        ? pane->uri
        : "about:blank";

    pane->engine_retry_us = 0;
    if ((!connect_engine(pane) && !ensure_engine(pane)) ||
        !start_view(pane, uri)) {
        disconnect_engine(pane);
        return;
    }
    reset_retry(&pane->engine_retry_us,
                &pane->engine_backoff_ms);
    send_engine_visibility(pane);
    send_engine_focus(pane,
                      pane->focused && pane_page_is_visible(pane));
    send_resize(pane);
}

static void
retry_control(Pane *pane)
{
    const gchar *uri = pane->uri ? pane->uri : "";
    gboolean connected;

    pane->control_retry_us = 0;
    connected = connect_control(pane, uri);
    if (!connected && ensure_muxd()) {
        for (guint attempt = 0; attempt < 10 && !connected; attempt++) {
            g_usleep(20000);
            connected = connect_control(pane, uri);
        }
    }
    if (!connected) {
        queue_retry(&pane->control_retry_us,
                    &pane->control_backoff_ms);
        return;
    }
    reset_retry(&pane->control_retry_us,
                &pane->control_backoff_ms);
    control_report_state(pane);
    control_report_focus(pane, pane->focused);
}

static void
retry_events(Pane *pane)
{
    pane->events_retry_us = 0;
    if (!connect_events(pane)) {
        queue_retry(&pane->events_retry_us,
                    &pane->events_backoff_ms);
        return;
    }
    reset_retry(&pane->events_retry_us,
                &pane->events_backoff_ms);
}

static gint
deadline_timeout_ms(gint64 deadline_us, gint64 now_us)
{
    gint64 remaining_us;

    if (!deadline_us)
        return -1;
    remaining_us = deadline_us - now_us;
    if (remaining_us <= 0)
        return 0;
    return (gint)MIN((remaining_us + 999) / 1000,
                     (gint64)G_MAXINT);
}

static void
tighten_timeout(gint *timeout_ms, gint candidate_ms)
{
    if (candidate_ms >= 0 &&
        (*timeout_ms < 0 || candidate_ms < *timeout_ms))
        *timeout_ms = candidate_ms;
}

static gint
next_poll_timeout(Pane *pane, gint64 now_us)
{
    gint timeout_ms = -1;
    gboolean active_maintenance =
        (pane->chooser != NULL &&
         mux_kitty_chooser_is_busy(pane->chooser)) ||
        (pane->ui_bridge != NULL &&
         mux_ui_pane_bridge_is_active(pane->ui_bridge)) ||
        (pane->clipboard != NULL &&
         mux_pane_clipboard_picker_is_open(pane->clipboard));

    tighten_timeout(&timeout_ms,
                    deadline_timeout_ms(pane->engine_retry_us,
                                        now_us));
    tighten_timeout(&timeout_ms,
                    deadline_timeout_ms(pane->control_retry_us,
                                        now_us));
    tighten_timeout(&timeout_ms,
                    deadline_timeout_ms(pane->events_retry_us,
                                        now_us));
    tighten_timeout(&timeout_ms,
                    deadline_timeout_ms(pane->close_deadline_us,
                                        now_us));
    if (pane->clipboard != NULL) {
        if (!pane->clipboard_tick_due_us)
            tighten_timeout(&timeout_ms, 0);
        else
            tighten_timeout(&timeout_ms,
                            deadline_timeout_ms(
                                pane->clipboard_tick_due_us,
                                now_us));
    }
    if (g_main_context_pending(pane->main_context))
        tighten_timeout(&timeout_ms, 0);
    else if (active_maintenance)
        tighten_timeout(&timeout_ms, ACTIVE_MAINTENANCE_MS);
    return timeout_ms;
}

static void
service_maintenance(Pane *pane, gint64 monotonic_us)
{
    if (pane->close_request_serial && pane->close_deadline_us &&
        monotonic_us >= pane->close_deadline_us) {
        cancel_close_request(pane);
    }
    while (g_main_context_iteration(pane->main_context, FALSE))
        ;
    if (pane->clipboard != NULL &&
        (!pane->clipboard_tick_due_us ||
         monotonic_us >= pane->clipboard_tick_due_us)) {
        mux_pane_clipboard_tick(pane->clipboard, monotonic_us);
        pane->clipboard_tick_due_us = monotonic_us +
            (gint64)(pane->clipboard_write_active ||
                     mux_pane_clipboard_fresh_paste_pending(
                         pane->clipboard)
                         ? ACTIVE_MAINTENANCE_MS
                         : IDLE_MAINTENANCE_MS) * 1000;
    }
    if (pane->chooser != NULL)
        mux_kitty_chooser_tick(pane->chooser, monotonic_us);
    if (pane->ui_bridge != NULL) {
        gboolean resolved = FALSE;
        GError *ui_error = NULL;

        if (!mux_ui_pane_bridge_tick(pane->ui_bridge,
                                     monotonic_us,
                                     &resolved,
                                     &ui_error)) {
            g_warning("pane UI timer failed: %s",
                      ui_error ? ui_error->message : "unknown error");
            g_clear_error(&ui_error);
        }
    }
}

static void
run_pane(Pane *pane)
{
    struct pollfd descriptors[5];

    send_focus(pane, TRUE);
    while (!pane->quit && !quit_requested) {
        gint64 monotonic_us = g_get_monotonic_time();
        int ready;

        if (pane->engine_fd < 0 && pane->engine_retry_us &&
            monotonic_us >= pane->engine_retry_us)
            retry_engine(pane);
        if (pane->control_fd < 0 && pane->control_retry_us &&
            monotonic_us >= pane->control_retry_us)
            retry_control(pane);
        if (pane->events_fd < 0 && pane->events_retry_us &&
            monotonic_us >= pane->events_retry_us)
            retry_events(pane);
        if (resize_requested) {
            resize_requested = 0;
            handle_resize(pane);
        }
        descriptors[0].fd = pane->terminal_active
            ? STDIN_FILENO
            : -1;
        descriptors[0].events = pane->terminal_active
            ? POLLIN
            : 0;
        descriptors[0].revents = 0;
        descriptors[1].fd = pane->engine_fd;
        descriptors[1].events = pane->engine_fd >= 0 ? POLLIN : 0;
        descriptors[1].revents = 0;
        descriptors[2].fd = pane->control_fd;
        descriptors[2].events =
            pane->control_fd >= 0 ? POLLIN : 0;
        descriptors[2].revents = 0;
        descriptors[3].fd = pane->events_fd;
        descriptors[3].events = pane->events_fd >= 0 ? POLLIN : 0;
        descriptors[3].revents = 0;
        descriptors[4].fd = !pane->terminal_output_failed &&
                pane->terminal_output_bytes > 0
            ? STDOUT_FILENO
            : -1;
        descriptors[4].events = descriptors[4].fd >= 0 ? POLLOUT : 0;
        descriptors[4].revents = 0;
        ready = poll(descriptors,
                     G_N_ELEMENTS(descriptors),
                     next_poll_timeout(pane, monotonic_us));
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            pane->quit = TRUE;
            break;
        }
        if (descriptors[4].revents & POLLOUT) {
            g_autoptr(GError) output_error = NULL;

            if (!terminal_output_flush(pane, &output_error))
                g_warning("pane terminal output failed: %s",
                          output_error != NULL
                              ? output_error->message
                              : "unknown error");
        }
        if (descriptors[4].revents & (POLLHUP | POLLERR | POLLNVAL))
            terminal_output_fail(pane,
                                 EPIPE,
                                 "poll pane terminal output");
        if (descriptors[0].revents & POLLIN)
            read_terminal(pane);
        if (pane->engine_fd >= 0 &&
            descriptors[1].revents & POLLIN)
            handle_engine_message(pane);
        if (pane->control_fd >= 0 &&
            descriptors[2].revents & POLLIN)
            read_control(pane);
        if (pane->control_fd >= 0 &&
            descriptors[2].revents &
                (POLLHUP | POLLERR | POLLNVAL)) {
            disconnect_control(pane);
        }
        if (pane->events_fd >= 0 &&
            descriptors[3].revents & POLLIN)
            read_events(pane);
        if (pane->events_fd >= 0 &&
            descriptors[3].revents &
                (POLLHUP | POLLERR | POLLNVAL))
            disconnect_events(pane);
        if (descriptors[1].revents &
            (POLLHUP | POLLERR | POLLNVAL))
            disconnect_engine(pane);
        if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            pane->quit = TRUE;
        service_maintenance(pane, g_get_monotonic_time());
        if (pane->terminal_output_failed &&
            !pane->terminal_failure_reported) {
            pane->terminal_failure_reported = TRUE;
            if (pane->clipboard != NULL)
                mux_pane_clipboard_tick(pane->clipboard, G_MAXINT64);
            pane->quit = TRUE;
        }
    }
}

static void
chooser_clear(Pane *pane)
{
    if (pane->chooser == NULL)
        return;
    mux_kitty_chooser_cancel_all(pane->chooser);
    while (mux_kitty_chooser_is_busy(pane->chooser))
        g_main_context_iteration(pane->main_context, TRUE);
    g_clear_pointer(&pane->chooser, mux_kitty_chooser_free);
}

static void
pane_clear(Pane *pane)
{
    g_autoptr(GError) terminal_error = NULL;

    pane->shutting_down = TRUE;
    chooser_clear(pane);
    g_clear_pointer(&pane->notifications,
                    mux_notification_pane_free);
    g_clear_pointer(&pane->ui_bridge, mux_ui_pane_bridge_free);
    g_clear_pointer(&pane->clipboard, mux_pane_clipboard_free);
    if (!terminal_disable(pane, &terminal_error))
        g_warning("disable pane terminal: %s",
                  terminal_error != NULL
                      ? terminal_error->message
                      : "unknown error");
    if (pane->control_fd >= 0) {
        control_write(pane, "BYE");
        if (pane->control_fd >= 0)
            close(pane->control_fd);
        pane->control_fd = -1;
    }
    if (pane->events_fd >= 0)
        close(pane->events_fd);
    pane->events_fd = -1;
    if (pane->engine_fd >= 0) {
        if (pane->view_id)
            send_empty(pane,
                       MUX_ENGINE_MESSAGE_DESTROY_VIEW,
                       pane->view_id,
                       ++pane->next_serial);
        if (pane->engine_fd >= 0)
            close(pane->engine_fd);
        pane->engine_fd = -1;
    }
    g_clear_pointer(&pane->input, g_byte_array_unref);
    g_clear_pointer(&pane->control_input, g_byte_array_unref);
    g_clear_pointer(&pane->events_input, g_byte_array_unref);
    terminal_output_clear(pane);
    g_clear_error(&pane->terminal_output_error);
    g_free(pane->profile);
    g_free(pane->socket_path);
    g_free(pane->layer);
    g_free(pane->uri);
    g_free(pane->title);
    g_free(pane->control_id);
    g_free(pane->active_layer);
    g_free(pane->popup_token);
    g_free(pane->find_query);
    g_clear_pointer(&pane->main_context, g_main_context_unref);
}

int
main(int argc, char **argv)
{
    const gchar *profile_environment = g_getenv("MUX_PROFILE");
    const gchar *layer_environment = g_getenv("MUX_LAYER");
    const gchar *ephemeral_environment = g_getenv("MUX_EPHEMERAL");
    const gchar *popup_environment = g_getenv("MUX_POPUP_TOKEN");
    const gchar *initial_uri = argc > 1
        ? argv[1]
        : "https://wpewebkit.org";
    Pane pane = {
        .engine_fd = -1,
        .control_fd = -1,
        .events_fd = -1,
        .saved_terminal_output_flags = -1,
        .scale_milli = 1000,
        .next_serial = 1,
        .visible = TRUE,
        .focused = TRUE,
        .main_context = g_main_context_ref_thread_default(),
    };

    g_set_prgname("mux-pane");
    signal(SIGPIPE, SIG_IGN);
    {
        g_autoptr(GError) scale_error = NULL;

        if (!mux_engine_parse_device_scale(
                g_getenv(MUX_DEVICE_SCALE_ENV),
                &pane.scale_milli,
                &scale_error)) {
            g_printerr("mux-pane: %s\n", scale_error->message);
            g_main_context_unref(pane.main_context);
            return 2;
        }
    }
    pane.profile = g_strdup(profile_environment && *profile_environment
                                ? profile_environment
                                : "default");
    pane.layer = g_strdup(layer_environment && *layer_environment
                              ? layer_environment
                              : "main");
    pane.ephemeral = ephemeral_environment &&
        g_strcmp0(ephemeral_environment, "0") != 0;
    pane.popup_token = g_strdup(popup_environment);
    pane.socket_path = engine_socket_path(pane.profile);
    pane.input = g_byte_array_new();
    pane.control_input = g_byte_array_new();
    pane.events_input = g_byte_array_new();
    pane.uri = g_strdup(initial_uri);

    if (!read_window_size(&pane.width, &pane.height)) {
        g_printerr("mux-pane: cannot determine terminal size\n");
        pane_clear(&pane);
        return 1;
    }
    if (!connect_engine(&pane) && !ensure_engine(&pane)) {
        g_printerr("mux-pane: connect %s: %s\n",
                   pane.socket_path,
                   g_strerror(errno));
        pane_clear(&pane);
        return 1;
    }
    if (!start_view(&pane, initial_uri)) {
        pane_clear(&pane);
        return 1;
    }
    send_engine_visibility(&pane);
    if (!connect_control(&pane, initial_uri))
        g_printerr("mux-pane: muxd unavailable; global controls disabled\n");
    if (pane.control_fd < 0)
        queue_retry(&pane.control_retry_us,
                    &pane.control_backoff_ms);
    else
        reset_retry(&pane.control_retry_us,
                    &pane.control_backoff_ms);
    if (!connect_events(&pane))
        queue_retry(&pane.events_retry_us,
                    &pane.events_backoff_ms);
    else
        reset_retry(&pane.events_retry_us,
                    &pane.events_backoff_ms);
    reset_retry(&pane.engine_retry_us,
                &pane.engine_backoff_ms);
    if (!terminal_enable(&pane)) {
        g_printerr("mux-pane: a real TTY is required\n");
        pane_clear(&pane);
        return 1;
    }

    g_autoptr(GError) ui_error = NULL;
    pane.ui_bridge = mux_ui_pane_bridge_new(ui_wire_output,
                                            ui_terminal_output,
                                            &pane,
                                            NULL);
    if (pane.ui_bridge == NULL) {
        g_printerr("mux-pane: terminal UI unavailable\n");
    } else if (!ui_update_size(&pane, &ui_error)) {
        g_printerr("mux-pane: terminal UI size: %s\n",
                   ui_error->message);
        g_clear_error(&ui_error);
    }
    pane.chooser = mux_kitty_chooser_new(ui_wire_output,
                                         chooser_suspend,
                                         chooser_resume,
                                         &pane,
                                         NULL);
    if (pane.chooser == NULL)
        g_printerr("mux-pane: Kitty file chooser unavailable\n");
    pane.notifications = mux_notification_pane_new(
        ui_wire_output,
        ui_terminal_output,
        &pane,
        NULL);
    if (pane.notifications == NULL)
        g_printerr("mux-pane: desktop notifications unavailable\n");

    g_autoptr(GError) clipboard_error = NULL;
    pane.clipboard = mux_pane_clipboard_new(
        pane.profile,
        pane.ephemeral,
        pane.view_id,
        pane.main_context,
        clipboard_terminal_output,
        clipboard_wire_output,
        clipboard_changed,
        clipboard_closed,
        clipboard_failure,
        &pane,
        NULL,
        &clipboard_error);
    if (pane.clipboard == NULL) {
        g_printerr("mux-pane: clipboard unavailable: %s\n",
                   clipboard_error->message);
    } else if (!mux_pane_clipboard_set_enabled(pane.clipboard,
                                               TRUE,
                                               &clipboard_error)) {
        g_printerr("mux-pane: enable clipboard: %s\n",
                   clipboard_error->message);
        g_clear_error(&clipboard_error);
    }

    signal(SIGWINCH, signal_resize);
    signal(SIGINT, signal_quit);
    signal(SIGTERM, signal_quit);
    run_pane(&pane);
    pane_clear(&pane);
    return 0;
}

#endif
