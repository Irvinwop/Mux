#define _GNU_SOURCE

#include "mux-protocol.h"
#include "mux-session-state.h"
#include "muxd-clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    CLIENT_UNKNOWN,
    CLIENT_VIEW,
    CLIENT_SUBSCRIBER,
    CLIENT_BAR,
} ClientKind;

typedef struct {
    int fd;
    ClientKind kind;
    gboolean closing;
    gboolean close_after_flush;
    gboolean request_pending;
    gboolean graceful_bye;
    gboolean persistable;
    GString *input;
    GQueue *output;
    gsize output_bytes;
    gsize output_offset;

    gchar *id;
    gchar *kitty_window;
    gchar *kitty_socket;
    gchar *layer;
    gchar *uri;
    gchar *title;
    long pid;
    pid_t peer_pid;
    guint64 session_view_id;
    gboolean focused;
} Client;

typedef struct {
    int listener;
    int lock_fd;
    GPtrArray *clients;
    gchar *socket_path;
    gchar *active_id;
    gchar *current_layer;
    guint64 revision;
    guint64 next_view_id;
    guint64 reserved_view_id_limit;
    guint64 next_transient_id;
    gboolean running;
    gboolean session_dirty;
    gint64 session_write_due_us;
    GMainContext *main_context;
    GPtrArray *pending_moves;
    MuxdClipboard *clipboard;
    MuxSessionState *session;
    gchar *session_path;
} Server;

typedef struct {
    Server *server;
    Client *control;
    Client *source;
    gchar *layer;
    GPid child_pid;
    GSource *child_source;
    GSource *timeout_source;
    gint64 deadline_us;
    gboolean timed_out;
    gboolean cancelled;
    gboolean source_lost;
} PendingMove;

#define SESSION_WRITE_DEBOUNCE_US (250 * 1000)
#define SESSION_WRITE_RETRY_US (1000 * 1000)
#define VIEW_ID_RESERVATION_SIZE 64u
#define MAX_PEER_ENVIRONMENT_BYTES (4u * 1024u * 1024u)
#define MAX_KITTY_SOCKET_BYTES 4096u
#define MAX_KITTY_WINDOW_ID_BYTES 20u
#define MAX_CLIENTS 128u
#define MAX_CLIENT_OUTPUT_BYTES (1024u * 1024u)
#define MAX_CLIENT_IO_BYTES_PER_TICK (64u * 1024u)
#define KITTY_MOVE_TIMEOUT_MS 2500u
#define ENSURE_STARTUP_TIMEOUT_MS 2000u
#define ENSURE_PING_TIMEOUT_MS 50
#define ENSURE_STARTUP_POLL_US 10000u
#define ENSURE_TERMINATE_GRACE_MS 500u
#define ENSURE_KILL_GRACE_MS 500u

static volatile sig_atomic_t stop_requested;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void replace_string(gchar **destination, gchar *value)
{
    g_free(*destination);
    *destination = value;
}

static gboolean layer_is_valid(const gchar *layer)
{
    gsize length;

    if (!layer)
        return FALSE;
    length = strlen(layer);
    if (length == 0 || length > 128)
        return FALSE;
    for (const guchar *cursor = (const guchar *)layer;
         *cursor;
         cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '.' &&
            *cursor != '_' && *cursor != '-')
            return FALSE;
    }
    return TRUE;
}

static gchar *decode_layer(const gchar *encoded)
{
    gchar *layer = mux_decode(encoded);

    if (!layer_is_valid(layer)) {
        g_free(layer);
        return NULL;
    }
    return layer;
}

static gboolean encoded_layer_is_valid(const gchar *encoded)
{
    g_autofree gchar *layer = decode_layer(encoded);

    return layer != NULL;
}

static gboolean peer_ephemeral_status(pid_t pid, gboolean *ephemeral)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    gsize offset = 0;

    g_return_val_if_fail(ephemeral != NULL, FALSE);
    *ephemeral = TRUE;
    if (pid <= 0)
        return FALSE;

    path = g_strdup_printf("/proc/%ld/environ", (long)pid);
    if (!g_file_get_contents(path, &contents, &length, NULL) ||
        length > MAX_PEER_ENVIRONMENT_BYTES)
        return FALSE;

    *ephemeral = FALSE;
    while (offset < length) {
        const gchar *entry = contents + offset;
        gsize remaining = length - offset;
        const gchar *terminator = memchr(entry, '\0', remaining);
        gsize entry_length = terminator
            ? (gsize)(terminator - entry)
            : remaining;
        const gchar prefix[] = "MUX_EPHEMERAL=";

        if (entry_length >= sizeof(prefix) - 1 &&
            memcmp(entry, prefix, sizeof(prefix) - 1) == 0) {
            const gchar *value = entry + sizeof(prefix) - 1;
            gsize value_length = entry_length - (sizeof(prefix) - 1);

            *ephemeral = !(value_length == 1 && value[0] == '0');
            return TRUE;
        }
        offset += entry_length + (terminator != NULL ? 1 : 0);
    }
    return TRUE;
}

static void schedule_session_write(Server *server)
{
    server->session_dirty = TRUE;
    server->session_write_due_us =
        g_get_monotonic_time() + SESSION_WRITE_DEBOUNCE_US;
}

static gboolean persist_session_now(Server *server)
{
    g_autoptr(GError) error = NULL;

    if (mux_session_state_save_atomic(server->session,
                                      server->session_path,
                                      &error)) {
        server->session_dirty = FALSE;
        server->session_write_due_us = 0;
        return TRUE;
    }
    g_printerr("muxd: cannot persist workspace: %s\n", error->message);
    server->session_dirty = TRUE;
    server->session_write_due_us =
        g_get_monotonic_time() + SESSION_WRITE_RETRY_US;
    return FALSE;
}

static void flush_session_if_due(Server *server)
{
    if (server->session_dirty && server->session_write_due_us > 0 &&
        g_get_monotonic_time() >= server->session_write_due_us)
        persist_session_now(server);
}

static gboolean allocate_persistent_view_id(Server *server,
                                            guint64 *view_id)
{
    if (server->next_view_id >= server->reserved_view_id_limit) {
        guint64 previous_limit =
            mux_session_state_get_next_view_id(server->session);
        guint64 new_limit;

        if (server->next_view_id >
            G_MAXUINT64 - VIEW_ID_RESERVATION_SIZE) {
            g_printerr("muxd: persistent view ID space is exhausted\n");
            return FALSE;
        }
        new_limit = server->next_view_id + VIEW_ID_RESERVATION_SIZE;
        if (!mux_session_state_set_next_view_id(server->session, new_limit) ||
            !persist_session_now(server)) {
            mux_session_state_set_next_view_id(server->session,
                                               previous_limit);
            return FALSE;
        }
        server->reserved_view_id_limit = new_limit;
    }

    *view_id = server->next_view_id++;
    return TRUE;
}

