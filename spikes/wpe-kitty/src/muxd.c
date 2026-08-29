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
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    CLIENT_UNKNOWN,
    CLIENT_VIEW,
    CLIENT_SUBSCRIBER,
    CLIENT_BAR,
    CLIENT_ENGINE,
} ClientKind;

typedef struct _EngineRegistration EngineRegistration;

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
    gchar *profile;
    gchar *layer;
    gchar *uri;
    gchar *title;
    long pid;
    pid_t peer_pid;
    int peer_pidfd;
    gboolean stop_term_sent;
    gboolean stop_kill_sent;
    gboolean stop_abandon;
    guint64 session_view_id;
    gboolean focused;
    EngineRegistration *engine;
} Client;

struct _EngineRegistration {
    gchar *profile;
    pid_t peer_pid;
    int peer_pidfd;
    Client *client;
    gboolean stop_term_sent;
    gboolean stop_kill_sent;
    gboolean stop_abandon;
};

typedef struct {
    int listener;
    int lock_fd;
    GPtrArray *clients;
    GPtrArray *engines;
    gchar *socket_path;
    gchar *active_id;
    gchar *current_layer;
    guint64 revision;
    guint64 next_view_id;
    guint64 reserved_view_id_limit;
    guint64 next_transient_id;
    gboolean running;
    gboolean stopping;
    gboolean stop_term_sent;
    gboolean stop_kill_sent;
    gboolean stop_cleanup_failed;
    gboolean stop_engines_requested;
    gboolean stop_engine_term_sent;
    gboolean stop_engine_kill_sent;
    gint64 stop_deadline_us;
    Client *stop_client;
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
#define MAX_ENGINES 32u
#define ENGINE_REGISTRY_VERSION 1u
#define MAX_ENGINE_PROFILE_BYTES 64u
#define MAX_CLIENT_OUTPUT_BYTES (1024u * 1024u)
#define MAX_CLIENT_IO_BYTES_PER_TICK (64u * 1024u)
#define KITTY_MOVE_TIMEOUT_MS 2500u
#define ENSURE_STARTUP_TIMEOUT_MS 2000u
#define ENSURE_PING_TIMEOUT_MS 50
#define ENSURE_STARTUP_POLL_US 10000u
#define ENSURE_TERMINATE_GRACE_MS 500u
#define ENSURE_KILL_GRACE_MS 500u
#define STOP_CLIENT_GRACE_MS 1500u
#define STOP_CLIENT_TERM_GRACE_MS 500u
#define STOP_CLIENT_KILL_GRACE_MS 500u
#define STOP_ENGINE_GRACE_MS 1000u
#define STOP_ENGINE_TERM_GRACE_MS 250u
#define STOP_ENGINE_KILL_GRACE_MS 250u
#define STOP_OWNED_CHILD_REAP_MS 500u
#define STOP_REPLY_FLUSH_MS 500u

static volatile sig_atomic_t stop_requested;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int peer_pidfd_open(pid_t pid)
{
#ifdef SYS_pidfd_open
    int fd;

    do {
        fd = (int)syscall(SYS_pidfd_open, pid, 0);
    } while (fd < 0 && errno == EINTR);
    return fd;
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

static gboolean peer_pidfd_signal(Client *client, int signal_number)
{
#ifdef SYS_pidfd_send_signal
    long result;

    if (client->peer_pidfd < 0) {
        errno = ENOTSUP;
        return FALSE;
    }
    do {
        result = syscall(SYS_pidfd_send_signal,
                         client->peer_pidfd,
                         signal_number,
                         NULL,
                         0);
    } while (result < 0 && errno == EINTR);
    return result == 0 || errno == ESRCH;
#else
    (void)client;
    (void)signal_number;
    errno = ENOSYS;
    return FALSE;
#endif
}

static gboolean peer_pidfd_exited(Client *client)
{
    struct pollfd descriptor = {
        .fd = client->peer_pidfd,
        .events = POLLIN,
    };
    int result;

    if (client->peer_pidfd < 0)
        return FALSE;
    do {
        result = poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    return result > 0 &&
        (descriptor.revents & (POLLIN | POLLHUP | POLLERR));
}

static gboolean engine_pidfd_signal(EngineRegistration *engine,
                                    int signal_number)
{
#ifdef SYS_pidfd_send_signal
    long result;

    if (engine->peer_pidfd < 0) {
        errno = ENOTSUP;
        return FALSE;
    }
    do {
        result = syscall(SYS_pidfd_send_signal,
                         engine->peer_pidfd,
                         signal_number,
                         NULL,
                         0);
    } while (result < 0 && errno == EINTR);
    return result == 0 || errno == ESRCH;
#else
    (void)engine;
    (void)signal_number;
    errno = ENOSYS;
    return FALSE;
#endif
}

static gboolean engine_pidfd_exited(EngineRegistration *engine)
{
    struct pollfd descriptor = {
        .fd = engine->peer_pidfd,
        .events = POLLIN,
    };
    int result;

    if (engine->peer_pidfd < 0)
        return FALSE;
    do {
        result = poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    return result > 0 &&
        (descriptor.revents & (POLLIN | POLLHUP | POLLERR));
}

static void engine_registration_free(gpointer data)
{
    EngineRegistration *engine = data;

    if (!engine)
        return;
    if (engine->client && engine->client->engine == engine)
        engine->client->engine = NULL;
    if (engine->peer_pidfd >= 0)
        close(engine->peer_pidfd);
    g_free(engine->profile);
    g_free(engine);
}

static gboolean signal_stopping_view(Server *server,
                                     Client *client,
                                     int signal_number)
{
    if (peer_pidfd_signal(client, signal_number))
        return TRUE;

    server->stop_cleanup_failed = TRUE;
    client->stop_abandon = TRUE;
    g_warning("cannot signal tracked pane %ld safely: %s",
              (long)client->peer_pid,
              g_strerror(errno));
    return FALSE;
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

static gboolean profile_is_valid(const gchar *profile)
{
    gsize length;

    if (!profile)
        return FALSE;
    length = strlen(profile);
    if (length == 0 || length > 64)
        return FALSE;
    for (const guchar *cursor = (const guchar *)profile;
         *cursor != '\0';
         cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '.' &&
            *cursor != '_' && *cursor != '-')
            return FALSE;
    }
    return TRUE;
}

static gboolean peer_view_environment(pid_t pid,
                                      gboolean *ephemeral,
                                      gchar **profile)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    gsize offset = 0;

    g_return_val_if_fail(ephemeral != NULL && profile != NULL, FALSE);
    *ephemeral = TRUE;
    *profile = g_strdup("default");
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
        const gchar ephemeral_prefix[] = "MUX_EPHEMERAL=";
        const gchar profile_prefix[] = "MUX_PROFILE=";

        if (entry_length >= sizeof(ephemeral_prefix) - 1 &&
            memcmp(entry,
                   ephemeral_prefix,
                   sizeof(ephemeral_prefix) - 1) == 0) {
            const gchar *value = entry + sizeof(ephemeral_prefix) - 1;
            gsize value_length =
                entry_length - (sizeof(ephemeral_prefix) - 1);

            *ephemeral = !(value_length == 1 && value[0] == '0');
        } else if (entry_length >= sizeof(profile_prefix) - 1 &&
                   memcmp(entry,
                          profile_prefix,
                          sizeof(profile_prefix) - 1) == 0) {
            g_autofree gchar *candidate = g_strndup(
                entry + sizeof(profile_prefix) - 1,
                entry_length - (sizeof(profile_prefix) - 1));

            if (!profile_is_valid(candidate))
                return FALSE;
            g_free(*profile);
            *profile = g_steal_pointer(&candidate);
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
    if (mux_session_state_upsert_view_with_profile(
            server->session,
            client->session_view_id,
            client->profile ? client->profile : "default",
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

static gboolean view_is_live(const Client *client)
{
    return client != NULL && client->kind == CLIENT_VIEW &&
        !client->closing && client->fd >= 0;
}

static gboolean session_view_id_is_live(Server *server, guint64 view_id)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);

        if (view_is_live(client) &&
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

static gboolean client_flush_bounded(Client *client, guint timeout_ms)
{
    gint64 deadline_us = g_get_monotonic_time() +
        ((gint64)timeout_ms * 1000);

    while (!client->closing && client->output_bytes > 0) {
        struct pollfd descriptor = {
            .fd = client->fd,
            .events = POLLOUT,
        };
        gint64 remaining_us;
        gint wait_ms;
        int result;

        (void)client_flush(client);
        if (client->output_bytes == 0)
            return TRUE;
        if (client->closing)
            return FALSE;

        remaining_us = deadline_us - g_get_monotonic_time();
        if (remaining_us <= 0)
            return FALSE;
        wait_ms = (gint)MIN((remaining_us + 999) / 1000,
                            (gint64)G_MAXINT);
        do {
            result = poll(&descriptor, 1, wait_ms);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 ||
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return FALSE;
    }
    return client->output_bytes == 0;
}

static void client_free(gpointer data)
{
    Client *client = data;
    if (client->engine && client->engine->client == client)
        client->engine->client = NULL;
    if (client->fd >= 0)
        close(client->fd);
    if (client->peer_pidfd >= 0)
        close(client->peer_pidfd);
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
    g_free(client->profile);
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
        if (view_is_live(client) && g_strcmp0(client->id, id) == 0)
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
            g_strcmp0(client->layer, view->layer) == 0 &&
            g_strcmp0(client->profile, view->profile) == 0) {
            return client;
        }
    }
    return NULL;
}

static Client *find_layer_view(Server *server, const gchar *layer)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (view_is_live(client) &&
            g_strcmp0(client->layer, layer) == 0) {
            return client;
        }
    }
    return NULL;
}

static gboolean view_matches_bar(const Client *view, const Client *bar)
{
    return view != NULL && bar != NULL && bar->kind == CLIENT_BAR &&
        g_strcmp0(view->kitty_socket, bar->kitty_socket) == 0 &&
        g_strcmp0(view->layer, bar->layer) == 0 &&
        g_strcmp0(view->profile, bar->profile) == 0;
}

static Client *find_bar_active_view(Server *server, Client *bar)
{
    Client *active = find_view(server, server->active_id);

    if (view_matches_bar(active, bar))
        return active;
    for (guint i = 0; i < server->clients->len; i++) {
        Client *candidate = g_ptr_array_index(server->clients, i);

        if (view_is_live(candidate) && view_matches_bar(candidate, bar))
            return candidate;
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

static gboolean pending_moves_shutdown(Server *server)
{
    gint64 deadline_us = g_get_monotonic_time() +
        ((gint64)STOP_OWNED_CHILD_REAP_MS * 1000);
    gboolean complete = TRUE;

    for (guint i = 0; i < server->pending_moves->len; i++) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, i);

        pending_move_destroy_source(&move->timeout_source);
        pending_move_destroy_source(&move->child_source);
        pending_move_kill(move);
    }

    while (server->pending_moves->len > 0 &&
           g_get_monotonic_time() < deadline_us) {
        for (gint i = (gint)server->pending_moves->len - 1; i >= 0; i--) {
            PendingMove *move =
                g_ptr_array_index(server->pending_moves, (guint)i);
            gint wait_status;
            pid_t waited;

            do {
                waited = waitpid((pid_t)move->child_pid,
                                 &wait_status,
                                 WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited != (pid_t)move->child_pid &&
                !(waited < 0 && errno == ECHILD))
                continue;
            g_spawn_close_pid(move->child_pid);
            move->child_pid = 0;
            g_ptr_array_remove_index(server->pending_moves, (guint)i);
        }
        if (server->pending_moves->len > 0)
            (void)poll(NULL, 0, 10);
    }

    while (server->pending_moves->len > 0) {
        PendingMove *move = g_ptr_array_index(server->pending_moves, 0);

        g_printerr("muxd: owned Kitty move child %ld was not reaped\n",
                   (long)move->child_pid);
        complete = FALSE;
        g_spawn_close_pid(move->child_pid);
        move->child_pid = 0;
        g_ptr_array_remove_index(server->pending_moves, 0);
    }
    return complete;
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

    g_autofree gchar *standard_output = NULL;
    g_autofree gchar *standard_error = NULL;
    g_autoptr(GError) error = NULL;
    gint status = 0;

    if (!g_spawn_sync(NULL,
                      argv,
                      NULL,
                      G_SPAWN_SEARCH_PATH,
                      NULL,
                      NULL,
                      &standard_output,
                      &standard_error,
                      &status,
                      &error) ||
        !g_spawn_check_wait_status(status, &error)) {
        g_warning("Kitty focus-window failed: %s%s%s",
                  error ? error->message : "unknown failure",
                  standard_error && *standard_error ? ": " : "",
                  standard_error && *standard_error
                      ? g_strstrip(standard_error)
                      : "");
        return FALSE;
    }
    return TRUE;
}

static guint view_count(Server *server)
{
    guint count = 0;
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if (view_is_live(client))
            count++;
    }
    return count;
}

static guint engine_count(Server *server)
{
    return server->engines ? server->engines->len : 0;
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
        if (client->kind == CLIENT_SUBSCRIBER &&
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
    for (guint i = 0; i < server->clients->len; i++) {
        Client *recipient = g_ptr_array_index(server->clients, i);

        if ((recipient->kind == CLIENT_SUBSCRIBER ||
             (recipient->kind == CLIENT_BAR &&
              view_matches_bar(view, recipient))) &&
            !client_queue_line(recipient, line))
            recipient->closing = TRUE;
    }
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
    for (guint i = 0; i < server->clients->len; i++) {
        Client *recipient = g_ptr_array_index(server->clients, i);
        Client *active_view;
        g_autofree gchar *active = NULL;
        g_autofree gchar *line = NULL;

        if (recipient->kind != CLIENT_SUBSCRIBER &&
            recipient->kind != CLIENT_BAR)
            continue;
        active_view = recipient->kind == CLIENT_BAR
            ? find_bar_active_view(server, recipient)
            : find_view(server, server->active_id);
        active = mux_encode(active_view ? active_view->id : NULL);
        line = g_strdup_printf(
            "EVENT\t%" G_GUINT64_FORMAT "\tACTIVE\t%s",
            server->revision,
            active);
        if (!client_queue_line(recipient, line))
            recipient->closing = TRUE;
    }
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
    return view_is_live(view) &&
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

        if (!view_is_live(candidate))
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
    Client *snapshot_active = client->kind == CLIENT_BAR
        ? find_bar_active_view(server, client)
        : find_view(server, server->active_id);
    gchar *active = mux_encode(snapshot_active ? snapshot_active->id : NULL);
    gchar *layer = mux_encode(client->kind == CLIENT_BAR
                                  ? client->layer
                                  : server->current_layer);
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
        if (!complete || !view_is_live(view) ||
            (client->kind == CLIENT_BAR && !view_matches_bar(view, client)))
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

static gboolean engine_profile_is_valid(const gchar *profile)
{
    gsize length;

    if (!profile || !*profile ||
        g_str_equal(profile, ".") || g_str_equal(profile, ".."))
        return FALSE;
    length = strlen(profile);
    if (length > MAX_ENGINE_PROFILE_BYTES)
        return FALSE;
    for (gsize i = 0; i < length; i++) {
        const gchar byte = profile[i];

        if (!g_ascii_isalnum(byte) && byte != '-' &&
            byte != '_' && byte != '.')
            return FALSE;
    }
    return TRUE;
}

static gchar *decode_engine_profile(const gchar *encoded)
{
    g_autofree gchar *profile = NULL;
    g_autofree gchar *canonical = NULL;

    if (!encoded || strlen(encoded) > 4 * MAX_ENGINE_PROFILE_BYTES + 4)
        return NULL;
    profile = mux_decode(encoded);
    if (!engine_profile_is_valid(profile))
        return NULL;
    canonical = mux_encode(profile);
    if (g_strcmp0(canonical, encoded) != 0)
        return NULL;
    return g_steal_pointer(&profile);
}

static gboolean parse_engine_pid(const gchar *text, pid_t *pid)
{
    gchar *end = NULL;
    gint64 parsed;

    if (!text || !*text)
        return FALSE;
    errno = 0;
    parsed = g_ascii_strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed <= 0 || parsed > G_MAXINT)
        return FALSE;
    *pid = (pid_t)parsed;
    return TRUE;
}

static EngineRegistration *find_engine(Server *server,
                                       const gchar *profile)
{
    for (guint i = 0; i < server->engines->len; i++) {
        EngineRegistration *engine =
            g_ptr_array_index(server->engines, i);

        if (g_strcmp0(engine->profile, profile) == 0)
            return engine;
    }
    return NULL;
}

static void remove_exited_engines(Server *server)
{
    if (!server->engines)
        return;
    for (gint i = (gint)server->engines->len - 1; i >= 0; i--) {
        EngineRegistration *engine =
            g_ptr_array_index(server->engines, (guint)i);

        if (!engine_pidfd_exited(engine))
            continue;
        if (engine->client) {
            engine->client->engine = NULL;
            engine->client->closing = TRUE;
            engine->client = NULL;
        }
        g_ptr_array_remove_index(server->engines, (guint)i);
    }
}

static gboolean register_engine(Server *server,
                                Client *client,
                                gchar **fields,
                                guint field_count)
{
    g_autofree gchar *profile = NULL;
    EngineRegistration *engine;
    pid_t claimed_pid;

    if (field_count != 4 ||
        g_strcmp0(fields[1], "1") != 0 ||
        !parse_engine_pid(fields[3], &claimed_pid) ||
        claimed_pid != client->peer_pid || client->peer_pidfd < 0 ||
        !(profile = decode_engine_profile(fields[2]))) {
        control_error(client, "invalid engine registration");
        return FALSE;
    }

    remove_exited_engines(server);
    engine = find_engine(server, profile);
    if (engine) {
        if (engine->peer_pid != client->peer_pid || engine->client) {
            control_error(client, "engine profile is already registered");
            return FALSE;
        }
        close(client->peer_pidfd);
        client->peer_pidfd = -1;
    } else {
        if (server->engines->len >= MAX_ENGINES) {
            control_error(client, "engine registry is full");
            return FALSE;
        }
        engine = g_new0(EngineRegistration, 1);
        engine->profile = g_steal_pointer(&profile);
        engine->peer_pid = client->peer_pid;
        engine->peer_pidfd = client->peer_pidfd;
        client->peer_pidfd = -1;
        g_ptr_array_add(server->engines, engine);
    }

    client->kind = CLIENT_ENGINE;
    client->engine = engine;
    engine->client = client;
    if (!client_send_line(client,
                          "ENGINE_OK\t%u",
                          ENGINE_REGISTRY_VERSION))
        client->closing = TRUE;
    return !client->closing;
}

static void handle_engine(Client *client,
                          gchar **fields,
                          guint field_count)
{
    if (field_count == 1 &&
        g_strcmp0(fields[0], "ENGINE_BYE") == 0) {
        client->graceful_bye = TRUE;
        client->closing = TRUE;
        return;
    }
    client->closing = TRUE;
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

static gboolean begin_controlled_stop(Server *server, Client *control)
{
    if (server->stopping) {
        control_error(control, "daemon is already stopping");
        return FALSE;
    }
    if (server->socket_path != NULL &&
        unlink(server->socket_path) < 0 && errno != ENOENT) {
        control_error(control, "cannot remove daemon socket");
        return FALSE;
    }
    if (server->listener >= 0) {
        close(server->listener);
        server->listener = -1;
    }

    server->stopping = TRUE;
    server->stop_client = control;
    server->stop_deadline_us = g_get_monotonic_time() +
        ((gint64)STOP_CLIENT_GRACE_MS * 1000);
    control->request_pending = TRUE;

    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);

        if (client == control)
            continue;
        if (client->kind == CLIENT_VIEW && !client->closing) {
            if (!client_send_line(client, "DO\tQUIT\t"))
                client->closing = TRUE;
        } else if (client->kind != CLIENT_ENGINE) {
            client->closing = TRUE;
        }
    }
    return TRUE;
}

static void handle_control(
    Server *server,
    Client *client,
    gchar **fields,
    guint field_count)
{
    const gchar *command = field_count > 1 ? fields[1] : "";

    if (g_strcmp0(command, "STOP") == 0 && field_count == 2) {
        (void)begin_controlled_stop(server, client);
        return;
    }

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
        Client *view = find_bar_active_view(server, client);
        if (view) {
            client_send_line(view, "DO\tOPEN\t%s", fields[1]);
            kitty_focus(view);
        }
        return;
    }
    if (g_strcmp0(fields[0], "CANCEL") == 0) {
        Client *view = find_bar_active_view(server, client);
        if (view)
            kitty_focus(view);
        return;
    }
    if (g_strcmp0(fields[0], "CLOSE") == 0) {
        Client *view = find_bar_active_view(server, client);

        if (view && !client_send_line(view, "DO\tQUIT\t"))
            view->closing = TRUE;
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
    if (client->kind == CLIENT_ENGINE) {
        handle_engine(client, fields, field_count);
        g_strfreev(fields);
        return;
    }

    if (client->kind == CLIENT_UNKNOWN &&
        g_strcmp0(fields[0], "ENGINE") == 0) {
        (void)register_engine(server, client, fields, field_count);
    } else if (client->kind == CLIENT_UNKNOWN &&
        g_strcmp0(fields[0], "VIEW") == 0 &&
        field_count >= 7 && encoded_layer_is_valid(fields[5])) {
        g_autofree gchar *proposed_id = mux_decode(fields[1]);
        g_autofree gchar *peer_profile = NULL;
        guint64 proposed_session_view_id = 0;
        gboolean ephemeral;

        client->kind = CLIENT_VIEW;
        client->pid = (long)client->peer_pid;
        client->kitty_window = mux_decode(fields[3]);
        client->kitty_socket = mux_decode(fields[4]);
        client->layer = mux_decode(fields[5]);
        client->uri = mux_decode(fields[6]);
        client->title = g_strdup("");
        client->persistable = peer_view_environment(client->peer_pid,
                                                    &ephemeral,
                                                    &peer_profile) &&
            !ephemeral;
        client->profile = g_steal_pointer(&peer_profile);
        if (!client->profile)
            client->profile = g_strdup("default");
        Client *duplicate = find_view(server, proposed_id);
        if (duplicate && duplicate != client &&
            duplicate->peer_pid == client->peer_pid &&
            g_strcmp0(duplicate->kitty_window, client->kitty_window) == 0 &&
            g_strcmp0(duplicate->kitty_socket, client->kitty_socket) == 0)
            duplicate->closing = TRUE;
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
        client->profile = field_count >= 7
            ? mux_decode(fields[6])
            : g_strdup("default");
        client->uri = g_strdup("");
        client->title = g_strdup("");
        if (!profile_is_valid(client->profile)) {
            client->closing = TRUE;
        } else if (client_send_line(client,
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

        if (server->stopping && client->kind == CLIENT_VIEW &&
            !client->stop_abandon) {
            if (client->fd >= 0) {
                close(client->fd);
                client->fd = -1;
            }
            if (!peer_pidfd_exited(client)) {
                if (!client->graceful_bye &&
                    !client->stop_term_sent &&
                    signal_stopping_view(server, client, SIGTERM))
                    client->stop_term_sent = TRUE;
                if (!client->stop_abandon)
                    continue;
            }
        }

        if (client == server->stop_client)
            server->stop_client = NULL;

        if (client->kind == CLIENT_VIEW) {
            Client *replacement = find_view(server, client->id);

            if (!replacement && !server->stopping && client->graceful_bye &&
                client->persistable &&
                client->session_view_id != 0 &&
                mux_session_state_remove_view(server->session,
                                              client->session_view_id))
                schedule_session_write(server);
            if (!replacement)
                broadcast_view(server, client, "REMOVE");
            reconcile_active(server, NULL);
        }
        if (client->kind == CLIENT_ENGINE && client->engine &&
            client->engine->client == client)
            client->engine->client = NULL;
        g_ptr_array_remove_index(server->clients, (guint)i);
    }
}

static gboolean signal_stopping_engine(Server *server,
                                       EngineRegistration *engine,
                                       int signal_number)
{
    if (engine_pidfd_signal(engine, signal_number))
        return TRUE;

    server->stop_cleanup_failed = TRUE;
    engine->stop_abandon = TRUE;
    g_warning("cannot signal tracked engine %s pid %ld safely: %s",
              engine->profile,
              (long)engine->peer_pid,
              g_strerror(errno));
    return FALSE;
}

static void request_engine_shutdown(Server *server, gint64 now_us)
{
    for (guint i = 0; i < server->engines->len; i++) {
        EngineRegistration *engine =
            g_ptr_array_index(server->engines, i);

        if (engine->client && !engine->client->closing &&
            !client_send_line(engine->client, "ENGINE_STOP"))
            engine->client->closing = TRUE;
    }
    server->stop_engines_requested = TRUE;
    server->stop_deadline_us = now_us +
        ((gint64)STOP_ENGINE_GRACE_MS * 1000);
}

static void update_controlled_stop(Server *server)
{
    gint64 now_us;

    if (!server->stopping)
        return;
    now_us = g_get_monotonic_time();
    if (view_count(server) > 0) {
        if (now_us < server->stop_deadline_us)
            return;
        if (!server->stop_term_sent) {
            for (guint i = 0; i < server->clients->len; i++) {
                Client *client = g_ptr_array_index(server->clients, i);

                if (client->kind != CLIENT_VIEW)
                    continue;
                if (peer_pidfd_exited(client)) {
                    client->closing = TRUE;
                    continue;
                }
                if (!client->stop_term_sent &&
                    signal_stopping_view(server, client, SIGTERM))
                    client->stop_term_sent = TRUE;
                if (client->stop_abandon)
                    client->closing = TRUE;
            }
            server->stop_term_sent = TRUE;
            server->stop_deadline_us = now_us +
                ((gint64)STOP_CLIENT_TERM_GRACE_MS * 1000);
            return;
        }

        if (!server->stop_kill_sent) {
            for (guint i = 0; i < server->clients->len; i++) {
                Client *client = g_ptr_array_index(server->clients, i);

                if (client->kind != CLIENT_VIEW)
                    continue;
                if (peer_pidfd_exited(client)) {
                    client->closing = TRUE;
                    continue;
                }
                if (!client->stop_kill_sent &&
                    signal_stopping_view(server, client, SIGKILL))
                    client->stop_kill_sent = TRUE;
                client->closing = TRUE;
            }
            server->stop_kill_sent = TRUE;
            server->stop_deadline_us = now_us +
                ((gint64)STOP_CLIENT_KILL_GRACE_MS * 1000);
            return;
        }

        for (guint i = 0; i < server->clients->len; i++) {
            Client *client = g_ptr_array_index(server->clients, i);

            if (client->kind != CLIENT_VIEW || peer_pidfd_exited(client))
                continue;
            server->stop_cleanup_failed = TRUE;
            client->stop_abandon = TRUE;
            client->closing = TRUE;
            g_warning("tracked pane %ld did not terminate within the stop deadline",
                      (long)client->peer_pid);
        }
        return;
    }

    if (!server->stop_engines_requested) {
        request_engine_shutdown(server, now_us);
        if (engine_count(server) == 0)
            server->running = FALSE;
        return;
    }
    if (engine_count(server) == 0) {
        server->running = FALSE;
        return;
    }
    if (now_us < server->stop_deadline_us)
        return;

    if (!server->stop_engine_term_sent) {
        for (guint i = 0; i < server->engines->len; i++) {
            EngineRegistration *engine =
                g_ptr_array_index(server->engines, i);

            if (!engine->stop_term_sent &&
                signal_stopping_engine(server, engine, SIGTERM))
                engine->stop_term_sent = TRUE;
        }
        server->stop_engine_term_sent = TRUE;
        server->stop_deadline_us = now_us +
            ((gint64)STOP_ENGINE_TERM_GRACE_MS * 1000);
        return;
    }

    if (!server->stop_engine_kill_sent) {
        for (guint i = 0; i < server->engines->len; i++) {
            EngineRegistration *engine =
                g_ptr_array_index(server->engines, i);

            if (!engine->stop_kill_sent &&
                signal_stopping_engine(server, engine, SIGKILL))
                engine->stop_kill_sent = TRUE;
        }
        server->stop_engine_kill_sent = TRUE;
        server->stop_deadline_us = now_us +
            ((gint64)STOP_ENGINE_KILL_GRACE_MS * 1000);
        return;
    }

    for (guint i = 0; i < server->engines->len; i++) {
        EngineRegistration *engine =
            g_ptr_array_index(server->engines, i);

        if (engine_pidfd_exited(engine))
            continue;
        server->stop_cleanup_failed = TRUE;
        engine->stop_abandon = TRUE;
        g_warning("tracked engine %s pid %ld did not terminate within the stop deadline",
                  engine->profile,
                  (long)engine->peer_pid);
    }
    server->running = FALSE;
}

static void send_controlled_stop_response(Server *server)
{
    Client *control = server->stop_client;

    if (control == NULL || control->closing)
        return;
    if (server->stop_cleanup_failed)
        control_error(control, "daemon stopped with incomplete cleanup");
    else
        client_send_line(control, "OK");
    if (!client_flush_bounded(control, STOP_REPLY_FLUSH_MS))
        g_printerr("muxd: stop response could not be delivered\n");
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
    client->peer_pidfd = peer_pidfd_open(credentials.pid);
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
        if (errno != ENOENT)
            return FALSE;
        if (mkdir(directory, 0700) < 0 && errno != EEXIST)
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
    server.engines = g_ptr_array_new_with_free_func(
        engine_registration_free);

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
        int poll_error = result < 0 ? errno : 0;

        while (g_main_context_iteration(server.main_context, FALSE))
            ;
        if (result < 0 && !(poll_error == EINTR && stop_requested)) {
            g_printerr("muxd: poll failed: %s\n", g_strerror(poll_error));
            server.stop_cleanup_failed = TRUE;
            server.running = FALSE;
        }
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
        remove_exited_engines(&server);
        flush_session_if_due(&server);
        update_controlled_stop(&server);
    }

    if (server.session_dirty && !persist_session_now(&server))
        server.stop_cleanup_failed = TRUE;
    if (!pending_moves_shutdown(&server))
        server.stop_cleanup_failed = TRUE;
    muxd_clipboard_free(server.clipboard);
    g_main_context_unref(server.main_context);
    if (server.listener >= 0)
        close(server.listener);
    if (unlink(server.socket_path) < 0 && errno != ENOENT)
        server.stop_cleanup_failed = TRUE;
    if (server.lock_fd >= 0) {
        if (close(server.lock_fd) < 0)
            server.stop_cleanup_failed = TRUE;
        server.lock_fd = -1;
    }
    if (server.stopping)
        send_controlled_stop_response(&server);
    g_ptr_array_unref(server.clients);
    g_ptr_array_unref(server.engines);
    g_ptr_array_unref(server.pending_moves);
    g_free(server.socket_path);
    g_free(server.active_id);
    g_free(server.current_layer);
    g_free(server.session_path);
    mux_session_state_free(server.session);
    return server.stop_cleanup_failed ? EXIT_FAILURE : EXIT_SUCCESS;
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

static gboolean terminate_and_cleanup_ensure_child(pid_t pid)
{
    EnsureOwnedSocket owned = { 0 };

    ensure_owned_socket_capture(&owned, pid);
    if (!terminate_and_reap_ensure_child(pid)) {
        g_printerr("muxd: failed to terminate startup child %ld\n", (long)pid);
        g_clear_pointer(&owned.path, g_free);
        return FALSE;
    }
    ensure_owned_socket_cleanup(&owned);
    return TRUE;
}

static void close_inherited_daemon_fds(void)
{
    long maximum_fd = sysconf(_SC_OPEN_MAX);

    if (maximum_fd < 0)
        maximum_fd = 1024;
    for (long fd = STDERR_FILENO + 1; fd < maximum_fd; fd++)
        close((int)fd);
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
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd < 0 ||
            dup2(null_fd, STDIN_FILENO) < 0 ||
            dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0) {
            if (null_fd > STDERR_FILENO)
                close(null_fd);
            _exit(EXIT_FAILURE);
        }
        if (null_fd > STDERR_FILENO)
            close(null_fd);
        close_inherited_daemon_fds();
        _exit(run_server());
    }

    gint64 startup_deadline_us =
        g_get_monotonic_time() +
        ((gint64)ENSURE_STARTUP_TIMEOUT_MS * 1000);
    gboolean child_reaped = FALSE;

    while (g_get_monotonic_time() < startup_deadline_us) {
        int status;
        pid_t wait_result;

        if (daemon_alive_with_timeout(ENSURE_PING_TIMEOUT_MS))
            return EXIT_SUCCESS;
        if (!child_reaped) {
            wait_result = waitpid(pid, &status, WNOHANG);
            if (wait_result == pid) {
                child_reaped = TRUE;
            } else if (wait_result < 0 && errno != EINTR) {
                if (errno == ECHILD) {
                    child_reaped = TRUE;
                } else {
                    int saved_errno = errno;

                    (void)terminate_and_cleanup_ensure_child(pid);
                    g_printerr("muxd: cannot monitor daemon startup: %s\n",
                               g_strerror(saved_errno));
                    return EXIT_FAILURE;
                }
            }
        }
        g_usleep(ENSURE_STARTUP_POLL_US);
    }

    if (!child_reaped)
        (void)terminate_and_cleanup_ensure_child(pid);
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
