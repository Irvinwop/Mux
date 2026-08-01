#define _GNU_SOURCE

#include "mux-protocol.h"
#include "muxd-clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
    GString *input;

    gchar *id;
    gchar *kitty_window;
    gchar *kitty_socket;
    gchar *layer;
    gchar *uri;
    gchar *title;
    long pid;
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
    gboolean running;
    GMainContext *main_context;
    MuxdClipboard *clipboard;
} Server;

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

static void client_free(gpointer data)
{
    Client *client = data;
    if (client->fd >= 0)
        close(client->fd);
    if (client->input)
        g_string_free(client->input, TRUE);
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
            g_strcmp0(client->layer, layer) == 0) {
            return client;
        }
    }
    return NULL;
}

static gboolean kitty_focus(Client *client)
{
    if (!client || !client->kitty_window || !*client->kitty_window ||
        !client->kitty_socket || !*client->kitty_socket) {
        return FALSE;
    }

    pid_t pid = fork();
    if (pid < 0)
        return FALSE;
    if (pid == 0) {
        gchar *match = g_strdup_printf("id:%s", client->kitty_window);
        execlp(
            "kitten",
            "kitten",
            "@",
            "--to",
            client->kitty_socket,
            "focus-window",
            "--match",
            match,
            NULL);
        _exit(127);
    }
    return TRUE;
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

static void broadcast_line(Server *server, const gchar *line)
{
    for (guint i = 0; i < server->clients->len; i++) {
        Client *client = g_ptr_array_index(server->clients, i);
        if ((client->kind == CLIENT_SUBSCRIBER ||
             client->kind == CLIENT_BAR) &&
            !mux_send_line(client->fd, "%s", line)) {
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
    if (!view || view->kind != CLIENT_VIEW)
        return;

    if (g_strcmp0(server->current_layer, view->layer) != 0) {
        replace_string(&server->current_layer, g_strdup(view->layer));
        broadcast_layer(server);
    }

    for (guint i = 0; i < server->clients->len; i++) {
        Client *candidate = g_ptr_array_index(server->clients, i);
        if (candidate->kind != CLIENT_VIEW)
            continue;
        gboolean focused = candidate == view;
        if (candidate->focused != focused) {
            candidate->focused = focused;
            broadcast_view(server, candidate, "UPSERT");
        }
    }

    if (g_strcmp0(server->active_id, view->id) != 0) {
        replace_string(&server->active_id, g_strdup(view->id));
        broadcast_active(server);
    }
}

static void send_snapshot(Server *server, Client *client)
{
    gchar *active = mux_encode(server->active_id);
    gchar *layer = mux_encode(server->current_layer);
    mux_send_line(
        client->fd,
        "BEGIN\t%" G_GUINT64_FORMAT "\t%s\t%s",
        server->revision,
        active,
        layer);
    g_free(layer);
    g_free(active);

    for (guint i = 0; i < server->clients->len; i++) {
        Client *view = g_ptr_array_index(server->clients, i);
        if (view->kind != CLIENT_VIEW)
            continue;

        gchar *id = mux_encode(view->id);
        gchar *view_layer = mux_encode(view->layer);
        gchar *uri = mux_encode(view->uri);
        gchar *title = mux_encode(view->title);
        gchar *kitty = mux_encode(view->kitty_window);
        mux_send_line(
            client->fd,
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
    mux_send_line(client->fd, "END");
}

static void control_error(Client *client, const gchar *message)
{
    gchar *encoded = mux_encode(message);
    mux_send_line(client->fd, "ERR\t%s", encoded);
    g_free(encoded);
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

static void handle_control(
    Server *server,
    Client *client,
    gchar **fields,
    guint field_count)
{
    const gchar *command = field_count > 1 ? fields[1] : "";

    if (g_strcmp0(command, "PING") == 0) {
        mux_send_line(client->fd, "PONG\t%d", MUX_PROTOCOL_VERSION);
        client->closing = TRUE;
        return;
    }
    if (g_strcmp0(command, "LIST") == 0) {
        send_snapshot(server, client);
        client->closing = TRUE;
        return;
    }
    if (g_strcmp0(command, "STATUS") == 0) {
        gchar *active = mux_encode(server->active_id);
        gchar *layer = mux_encode(server->current_layer);
        mux_send_line(
            client->fd,
            "STATUS\t%" G_GUINT64_FORMAT "\t%s\t%s\t%u",
            server->revision,
            active,
            layer,
            view_count(server));
        g_free(layer);
        g_free(active);
        client->closing = TRUE;
        return;
    }
    if (g_strcmp0(command, "LAYER") == 0 && field_count >= 3) {
        gchar *layer = decode_layer(fields[2]);

        if (!layer) {
            control_error(client, "invalid layer identifier");
            return;
        }
        replace_string(&server->current_layer, layer);
        broadcast_layer(server);
        Client *view = find_layer_view(server, server->current_layer);
        if (view) {
            set_active(server, view);
            kitty_focus(view);
        }
        mux_send_line(client->fd, "OK");
        client->closing = TRUE;
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
        mux_send_line(client->fd, "OK");
        client->closing = TRUE;
        return;
    }
    if (g_strcmp0(command, "MOVE") == 0 && field_count >= 4) {
        Client *view = control_target(server, fields[2]);
        gchar *layer = decode_layer(fields[3]);

        if (!view) {
            g_free(layer);
            control_error(client, "view not found");
            return;
        }
        if (!layer) {
            control_error(client, "invalid layer identifier");
            return;
        }
        replace_string(&view->layer, layer);
        broadcast_view(server, view, "UPSERT");
        mux_send_line(client->fd, "OK");
        client->closing = TRUE;
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
        if (!mux_send_line(view->fd, "DO\t%s\t%s", command, argument)) {
            control_error(client, "view connection failed");
            view->closing = TRUE;
            return;
        }
        mux_send_line(client->fd, "OK");
        client->closing = TRUE;
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
        broadcast_view(server, client, "UPSERT");
        return;
    }
    if (g_strcmp0(fields[0], "PROMPT") == 0) {
        Client *bar = find_bar(server, client);
        if (bar) {
            mux_send_line(bar->fd, "DO\tEDIT\t");
            kitty_focus(bar);
        }
        return;
    }
    if (g_strcmp0(fields[0], "BYE") == 0) {
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
            mux_send_line(view->fd, "DO\tOPEN\t%s", fields[1]);
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
        client->kind = CLIENT_VIEW;
        client->id = mux_decode(fields[1]);
        client->pid = strtol(fields[2], NULL, 10);
        client->kitty_window = mux_decode(fields[3]);
        client->kitty_socket = mux_decode(fields[4]);
        client->layer = mux_decode(fields[5]);
        client->uri = mux_decode(fields[6]);
        client->title = g_strdup("");
        mux_send_line(client->fd, "OK\t%d", MUX_PROTOCOL_VERSION);
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
        mux_send_line(client->fd, "OK\t%d", MUX_PROTOCOL_VERSION);
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
    ssize_t count;
    do {
        count = recv(client->fd, buffer, sizeof(buffer), 0);
    } while (count < 0 && errno == EINTR);

    if (count <= 0) {
        client->closing = TRUE;
        return;
    }

    g_string_append_len(client->input, buffer, count);
    if (client->input->len > 1024 * 1024) {
        client->closing = TRUE;
        return;
    }

    while (!client->closing) {
        gchar *newline = memchr(client->input->str, '\n', client->input->len);
        if (!newline)
            break;
        gsize line_length = (gsize)(newline - client->input->str);
        gchar *line = g_strndup(client->input->str, line_length);
        g_strchomp(line);
        g_string_erase(client->input, 0, line_length + 1);
        handle_line(server, client, line);
        g_free(line);
    }
}

static void remove_closed_clients(Server *server)
{
    for (gint i = (gint)server->clients->len - 1; i >= 0; i--) {
        Client *client = g_ptr_array_index(server->clients, (guint)i);
        if (!client->closing)
            continue;

        if (client->kind == CLIENT_VIEW) {
            broadcast_view(server, client, "REMOVE");
            if (g_strcmp0(server->active_id, client->id) == 0) {
                g_clear_pointer(&server->active_id, g_free);
                for (guint j = 0; j < server->clients->len; j++) {
                    Client *candidate = g_ptr_array_index(server->clients, j);
                    if (candidate != client &&
                        candidate->kind == CLIENT_VIEW &&
                        !candidate->closing) {
                        server->active_id = g_strdup(candidate->id);
                        candidate->focused = TRUE;
                        break;
                    }
                }
                broadcast_active(server);
            }
        }
        g_ptr_array_remove_index(server->clients, (guint)i);
    }
}

static gboolean accept_client(Server *server)
{
    int fd = accept4(server->listener, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return FALSE;

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
    client->input = g_string_new(NULL);
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

static int run_server(void)
{
    Server server = {
        .listener = -1,
        .lock_fd = -1,
        .clients = g_ptr_array_new_with_free_func(client_free),
        .current_layer = g_strdup("main"),
        .running = TRUE,
        .main_context = g_main_context_new(),
    };

    server.listener = create_listener(&server.socket_path,
                                      &server.lock_fd);
    if (server.listener < 0) {
        g_printerr("muxd: cannot listen: %s\n", g_strerror(errno));
        g_ptr_array_unref(server.clients);
        g_free(server.current_layer);
        g_main_context_unref(server.main_context);
        return EXIT_FAILURE;
    }

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
        g_free(server.socket_path);
        g_free(server.current_layer);
        g_main_context_unref(server.main_context);
        return EXIT_FAILURE;
    }

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    while (server.running && !stop_requested) {
        guint client_count = server.clients->len;
        struct pollfd *poll_fds = g_new0(struct pollfd, client_count + 1);
        poll_fds[0].fd = server.listener;
        poll_fds[0].events = POLLIN;
        for (guint i = 0; i < client_count; i++) {
            Client *client = g_ptr_array_index(server.clients, i);
            poll_fds[i + 1].fd = client->fd;
            poll_fds[i + 1].events = POLLIN;
        }

        int result;
        do {
            result = poll(poll_fds, client_count + 1, 50);
        } while (result < 0 && errno == EINTR && !stop_requested);

        if (result > 0) {
            if (poll_fds[0].revents & POLLIN)
                accept_client(&server);
            for (guint i = 0; i < client_count; i++) {
                Client *client = g_ptr_array_index(server.clients, i);
                short events = poll_fds[i + 1].revents;
                if (events & POLLIN)
                    client_read(&server, client);
                if (events & (POLLHUP | POLLERR | POLLNVAL))
                    client->closing = TRUE;
            }
        }
        g_free(poll_fds);
        while (g_main_context_iteration(server.main_context, FALSE))
            ;
        remove_closed_clients(&server);
    }

    muxd_clipboard_free(server.clipboard);
    g_main_context_unref(server.main_context);
    close(server.listener);
    unlink(server.socket_path);
    close(server.lock_fd);
    g_ptr_array_unref(server.clients);
    g_free(server.socket_path);
    g_free(server.active_id);
    g_free(server.current_layer);
    return EXIT_SUCCESS;
}

static gboolean daemon_alive(void)
{
    int fd = mux_connect_socket();
    if (fd < 0)
        return FALSE;
    gboolean sent = mux_send_line(fd, "CTL\tPING");
    gchar *response = sent ? mux_read_line(fd, 500) : NULL;
    gboolean alive = response && g_str_has_prefix(response, "PONG\t");
    g_free(response);
    close(fd);
    return alive;
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

    for (guint attempt = 0; attempt < 200; attempt++) {
        if (daemon_alive())
            return EXIT_SUCCESS;
        g_usleep(10000);
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