static void assign_transient_view_id(Server *server, Client *client)
{
    client->id = g_strdup_printf("transient-%ld-%" G_GUINT64_FORMAT,
                                 (long)client->peer_pid,
                                 server->next_transient_id++);
}

static void update_persisted_view(Server *server, Client *client)
{
    if (!client->persistable || client->session_view_id == 0)
        return;
    if (mux_session_state_upsert_view(server->session,
                                      client->session_view_id,
                                      client->layer,
                                      client->uri ? client->uri : "",
                                      client->title ? client->title : ""))
        schedule_session_write(server);
}

static gboolean session_contains_view_id(Server *server, guint64 view_id)
{
    guint count = mux_session_state_get_view_count(server->session);

    for (guint i = 0; i < count; i++) {
        const MuxSessionView *view =
            mux_session_state_get_view(server->session, i);

        if (view != NULL && view->id == view_id)
            return TRUE;
    }
    return FALSE;
}

static gboolean session_view_id_is_live(Server *server, guint64 view_id)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);

        if (client->kind == CLIENT_VIEW &&
            client->session_view_id == view_id)
            return TRUE;
    }
    return FALSE;
}

static gboolean parse_persistent_view_id(const gchar *id,
                                         guint64 *view_id)
{
    const gchar *number;
    gchar *end = NULL;
    guint64 parsed;

    if (id == NULL || !g_str_has_prefix(id, "view-"))
        return FALSE;
    number = id + strlen("view-");
    if (!g_ascii_isdigit(*number) || *number == '0')
        return FALSE;
    for (const gchar *cursor = number; *cursor != '\0'; cursor++) {
        if (!g_ascii_isdigit(*cursor))
            return FALSE;
    }

    errno = 0;
    parsed = g_ascii_strtoull(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || parsed == 0)
        return FALSE;
    *view_id = parsed;
    return TRUE;
}

static gboolean layer_has_persistable_view(Server *server,
                                           const gchar *layer)
{
    guint count = mux_session_state_get_view_count(server->session);

    for (guint i = 0; i < count; i++) {
        const MuxSessionView *view =
            mux_session_state_get_view(server->session, i);

        if (view != NULL && g_strcmp0(view->layer, layer) == 0)
            return TRUE;
    }
    return FALSE;
}

static void persist_active_layer_if_eligible(Server *server,
                                             const gchar *layer,
                                             const Client *source_view)
{
    if ((source_view != NULL && !source_view->persistable) ||
        !layer_has_persistable_view(server, layer))
        return;
    if (mux_session_state_set_active_layer(server->session, layer))
        schedule_session_write(server);
}

static gboolean client_queue_line(Client *client, const gchar *line)
{
    gsize line_length;
    gsize wire_length;
    gchar *wire;

    if (client->closing)
        return FALSE;

    line_length = strlen(line);
    if (line_length >= MAX_CLIENT_OUTPUT_BYTES) {
        client->closing = TRUE;
        return FALSE;
    }
    wire_length = line_length + 1u;
    if (client->output_bytes > MAX_CLIENT_OUTPUT_BYTES - wire_length) {
        client->closing = TRUE;
        return FALSE;
    }

    wire = g_malloc(wire_length);
    memcpy(wire, line, line_length);
    wire[line_length] = '\n';
    g_queue_push_tail(client->output,
                      g_bytes_new_take(wire, wire_length));
    client->output_bytes += wire_length;
    return TRUE;
}

static gboolean client_send_line(Client *client,
                                 const gchar *format,
                                 ...)
{
    va_list arguments;
    g_autofree gchar *line = NULL;

    va_start(arguments, format);
    line = g_strdup_vprintf(format, arguments);
    va_end(arguments);
    return client_queue_line(client, line);
}

static void client_close_after_flush(Client *client)
{
    client->request_pending = FALSE;
    client->close_after_flush = TRUE;
    if (client->output_bytes == 0)
        client->closing = TRUE;
}

static gboolean client_flush(Client *client)
{
    gsize flushed = 0;

    while (!client->closing && !g_queue_is_empty(client->output) &&
           flushed < MAX_CLIENT_IO_BYTES_PER_TICK) {
        GBytes *record = g_queue_peek_head(client->output);
        gsize record_length = 0;
        const guint8 *record_data = g_bytes_get_data(record,
                                                     &record_length);
        gsize remaining = record_length - client->output_offset;
        gsize send_length = MIN(remaining,
                                MAX_CLIENT_IO_BYTES_PER_TICK - flushed);
        ssize_t sent;

        do {
            sent = send(client->fd,
                        record_data + client->output_offset,
                        send_length,
                        MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);

        if (sent > 0) {
            client->output_offset += (gsize)sent;
            client->output_bytes -= (gsize)sent;
            flushed += (gsize)sent;
            if (client->output_offset == record_length) {
                g_bytes_unref(g_queue_pop_head(client->output));
                client->output_offset = 0;
            }
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return TRUE;
        client->closing = TRUE;
        return FALSE;
    }

    if (!client->closing && client->close_after_flush &&
        client->output_bytes == 0)
        client->closing = TRUE;
    return !client->closing;
}

static void client_free(gpointer data)
{
    Client *client = data;
    if (client->fd >= 0)
        close(client->fd);
    if (client->input)
        g_string_free(client->input, TRUE);
    if (client->output) {
        while (!g_queue_is_empty(client->output))
            g_bytes_unref(g_queue_pop_head(client->output));
        g_queue_free(client->output);
    }
    g_free(client->id);
    g_free(client->kitty_window);
    g_free(client->kitty_socket);
    g_free(client->layer);
    g_free(client->uri);
    g_free(client->title);
    g_free(client);
}

static Client *find_view(Server *server, const gchar *id)
{
    if (!id || !*id)
        id = server->active_id;
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (client->kind == CLIENT_VIEW && g_strcmp0(client->id, id) == 0)
            return client;
    }
    return NULL;
}

static Client *find_bar(Server *server, Client *view)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (client->kind == CLIENT_BAR &&
            g_strcmp0(client->kitty_socket, view->kitty_socket) == 0 &&
            g_strcmp0(client->layer, view->layer) == 0) {
            return client;
        }
    }
    return NULL;
}

static Client *find_layer_view(Server *server, const gchar *layer)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (client->kind == CLIENT_VIEW &&
            !client->closing && client->fd >= 0 &&
            g_strcmp0(client->layer, layer) == 0) {
            return client;
        }
    }
    return NULL;
}

static gboolean kitty_socket_is_valid(const gchar *socket)
{
    gsize length;

    if (!socket)
        return FALSE;
    length = strnlen(socket, MAX_KITTY_SOCKET_BYTES + 1u);
    if (length == 0 || length > MAX_KITTY_SOCKET_BYTES)
        return FALSE;
    for (gsize i = 0; i < length; i++) {
        guchar byte = (guchar)socket[i];

        if (byte < 0x20 || byte == 0x7f)
            return FALSE;
    }
    return TRUE;
}

static gboolean kitty_window_id_is_valid(const gchar *id)
{
    gchar *end = NULL;
    guint64 value;
    gsize length;

    if (!id)
        return FALSE;
    length = strnlen(id, MAX_KITTY_WINDOW_ID_BYTES + 1u);
    if (length == 0 || length > MAX_KITTY_WINDOW_ID_BYTES)
        return FALSE;
    for (gsize i = 0; i < length; i++) {
        if (!g_ascii_isdigit((guchar)id[i]))
            return FALSE;
    }

    errno = 0;
    value = g_ascii_strtoull(id, &end, 10);
    return errno != ERANGE && end == id + length && value > 0;
}

static void control_error(Client *client, const gchar *message);
static void broadcast_view(Server *server, Client *view, const gchar *event);
static void reconcile_active(Server *server, Client *preferred);

static void pending_move_destroy_source(GSource **source)
{
    if (*source == NULL)
        return;
    g_source_destroy(*source);
    g_source_unref(*source);
    *source = NULL;
}

static void pending_move_free(gpointer data)
{
    PendingMove *move = data;

    pending_move_destroy_source(&move->timeout_source);
    pending_move_destroy_source(&move->child_source);
    g_free(move->layer);
    g_free(move);
}

static void pending_move_kill(PendingMove *move)
{
    if (move->child_pid <= 0)
        return;
    while (kill((pid_t)move->child_pid, SIGKILL) < 0 && errno == EINTR)
        ;
}

static gboolean kitty_move_timed_out(gpointer user_data)
{
    PendingMove *move = user_data;

    move->timed_out = TRUE;
    pending_move_kill(move);
    return G_SOURCE_REMOVE;
}

static void kitty_move_child_exited(GPid child_pid,
                                    gint wait_status,
                                    gpointer user_data)
{
    PendingMove *move = user_data;
    Client *control = move->control;
    Client *source = move->source;
    gboolean source_live =
        source != NULL && source->kind == CLIENT_VIEW &&
        !source->closing && source->fd >= 0;
    gboolean accepted = g_spawn_check_wait_status(wait_status, NULL);

    pending_move_destroy_source(&move->timeout_source);
    pending_move_destroy_source(&move->child_source);
    g_spawn_close_pid(child_pid);
    move->child_pid = 0;

    if (!move->timed_out && !move->cancelled && accepted && source_live) {
        replace_string(&source->layer, g_steal_pointer(&move->layer));
        update_persisted_view(move->server, source);
        broadcast_view(move->server, source, "UPSERT");
        reconcile_active(move->server, NULL);
        if (control != NULL && !control->closing) {
            client_send_line(control, "OK");
            client_close_after_flush(control);
        }
    } else if (control != NULL && !control->closing) {
        if (move->timed_out) {
            control_error(control, "Kitty layer move timed out");
        } else if (move->source_lost || !source_live) {
            control_error(control,
                          "source view disconnected during Kitty layer move");
        } else if (move->cancelled) {
            control_error(control, "Kitty layer move was cancelled");
        } else {
            control_error(control, "Kitty rejected layer move");
        }
    }

    g_ptr_array_remove(move->server->pending_moves, move);
}

static gboolean kitty_move_view_async(Server *server,
                                      Client *control,
                                      Client *source,
                                      const Client *target,
                                      const gchar *layer)
{
    g_autofree gchar *source_match =
        g_strdup_printf("id:%s", source->kitty_window);
    g_autofree gchar *target_tab =
        g_strdup_printf("window_id:%s", target->kitty_window);
    gchar *argv[] = {
        "kitten",
        "@",
        "--use-password=always",
        "--to",
        source->kitty_socket,
        "detach-window",
        "--match",
        source_match,
        "--target-tab",
        target_tab,
        NULL,
    };
    PendingMove *move;
    GPid child_pid = 0;

    if (!g_spawn_async(NULL,
                       argv,
                       NULL,
                       G_SPAWN_SEARCH_PATH |
                           G_SPAWN_DO_NOT_REAP_CHILD |
                           G_SPAWN_STDIN_FROM_DEV_NULL |
                           G_SPAWN_STDOUT_TO_DEV_NULL |
                           G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL,
                       NULL,
                       &child_pid,
                       NULL))
        return FALSE;

    move = g_new0(PendingMove, 1);
    move->server = server;
    move->control = control;
    move->source = source;
    move->layer = g_strdup(layer);
    move->child_pid = child_pid;
    move->deadline_us =
        g_get_monotonic_time() + ((gint64)KITTY_MOVE_TIMEOUT_MS * 1000);
    move->child_source = g_child_watch_source_new(child_pid);
    g_source_set_callback(move->child_source,
                          G_SOURCE_FUNC(kitty_move_child_exited),
                          move,
                          NULL);
    g_source_attach(move->child_source, server->main_context);
    move->timeout_source = g_timeout_source_new(KITTY_MOVE_TIMEOUT_MS);
    g_source_set_callback(move->timeout_source,
                          kitty_move_timed_out,
                          move,
                          NULL);
    g_source_attach(move->timeout_source, server->main_context);
    g_ptr_array_add(server->pending_moves, move);
    control->request_pending = TRUE;
    return TRUE;
}

static gboolean source_has_pending_move(Server *server,
                                        const Client *source)
{
    for (guint i = 0; i < server->pending_moves->len; i++) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, i);

        if (move->source == source && !move->cancelled)
            return TRUE;
    }
    return FALSE;
}

static void pending_moves_detach_client(Server *server, Client *client)
{
    for (guint i = 0; i < server->pending_moves->len; i++) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, i);
        gboolean affected = FALSE;

        if (move->control == client) {
            move->control = NULL;
            affected = TRUE;
        }
        if (move->source == client) {
            move->source = NULL;
            move->source_lost = TRUE;
            affected = TRUE;
        }
        if (affected && !move->cancelled) {
            move->cancelled = TRUE;
            pending_move_destroy_source(&move->timeout_source);
            pending_move_kill(move);
        }
    }
}

static void pending_moves_shutdown(Server *server)
{
    while (server->pending_moves->len > 0) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, 0);
        gint wait_status;
        pid_t waited;

        pending_move_destroy_source(&move->timeout_source);
        pending_move_destroy_source(&move->child_source);
        pending_move_kill(move);
        do {
            waited = waitpid((pid_t)move->child_pid, &wait_status, 0);
        } while (waited < 0 && errno == EINTR);
        g_spawn_close_pid(move->child_pid);
        move->child_pid = 0;
        g_ptr_array_remove_index(server->pending_moves, 0);
    }
}

static gint pending_move_poll_timeout(Server *server, gint fallback_ms)
{
    gint timeout_ms = fallback_ms;
    gint64 now_us = g_get_monotonic_time();

    for (guint i = 0; i < server->pending_moves->len; i++) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, i);
        gint64 remaining_us = move->deadline_us - now_us;
        gint remaining_ms = remaining_us <= 0
            ? 0
            : (gint)MIN((remaining_us + 999) / 1000,
                        (gint64)G_MAXINT);

        timeout_ms = MIN(timeout_ms, remaining_ms);
    }
    return timeout_ms;
}

static gboolean kitty_focus(Client *client)
{
    if (!client || !client->kitty_window || !*client->kitty_window ||
        !client->kitty_socket || !*client->kitty_socket) {
        return FALSE;
    }

    g_autofree gchar *match =
        g_strdup_printf("id:%s", client->kitty_window);
    gchar *argv[] = {
        "kitten",
        "@",
        "--to",
        client->kitty_socket,
        "focus-window",
        "--match",
        match,
        NULL,
    };

    return g_spawn_async(NULL,
                         argv,
                         NULL,
                         G_SPAWN_SEARCH_PATH |
                             G_SPAWN_STDIN_FROM_DEV_NULL |
                             G_SPAWN_STDOUT_TO_DEV_NULL |
                             G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL,
                         NULL,
                         NULL,
                         NULL);
}

static guint view_count(Server *server)
{
    guint count = 0;
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (client->kind == CLIENT_VIEW)
            count++;
    }
    return count;
}

static gboolean send_snapshot_line(Client *client,
                                   const gchar *format,
                                   ...)
{
    va_list arguments;
    g_autofree gchar *line = NULL;

    va_start(arguments, format);
    line = g_strdup_vprintf(format, arguments);
    va_end(arguments);

    return client_queue_line(client, line);
}

static void broadcast_line(Server *server, const gchar *line)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if ((client->kind == CLIENT_SUBSCRIBER ||
             client->kind == CLIENT_BAR) &&
            !client_queue_line(client, line)) {
            client->closing = TRUE;
        }
    }
}

static void broadcast_view(Server *server, Client *view, const gchar *event)
{
    server->revision++;
    gchar *id = mux_encode(view->id);
    gchar *layer = mux_encode(view->layer);
    gchar *uri = mux_encode(view->uri);
    gchar *title = mux_encode(view->title);
    gchar *kitty = mux_encode(view->kitty_window);
    gchar *line = g_strdup_printf(
        "EVENT\t%" G_GUINT64_FORMAT "\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%ld",
        server->revision,
        event,
        id,
        layer,
        uri,
        title,
        view->focused ? 1 : 0,
        kitty,
        view->pid);
    broadcast_line(server, line);
    g_free(line);
    g_free(kitty);
    g_free(title);
    g_free(uri);
    g_free(layer);
    g_free(id);
}

static void broadcast_active(Server *server)
{
    server->revision++;
    gchar *active = mux_encode(server->active_id);
    gchar *line = g_strdup_printf(
        "EVENT\t%" G_GUINT64_FORMAT "\tACTIVE\t%s",
        server->revision,
        active);
    broadcast_line(server, line);
    g_free(line);
    g_free(active);
}

static void broadcast_layer(Server *server)
{
    server->revision++;
    gchar *layer = mux_encode(server->current_layer);
    gchar *line = g_strdup_printf(
        "EVENT\t%" G_GUINT64_FORMAT "\tLAYER\t%s",
        server->revision,
        layer);
    broadcast_line(server, line);
    g_free(line);
    g_free(layer);
}

static void set_active(Server *server, Client *view)
{
    if (!view || view->kind != CLIENT_VIEW || view->closing || view->fd < 0)
        return;

    if (g_strcmp0(server->current_layer, view->layer) != 0) {
        replace_string(&server->current_layer, g_strdup(view->layer));
        persist_active_layer_if_eligible(server,
                                         server->current_layer,
                                         view);
        broadcast_layer(server);
    }

    reconcile_active(server, view);
}

static gboolean view_is_live_in_current_layer(Server *server, Client *view)
{
    return view != NULL && view->kind == CLIENT_VIEW && !view->closing &&
           view->fd >= 0 &&
           g_strcmp0(view->layer, server->current_layer) == 0;
}

static void reconcile_active(Server *server, Client *preferred)
{
    Client *selected = preferred;

    if (!view_is_live_in_current_layer(server, selected)) {
        selected = find_view(server, server->active_id);
        if (!view_is_live_in_current_layer(server, selected))
            selected = find_layer_view(server, server->current_layer);
    }

    for (guint i = 0; i < server->clients->len; i++) {
        Client *candidate = g_ptr_array_index(server->clients, i);
        gboolean focused;

        if (candidate->kind != CLIENT_VIEW || candidate->closing)
            continue;
        focused = candidate == selected;
        if (candidate->focused != focused) {
            candidate->focused = focused;
            broadcast_view(server, candidate, "UPSERT");
        }
    }

    if (g_strcmp0(server->active_id,
                  selected != NULL ? selected->id : NULL) != 0) {
        replace_string(&server->active_id,
                       selected != NULL ? g_strdup(selected->id) : NULL);
        broadcast_active(server);
    }
}

static void send_snapshot(Server *server, Client *client)
{
    gboolean complete;
    gchar *active = mux_encode(server->active_id);
    gchar *layer = mux_encode(server->current_layer);
    complete = send_snapshot_line(
        client,
        "BEGIN\t%" G_GUINT64_FORMAT "\t%s\t%s",
        server->revision,
        active,
        layer);
    g_free(layer);
    g_free(active);

    for (guint i = 0; i < server->clients->len; i++) {
        Client *view = g_ptr_array_index(server->clients, i);
        if (!complete || view->kind != CLIENT_VIEW)
            continue;

        gchar *id = mux_encode(view->id);
        gchar *view_layer = mux_encode(view->layer);
        gchar *uri = mux_encode(view->uri);
        gchar *title = mux_encode(view->title);
        gchar *kitty = mux_encode(view->kitty_window);
        complete = send_snapshot_line(
            client,
            "VIEW\t%s\t%s\t%s\t%s\t%d\t%s\t%ld",
            id,
            view_layer,
            uri,
            title,
            view->focused ? 1 : 0,
            kitty,
            view->pid);
        g_free(kitty);
        g_free(title);
        g_free(uri);
        g_free(view_layer);
        g_free(id);
    }
    if (complete)
        send_snapshot_line(client, "END");
}

static void control_error(Client *client, const gchar *message)
{
    gchar *encoded = mux_encode(message);
    client_send_line(client, "ERR\t%s", encoded);
    g_free(encoded);
    client_close_after_flush(client);
}

static Client *control_target(Server *server, const gchar *encoded)
{
    gchar *target = mux_decode(encoded);
    Client *view = NULL;
    if (!*target || g_strcmp0(target, "-") == 0 ||
        g_strcmp0(target, "active") == 0) {
        view = find_view(server, server->active_id);
    } else {
        view = find_view(server, target);
    }
    g_free(target);
    return view;
}

static void handle_control(
    Server *server,
    Client *client,
    gchar **fields,
    guint field_count)
{
    const gchar *command = field_count > 1 ? fields[1] : "";

    if (g_strcmp0(command, "PING") == 0) {
        client_send_line(client, "PONG\t%d", MUX_PROTOCOL_VERSION);
        client_close_after_flush(client);
        return;
    }
    if (g_strcmp0(command, "LIST") == 0) {
        send_snapshot(server, client);
        client_close_after_flush(client);
        return;
    }
    if (g_strcmp0(command, "STATUS") == 0) {
        gchar *active = mux_encode(server->active_id);
        gchar *layer = mux_encode(server->current_layer);
        client_send_line(
            client,
            "STATUS\t%" G_GUINT64_FORMAT "\t%s\t%s\t%u",
            server->revision,
            active,
            layer,
            view_count(server));
        g_free(layer);
        g_free(active);
        client_close_after_flush(client);
        return;
    }
    if (g_strcmp0(command, "LAYER") == 0 && field_count >= 3) {
        gchar *layer = decode_layer(fields[2]);

        if (!layer) {
            control_error(client, "invalid layer identifier");
            return;
        }
        replace_string(&server->current_layer, layer);
        persist_active_layer_if_eligible(server,
                                         server->current_layer,
                                         NULL);
        broadcast_layer(server);
        Client *view = find_layer_view(server, server->current_layer);
        reconcile_active(server, view);
        if (view) {
            kitty_focus(view);
        }
        client_send_line(client, "OK");
        client_close_after_flush(client);
        return;
    }
    if (g_strcmp0(command, "FOCUS") == 0 && field_count >= 3) {
        Client *view = control_target(server, fields[2]);
        if (!view) {
            control_error(client, "view not found");
            return;
        }
        set_active(server, view);
        kitty_focus(view);
        client_send_line(client, "OK");
        client_close_after_flush(client);
        return;
    }
    if (g_strcmp0(command, "MOVE") == 0 && field_count >= 4) {
        Client *view = control_target(server, fields[2]);
        gchar *layer = decode_layer(fields[3]);
        Client *target = NULL;
        gboolean found_live_view = FALSE;
        gboolean found_invalid_identity = FALSE;
        gboolean found_other_kitty = FALSE;

        if (!view) {
            g_free(layer);
            control_error(client, "view not found");
            return;
        }
        if (!layer) {
            control_error(client, "invalid layer identifier");
            return;
        }

        if (g_strcmp0(view->layer, layer) == 0) {
            g_free(layer);
            client_send_line(client, "OK");
            client_close_after_flush(client);
            return;
        }
        if (view->closing || view->fd < 0) {
            g_free(layer);
            control_error(client, "source view is not live");
            return;
        }
        if (!kitty_socket_is_valid(view->kitty_socket) ||
            !kitty_window_id_is_valid(view->kitty_window)) {
            g_free(layer);
            control_error(client, "source view has an invalid Kitty identity");
            return;
        }

        for (guint i = 0; i < server->clients->len; i++) {
            Client *candidate = g_ptr_array_index(server->clients, i);

            if (candidate->kind != CLIENT_VIEW || candidate->closing ||
                candidate->fd < 0 ||
                g_strcmp0(candidate->layer, layer) != 0) {
                continue;
            }
            found_live_view = TRUE;
            if (!kitty_socket_is_valid(candidate->kitty_socket) ||
                !kitty_window_id_is_valid(candidate->kitty_window)) {
                found_invalid_identity = TRUE;
                continue;
            }
            if (g_strcmp0(candidate->kitty_socket, view->kitty_socket) != 0) {
                found_other_kitty = TRUE;
                continue;
            }
            target = candidate;
            break;
        }

        if (!target) {
            g_free(layer);
            if (!found_live_view) {
                control_error(client, "target layer has no live Kitty view");
            } else if (found_other_kitty && !found_invalid_identity) {
                control_error(client,
                              "target layer belongs to a different Kitty instance");
            } else if (found_invalid_identity && !found_other_kitty) {
                control_error(client,
                              "target layer has no valid Kitty window");
            } else {
                control_error(client,
                              "target layer has no safe compatible Kitty view");
            }
            return;
        }

        if (source_has_pending_move(server, view)) {
            g_free(layer);
            control_error(client,
                          "source view already has a pending Kitty layer move");
            return;
        }
        if (!kitty_move_view_async(server,
                                   client,
                                   view,
                                   target,
                                   layer)) {
            g_free(layer);
            control_error(client, "failed to execute Kitty layer move");
            return;
        }
        g_free(layer);
        return;
    }

    if ((g_strcmp0(command, "OPEN") == 0 && field_count >= 4) ||
        g_strcmp0(command, "BACK") == 0 ||
        g_strcmp0(command, "FORWARD") == 0 ||
        g_strcmp0(command, "RELOAD") == 0 ||
        g_strcmp0(command, "QUIT") == 0) {
        if (field_count < 3) {
            control_error(client, "missing target");
            return;
        }
        Client *view = control_target(server, fields[2]);
        if (!view) {
            control_error(client, "view not found");
            return;
        }
        const gchar *argument = field_count >= 4 ? fields[3] : "";
        if (!client_send_line(view, "DO\t%s\t%s", command, argument)) {
            control_error(client, "view connection failed");
            view->closing = TRUE;
            return;
        }
        client_send_line(client, "OK");
        client_close_after_flush(client);
        return;
    }

    control_error(client, "unknown or malformed command");
}

static void handle_view(
    Server *server,
    Client *client,
    gchar **fields,
    guint field_count)
{
    if (g_strcmp0(fields[0], "STATE") == 0 && field_count >= 3) {
        replace_string(&client->uri, mux_decode(fields[1]));
        replace_string(&client->title, mux_decode(fields[2]));
        update_persisted_view(server, client);
        broadcast_view(server, client, "UPSERT");
        return;
    }
    if (g_strcmp0(fields[0], "FOCUS") == 0 && field_count >= 2) {
        gboolean focused = g_strcmp0(fields[1], "1") == 0;
        if (focused)
            set_active(server, client);
        else if (client->focused) {
            client->focused = FALSE;
            broadcast_view(server, client, "UPSERT");
        }
        return;
    }
    if (g_strcmp0(fields[0], "LAYER") == 0 && field_count >= 2) {
        gchar *layer = decode_layer(fields[1]);

        if (!layer) {
            client->closing = TRUE;
            return;
        }
        replace_string(&client->layer, layer);
        update_persisted_view(server, client);
        broadcast_view(server, client, "UPSERT");
        reconcile_active(server, NULL);
        return;
    }
    if (g_strcmp0(fields[0], "PROMPT") == 0) {
        Client *bar = find_bar(server, client);
        if (bar && client_send_line(bar, "DO\tEDIT\t")) {
            kitty_focus(bar);
        }
        return;
    }
    if (g_strcmp0(fields[0], "BYE") == 0) {
        client->graceful_bye = TRUE;
        client->closing = TRUE;
        return;
    }
}

static void handle_bar(
    Server *server,
    Client *client,
    gchar **fields,
    guint field_count)
{
    if (g_strcmp0(fields[0], "OPEN") == 0 && field_count >= 2) {
        Client *view = find_view(server, server->active_id);
        if (view) {
            client_send_line(view, "DO\tOPEN\t%s", fields[1]);
            kitty_focus(view);
        }
        return;
    }
    if (g_strcmp0(fields[0], "CANCEL") == 0) {
        Client *view = find_view(server, server->active_id);
        if (view)
            kitty_focus(view);
        return;
    }
    if (g_strcmp0(fields[0], "BYE") == 0)
        client->closing = TRUE;
}

static void handle_line(Server *server, Client *client, const gchar *line)
{
    gchar **fields = g_strsplit(line, "\t", -1);
    guint field_count = g_strv_length(fields);
    if (!field_count) {
        g_strfreev(fields);
        return;
    }

    if (client->kind == CLIENT_VIEW) {
        handle_view(server, client, fields, field_count);
        g_strfreev(fields);
        return;
    }
    if (client->kind == CLIENT_BAR) {
        handle_bar(server, client, fields, field_count);
        g_strfreev(fields);
        return;
    }

    if (client->kind == CLIENT_UNKNOWN &&
        g_strcmp0(fields[0], "VIEW") == 0 &&
        field_count >= 7 && encoded_layer_is_valid(fields[5])) {
        g_autofree gchar *proposed_id = mux_decode(fields[1]);
        guint64 proposed_session_view_id = 0;
        gboolean ephemeral;

        client->kind = CLIENT_VIEW;
        client->pid = (long)client->peer_pid;
        client->kitty_window = mux_decode(fields[3]);
        client->kitty_socket = mux_decode(fields[4]);
        client->layer = mux_decode(fields[5]);
        client->uri = mux_decode(fields[6]);
        client->title = g_strdup("");
        client->persistable =
            peer_ephemeral_status(client->peer_pid, &ephemeral) &&
            !ephemeral;
        if (client->persistable &&
            parse_persistent_view_id(proposed_id,
                                     &proposed_session_view_id) &&
            session_contains_view_id(server,
                                     proposed_session_view_id) &&
            !session_view_id_is_live(server,
                                     proposed_session_view_id)) {
            client->session_view_id = proposed_session_view_id;
            client->id = g_strdup_printf("view-%" G_GUINT64_FORMAT,
                                         client->session_view_id);
        } else if (client->persistable &&
            allocate_persistent_view_id(server,
                                        &client->session_view_id)) {
            client->id = g_strdup_printf("view-%" G_GUINT64_FORMAT,
                                         client->session_view_id);
        } else {
            client->persistable = FALSE;
            client->session_view_id = 0;
            assign_transient_view_id(server, client);
        }
        g_autofree gchar *encoded_assigned_id = mux_encode(client->id);
        client_send_line(client,
                         "OK\t%d\t%s",
                         MUX_PROTOCOL_VERSION,
                         encoded_assigned_id);
        update_persisted_view(server, client);
        broadcast_view(server, client, "UPSERT");
        if (!server->active_id)
            set_active(server, client);
    } else if (client->kind == CLIENT_UNKNOWN &&
               g_strcmp0(fields[0], "BAR") == 0 &&
               field_count >= 6 && encoded_layer_is_valid(fields[5])) {
        client->kind = CLIENT_BAR;
        client->id = mux_decode(fields[1]);
        client->pid = strtol(fields[2], NULL, 10);
        client->kitty_window = mux_decode(fields[3]);
        client->kitty_socket = mux_decode(fields[4]);
        client->layer = mux_decode(fields[5]);
        client->uri = g_strdup("");
        client->title = g_strdup("");
        if (client_send_line(client,
                             "OK\t%d",
                             MUX_PROTOCOL_VERSION))
            send_snapshot(server, client);
    } else if (client->kind == CLIENT_UNKNOWN &&
               g_strcmp0(fields[0], "SUB") == 0) {
        client->kind = CLIENT_SUBSCRIBER;
        send_snapshot(server, client);
    } else if (client->kind == CLIENT_UNKNOWN &&
               g_strcmp0(fields[0], "CTL") == 0) {
        handle_control(server, client, fields, field_count);
    } else {
        client->closing = TRUE;
    }

    g_strfreev(fields);
}

static void client_read(Server *server, Client *client)
{
    gchar buffer[8192];
    gsize received = 0;

    while (!client->closing && !client->close_after_flush &&
           !client->request_pending &&
           received < MAX_CLIENT_IO_BYTES_PER_TICK) {
        ssize_t count;

        do {
            count = recv(client->fd,
                         buffer,
                         MIN(sizeof(buffer),
                             MAX_CLIENT_IO_BYTES_PER_TICK - received),
                         0);
        } while (count < 0 && errno == EINTR);

        if (count > 0) {
            received += (gsize)count;
            g_string_append_len(client->input, buffer, count);
            if (client->input->len > 1024 * 1024) {
                client->closing = TRUE;
                return;
            }

            while (!client->closing && !client->close_after_flush &&
                   !client->request_pending) {
                gchar *newline = memchr(client->input->str,
                                        '\n',
                                        client->input->len);
                if (!newline)
                    break;
                gsize line_length =
                    (gsize)(newline - client->input->str);
                gchar *line = g_strndup(client->input->str, line_length);
                g_strchomp(line);
                g_string_erase(client->input, 0, line_length + 1);
                handle_line(server, client, line);
                g_free(line);
            }
            continue;
        }
        if (count == 0) {
            client->closing = TRUE;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        client->closing = TRUE;
        return;
    }
}

static void remove_closed_clients(Server *server)
{
    for (gint i = (gint)server->clients->len - 1; i >= 0; i--) {
        Client *client = g_ptr_array_index(server->clients, (guint)i);
        if (!client->closing)
            continue;

        pending_moves_detach_client(server, client);

        if (client->kind == CLIENT_VIEW) {
            if (client->graceful_bye && client->persistable &&
                client->session_view_id != 0 &&
                mux_session_state_remove_view(server->session,
                                              client->session_view_id))
                schedule_session_write(server);
            broadcast_view(server, client, "REMOVE");
            reconcile_active(server, NULL);
        }
        g_ptr_array_remove_index(server->clients, (guint)i);
    }
}

static gboolean accept_client(Server *server)
{
    int fd = accept4(server->listener,
                     NULL,
                     NULL,
                     SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return FALSE;

    if (server->clients->len >= MAX_CLIENTS) {
        close(fd);
        return FALSE;
    }

    struct ucred credentials;
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &credentials,
                   &credentials_size) < 0 ||
        credentials_size != sizeof(credentials) ||
        credentials.uid != geteuid()) {
        close(fd);
        return FALSE;
    }

    Client *client = g_new0(Client, 1);
    client->fd = fd;
    client->peer_pid = credentials.pid;
    client->input = g_string_new(NULL);
    client->output = g_queue_new();
    g_ptr_array_add(server->clients, client);
    return TRUE;
}

static gboolean secure_runtime_directory(const gchar *directory)
{
    struct stat status;
    int directory_fd;

    if (lstat(directory, &status) < 0) {
        if (errno != ENOENT || mkdir(directory, 0700) < 0)
            return FALSE;
    }

    directory_fd = open(directory,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0)
        return FALSE;
    if (fstat(directory_fd, &status) < 0 ||
        !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0700) {
        close(directory_fd);
        errno = EPERM;
        return FALSE;
    }
    close(directory_fd);
    return TRUE;
}

static int acquire_daemon_lock(const gchar *directory)
{
    g_autofree gchar *path =
        g_build_filename(directory, "muxd.lock", NULL);
    struct stat status;
    int fd = open(path,
                  O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                  0600);

    if (fd < 0)
        return -1;
    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || fchmod(fd, 0600) < 0 ||
        flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int saved_errno = errno ? errno : EPERM;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int create_listener(gchar **path_out, int *lock_fd_out)
{
    gchar *path = mux_socket_path();
    gchar *directory = g_path_get_dirname(path);

    *lock_fd_out = -1;
    if (!secure_runtime_directory(directory)) {
        g_printerr("muxd: insecure runtime directory %s: %s\n",
                   directory,
                   g_strerror(errno));
        g_free(directory);
        g_free(path);
        return -1;
    }
    *lock_fd_out = acquire_daemon_lock(directory);
    g_free(directory);
    if (*lock_fd_out < 0) {
        g_free(path);
        return -1;
    }

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_printerr("muxd: socket path is too long\n");
        close(*lock_fd_out);
        *lock_fd_out = -1;
        g_free(path);
        return -1;
    }

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        close(*lock_fd_out);
        *lock_fd_out = -1;
        g_free(path);
        return -1;
    }

    struct sockaddr_un address = { 0 };
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

    mode_t old_mask = umask(0077);
    int result = bind(fd, (struct sockaddr *)&address, sizeof(address));
    umask(old_mask);
    if (result < 0 || listen(fd, 64) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        close(*lock_fd_out);
        *lock_fd_out = -1;
        g_free(path);
        errno = saved_errno;
        return -1;
    }
    if (chmod(path, 0600) < 0) {
        int saved_errno = errno;

        close(fd);
        unlink(path);
        close(*lock_fd_out);
        *lock_fd_out = -1;
        g_free(path);
        errno = saved_errno;
        return -1;
    }
    *path_out = path;
    return fd;
}

static void ensure_startup_test_hook(void)
{
    const gchar *pid_path = g_getenv("MUX_TEST_ENSURE_CHILD_PID_FILE");
    const gchar *delay_value = g_getenv("MUX_TEST_ENSURE_STARTUP_DELAY_MS");

    if (pid_path != NULL && *pid_path != '\0') {
        g_autofree gchar *pid_text =
            g_strdup_printf("%ld\n", (long)getpid());

        (void)g_file_set_contents(pid_path, pid_text, -1, NULL);
    }

    if (delay_value != NULL && *delay_value != '\0') {
        gchar *end = NULL;
        guint64 delay_ms;

        errno = 0;
        delay_ms = g_ascii_strtoull(delay_value, &end, 10);
        if (errno == 0 && end != delay_value && *end == '\0' &&
            delay_ms <= 60000u)
            g_usleep(delay_ms * 1000u);
    }
}

static int run_server(void)
{
    Server server = {
        .listener = -1,
        .lock_fd = -1,
        .clients = g_ptr_array_new_with_free_func(client_free),
        .pending_moves = g_ptr_array_new_with_free_func(pending_move_free),
        .current_layer = g_strdup("main"),
        .next_transient_id = 1,
        .running = TRUE,
        .main_context = g_main_context_new(),
    };

    server.listener = create_listener(&server.socket_path,
                                      &server.lock_fd);
    if (server.listener < 0) {
        g_printerr("muxd: cannot listen: %s\n", g_strerror(errno));
        g_ptr_array_unref(server.clients);
        g_ptr_array_unref(server.pending_moves);
        g_free(server.current_layer);
        g_main_context_unref(server.main_context);
        return EXIT_FAILURE;
    }

    ensure_startup_test_hook();

    server.session_path = mux_session_state_default_path();
    g_autoptr(GError) session_error = NULL;
    server.session = mux_session_state_load(server.session_path,
                                            &session_error);
    if (server.session == NULL) {
        guint64 recovery_floor = (guint64)MAX(g_get_real_time(), 1);

        g_printerr("muxd: ignoring unusable workspace state: %s\n",
                   session_error->message);
        server.session = mux_session_state_new();
        mux_session_state_set_next_view_id(server.session,
                                           recovery_floor);
    }
    replace_string(
        &server.current_layer,
        g_strdup(mux_session_state_get_active_layer(server.session)));
    server.next_view_id =
        mux_session_state_get_next_view_id(server.session);
    server.reserved_view_id_limit = server.next_view_id;

    g_autoptr(GError) clipboard_error = NULL;
    server.clipboard = muxd_clipboard_new(server.main_context,
                                          &clipboard_error);
    if (server.clipboard == NULL) {
        g_printerr("muxd: cannot start clipboard broker: %s\n",
                   clipboard_error->message);
        close(server.listener);
        unlink(server.socket_path);
        close(server.lock_fd);
        g_ptr_array_unref(server.clients);
        g_ptr_array_unref(server.pending_moves);
        g_free(server.socket_path);
        g_free(server.current_layer);
        g_free(server.session_path);
        mux_session_state_free(server.session);
        g_main_context_unref(server.main_context);
        return EXIT_FAILURE;
    }

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    while (server.running && !stop_requested) {
        guint client_count = server.clients->len;
        struct pollfd *poll_fds = g_new0(struct pollfd, client_count + 1);
        poll_fds[0].fd = server.listener;
        poll_fds[0].events = POLLIN;
        for (guint i = 0; i < client_count; i++) {
            Client *client = g_ptr_array_index(server.clients, i);
            poll_fds[i + 1].fd = client->fd;
            if (!client->closing && !client->close_after_flush &&
                !client->request_pending)
                poll_fds[i + 1].events |= POLLIN;
            if (!client->closing && client->output_bytes > 0)
                poll_fds[i + 1].events |= POLLOUT;
        }

        int result;
        do {
            result = poll(poll_fds,
                          client_count + 1,
                          pending_move_poll_timeout(&server, 50));
        } while (result < 0 && errno == EINTR && !stop_requested);

        while (g_main_context_iteration(server.main_context, FALSE))
            ;
        if (result > 0) {
            if (poll_fds[0].revents & POLLIN)
                accept_client(&server);
            for (guint i = 0; i < client_count; i++) {
                Client *client = g_ptr_array_index(server.clients, i);
                short events = poll_fds[i + 1].revents;
                if (events & POLLIN)
                    client_read(&server, client);
                if ((events & POLLOUT) && !client->closing)
                    client_flush(client);
                if (events & (POLLHUP | POLLERR | POLLNVAL))
                    client->closing = TRUE;
            }
        }
        g_free(poll_fds);
        while (g_main_context_iteration(server.main_context, FALSE))
            ;
        remove_closed_clients(&server);
        flush_session_if_due(&server);
    }

    if (server.session_dirty)
        persist_session_now(&server);
    pending_moves_shutdown(&server);
    muxd_clipboard_free(server.clipboard);
    g_main_context_unref(server.main_context);
    close(server.listener);
    unlink(server.socket_path);
    close(server.lock_fd);
    g_ptr_array_unref(server.clients);
    g_ptr_array_unref(server.pending_moves);
    g_free(server.socket_path);
    g_free(server.active_id);
    g_free(server.current_layer);
    g_free(server.session_path);
    mux_session_state_free(server.session);
    return EXIT_SUCCESS;
}

static gboolean daemon_alive_with_timeout(gint timeout_ms)
{
    int fd = mux_connect_socket();
    if (fd < 0)
        return FALSE;
    gboolean sent = mux_send_line(fd, "CTL\tPING");
    gchar *response = sent ? mux_read_line(fd, timeout_ms) : NULL;
    gboolean alive = response && g_str_has_prefix(response, "PONG\t");
    g_free(response);
    close(fd);
    return alive;
}

static gboolean daemon_alive(void)
{
    return daemon_alive_with_timeout(500);
}

typedef struct {
    gchar *path;
    dev_t device;
    ino_t inode;
    gboolean captured;
} EnsureOwnedSocket;

static void ensure_owned_socket_capture(EnsureOwnedSocket *owned, pid_t pid)
{
    struct ucred credentials;
    socklen_t credentials_size = sizeof(credentials);
    struct stat status;
    int fd;

    owned->path = mux_socket_path();
    fd = mux_connect_socket();
    if (fd < 0)
        return;
    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &credentials,
                   &credentials_size) == 0 &&
        credentials_size == sizeof(credentials) &&
        credentials.pid == pid && credentials.uid == geteuid() &&
        lstat(owned->path, &status) == 0 && S_ISSOCK(status.st_mode) &&
        status.st_uid == geteuid()) {
        owned->device = status.st_dev;
        owned->inode = status.st_ino;
        owned->captured = TRUE;
    }
    close(fd);
}

static void ensure_owned_socket_cleanup(EnsureOwnedSocket *owned)
{
    struct stat status;

    if (owned->captured && lstat(owned->path, &status) == 0 &&
        S_ISSOCK(status.st_mode) && status.st_uid == geteuid() &&
        status.st_dev == owned->device && status.st_ino == owned->inode &&
        unlink(owned->path) < 0 && errno != ENOENT) {
        g_printerr("muxd: failed to remove owned stale socket: %s\n",
                   g_strerror(errno));
    }
    g_clear_pointer(&owned->path, g_free);
}

static gboolean terminate_and_reap_ensure_child(pid_t pid)
{
    gint64 deadline_us =
        g_get_monotonic_time() +
        ((gint64)ENSURE_TERMINATE_GRACE_MS * 1000);
    int status;

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
        return FALSE;

    while (g_get_monotonic_time() < deadline_us) {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid)
            return TRUE;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD;
        g_usleep(10000);
    }

    if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
        return FALSE;
    deadline_us = g_get_monotonic_time() +
        ((gint64)ENSURE_KILL_GRACE_MS * 1000);
    while (g_get_monotonic_time() < deadline_us) {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid)
            return TRUE;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD;
        g_usleep(10000);
    }
    return FALSE;
}

static int ensure_daemon(void)
{
    if (daemon_alive())
        return EXIT_SUCCESS;

    pid_t pid = fork();
    if (pid < 0) {
        g_printerr("muxd: fork failed: %s\n", g_strerror(errno));
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (setsid() < 0)
            _exit(EXIT_FAILURE);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        _exit(run_server());
    }

    gint64 startup_deadline_us =
        g_get_monotonic_time() +
        ((gint64)ENSURE_STARTUP_TIMEOUT_MS * 1000);

    while (g_get_monotonic_time() < startup_deadline_us) {
        int status;
        pid_t wait_result;

        if (daemon_alive_with_timeout(ENSURE_PING_TIMEOUT_MS))
            return EXIT_SUCCESS;
        wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            g_printerr("muxd: daemon exited before becoming ready\n");
            return EXIT_FAILURE;
        }
        if (wait_result < 0 && errno != EINTR) {
            g_printerr("muxd: cannot monitor daemon startup: %s\n",
                       g_strerror(errno));
            return EXIT_FAILURE;
        }
        g_usleep(ENSURE_STARTUP_POLL_US);
    }

    EnsureOwnedSocket owned = { 0 };

    ensure_owned_socket_capture(&owned, pid);
    if (!terminate_and_reap_ensure_child(pid)) {
        g_printerr("muxd: failed to terminate startup child %ld\n", (long)pid);
        g_clear_pointer(&owned.path, g_free);
    } else {
        ensure_owned_socket_cleanup(&owned);
    }
    g_printerr("muxd: daemon did not become ready\n");
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc > 1 && g_strcmp0(argv[1], "--ensure") == 0)
        return ensure_daemon();
    if (argc > 1 && g_strcmp0(argv[1], "--foreground") != 0) {
        g_printerr("usage: muxd [--ensure|--foreground]\n");
        return EXIT_FAILURE;
    }
    if (daemon_alive()) {
        g_printerr("muxd: already running\n");
        return EXIT_FAILURE;
    }
    return run_server();
}
