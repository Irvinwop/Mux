#define _GNU_SOURCE

#include "mux-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_TIMEOUT_MS 4000
#define CONNECT_TIMEOUT_MS 3000
#define RESPONSE_TIMEOUT_MS 1000
#define STATE_TIMEOUT_MS 2000

static gchar *muxd_executable;
static gchar *muxctl_executable;

static void set_errno_error(GError **error,
                            const gchar *operation,
                            int saved_errno)
{
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "%s: %s",
                operation,
                g_strerror(saved_errno));
}

static gint remaining_ms(gint64 deadline_us)
{
    gint64 remaining_us = deadline_us - g_get_monotonic_time();

    if (remaining_us <= 0)
        return 0;
    return (gint)MIN((remaining_us + 999) / 1000, (gint64)G_MAXINT);
}

static void reap_forcefully(pid_t pid)
{
    int status;

    if (pid <= 0)
        return;
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
        return;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}

static gboolean wait_child_success(pid_t pid,
                                   guint timeout_ms,
                                   GError **error)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);
    int status = 0;

    while (remaining_ms(deadline_us) > 0) {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return TRUE;
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "child %ld exited unsuccessfully (status=%d)",
                        (long)pid,
                        status);
            return FALSE;
        }
        if (result < 0 && errno != EINTR) {
            set_errno_error(error, "waitpid", errno);
            return FALSE;
        }
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }

    reap_forcefully(pid);
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "child %ld did not exit within %u ms",
                (long)pid,
                timeout_ms);
    return FALSE;
}

static gboolean reap_child_signal_now(pid_t pid,
                                      int expected_signal,
                                      GError **error)
{
    int status = 0;
    pid_t result;

    do {
        result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_BUSY,
                    "child %ld was still running when stop completed",
                    (long)pid);
        return FALSE;
    }
    if (result < 0) {
        set_errno_error(error, "waitpid", errno);
        return FALSE;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != expected_signal) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "child %ld terminated unexpectedly (status=%d)",
                    (long)pid,
                    status);
        return FALSE;
    }
    return TRUE;
}

static gboolean reap_child_exit_now(pid_t pid,
                                    int expected_status,
                                    GError **error)
{
    int status = 0;
    pid_t result;

    do {
        result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_BUSY,
                    "child %ld was still running when stop completed",
                    (long)pid);
        return FALSE;
    }
    if (result < 0) {
        set_errno_error(error, "waitpid", errno);
        return FALSE;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_status) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "child %ld exited unexpectedly (status=%d)",
                    (long)pid,
                    status);
        return FALSE;
    }
    return TRUE;
}

static void child_redirect_to_null(void)
{
    int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

    if (null_fd < 0)
        return;
    (void)dup2(null_fd, STDIN_FILENO);
    (void)dup2(null_fd, STDOUT_FILENO);
    (void)dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO)
        close(null_fd);
}

static gboolean run_command(const gchar *executable,
                            const gchar *argument,
                            guint timeout_ms,
                            GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork", errno);
        return FALSE;
    }
    if (pid == 0) {
        child_redirect_to_null();
        execl(executable, executable, argument, (char *)NULL);
        _exit(127);
    }
    return wait_child_success(pid, timeout_ms, error);
}

static gboolean run_command_with_extra_argument(const gchar *executable,
                                                const gchar *argument,
                                                const gchar *extra,
                                                guint timeout_ms,
                                                GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork", errno);
        return FALSE;
    }
    if (pid == 0) {
        child_redirect_to_null();
        execl(executable, executable, argument, extra, (char *)NULL);
        _exit(127);
    }
    return wait_child_success(pid, timeout_ms, error);
}

static gchar *run_command_capture(const gchar *executable,
                                  const gchar *argument,
                                  guint timeout_ms,
                                  GError **error)
{
    int output_pipe[2];
    pid_t pid;
    GString *output;
    gint64 deadline_us;
    gboolean exited = FALSE;
    gboolean eof = FALSE;
    int status = 0;

    if (pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        set_errno_error(error, "pipe2", errno);
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        int saved_errno = errno;

        close(output_pipe[0]);
        close(output_pipe[1]);
        set_errno_error(error, "fork", saved_errno);
        return NULL;
    }
    if (pid == 0) {
        int null_fd;

        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(output_pipe[1]);
        null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execl(executable, executable, argument, (char *)NULL);
        _exit(127);
    }

    close(output_pipe[1]);
    output = g_string_new(NULL);
    deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while ((!exited || !eof) && remaining_ms(deadline_us) > 0) {
        gchar buffer[1024];

        while (!eof) {
            ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));

            if (count > 0) {
                g_string_append_len(output, buffer, count);
                if (output->len > 64 * 1024) {
                    close(output_pipe[0]);
                    reap_forcefully(pid);
                    g_string_free(output, TRUE);
                    g_set_error_literal(error,
                                        G_IO_ERROR,
                                        G_IO_ERROR_MESSAGE_TOO_LARGE,
                                        "child output exceeded 64 KiB");
                    return NULL;
                }
                continue;
            }
            if (count == 0) {
                eof = TRUE;
                break;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            int saved_errno = errno;

            close(output_pipe[0]);
            reap_forcefully(pid);
            g_string_free(output, TRUE);
            set_errno_error(error, "read child output", saved_errno);
            return NULL;
        }

        if (!exited) {
            pid_t result = waitpid(pid, &status, WNOHANG);

            if (result == pid)
                exited = TRUE;
            else if (result < 0 && errno != EINTR) {
                int saved_errno = errno;

                close(output_pipe[0]);
                reap_forcefully(pid);
                g_string_free(output, TRUE);
                set_errno_error(error, "waitpid", saved_errno);
                return NULL;
            }
        }

        if (!exited || !eof) {
            struct pollfd poll_fd = {
                .fd = output_pipe[0],
                .events = POLLIN | POLLHUP,
            };

            (void)poll(&poll_fd,
                       1,
                       MIN(remaining_ms(deadline_us), 20));
        }
    }

    close(output_pipe[0]);
    if (!exited || !eof) {
        reap_forcefully(pid);
        g_string_free(output, TRUE);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_TIMED_OUT,
                    "child %ld did not complete within %u ms",
                    (long)pid,
                    timeout_ms);
        return NULL;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        g_string_free(output, TRUE);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "child %ld exited unsuccessfully (status=%d)",
                    (long)pid,
                    status);
        return NULL;
    }
    return g_string_free(output, FALSE);
}

static int connect_unix_socket(const gchar *path,
                               guint timeout_ms,
                               GError **error)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FILENAME_TOO_LONG,
                            "Unix socket path is too long");
        return -1;
    }

    while (remaining_ms(deadline_us) > 0) {
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        int fd = socket(AF_UNIX,
                        SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                        0);
        int connect_error = 0;

        if (fd < 0) {
            set_errno_error(error, "socket", errno);
            return -1;
        }
        g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

        if (connect(fd,
                    (const struct sockaddr *)&address,
                    sizeof(address)) < 0) {
            connect_error = errno;
            if (connect_error == EINPROGRESS) {
                struct pollfd poll_fd = {
                    .fd = fd,
                    .events = POLLOUT,
                };
                socklen_t error_size = sizeof(connect_error);
                int poll_result;

                do {
                    poll_result = poll(&poll_fd,
                                       1,
                                       remaining_ms(deadline_us));
                } while (poll_result < 0 && errno == EINTR);

                if (poll_result > 0 &&
                    getsockopt(fd,
                               SOL_SOCKET,
                               SO_ERROR,
                               &connect_error,
                               &error_size) == 0 &&
                    connect_error == 0) {
                    int flags = fcntl(fd, F_GETFL);

                    if (flags >= 0)
                        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                    return fd;
                }
                if (poll_result == 0)
                    connect_error = ETIMEDOUT;
                else if (poll_result < 0)
                    connect_error = errno;
            }
        } else {
            int flags = fcntl(fd, F_GETFL);

            if (flags >= 0)
                (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
            return fd;
        }

        close(fd);
        if (connect_error != ENOENT && connect_error != ECONNREFUSED) {
            set_errno_error(error, "connect", connect_error);
            return -1;
        }
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "timed out connecting to %s",
                path);
    return -1;
}

static gboolean get_peer_credentials(int fd,
                                     struct ucred *credentials,
                                     GError **error)
{
    socklen_t credentials_size = sizeof(*credentials);

    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   credentials,
                   &credentials_size) < 0) {
        set_errno_error(error, "getsockopt(SO_PEERCRED)", errno);
        return FALSE;
    }
    if (credentials_size != sizeof(*credentials)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "SO_PEERCRED returned an unexpected size");
        return FALSE;
    }
    return TRUE;
}

static gboolean persistent_view_id_is_valid(const gchar *id)
{
    const gchar *number;
    gchar *end = NULL;

    if (id == NULL || !g_str_has_prefix(id, "view-"))
        return FALSE;
    number = id + strlen("view-");
    if (*number < '1' || *number > '9')
        return FALSE;
    for (const gchar *cursor = number; *cursor != '\0'; cursor++) {
        if (!g_ascii_isdigit(*cursor))
            return FALSE;
    }
    errno = 0;
    (void)g_ascii_strtoull(number, &end, 10);
    return errno == 0 && end != number && *end == '\0';
}

static gboolean register_view_with_identity(int fd,
                                            const gchar *proposed_id,
                                            const gchar *window_id,
                                            const gchar *kitty_socket,
                                            const gchar *layer,
                                            gchar **assigned_id,
                                            GError **error)
{
    g_autofree gchar *encoded_id =
        mux_encode(proposed_id != NULL ? proposed_id : "");
    g_autofree gchar *encoded_window = mux_encode(window_id);
    g_autofree gchar *encoded_socket = mux_encode(kitty_socket);
    g_autofree gchar *encoded_layer = mux_encode(layer);
    g_autofree gchar *encoded_uri =
        mux_encode("https://example.invalid/muxd-integration");
    g_autofree gchar *line = NULL;
    g_auto(GStrv) fields = NULL;
    guint field_count;

    *assigned_id = NULL;
    if (!mux_send_line(fd,
                       "VIEW\t%s\t%ld\t%s\t%s\t%s\t%s",
                       encoded_id,
                       (long)getpid(),
                       encoded_window,
                       encoded_socket,
                       encoded_layer,
                       encoded_uri)) {
        set_errno_error(error, "send VIEW registration", errno);
        return FALSE;
    }

    line = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
    if (line == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_TIMED_OUT,
                            "timed out waiting for VIEW response");
        return FALSE;
    }
    fields = g_strsplit(line, "\t", -1);
    field_count = g_strv_length(fields);
    if (field_count < 2 || g_strcmp0(fields[0], "OK") != 0 ||
        g_ascii_strtoll(fields[1], NULL, 10) != MUX_PROTOCOL_VERSION) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "unexpected VIEW response: %s",
                    line);
        return FALSE;
    }
    if (field_count >= 3 && fields[2][0] != '\0')
        *assigned_id = mux_decode(fields[2]);
    return TRUE;
}

static gboolean register_view(int fd,
                              const gchar *proposed_id,
                              gchar **assigned_id,
                              GError **error)
{
    return register_view_with_identity(fd,
                                       proposed_id,
                                       "integration-window",
                                       "integration-kitty",
                                       "main",
                                       assigned_id,
                                       error);
}

static gboolean query_view_count(const gchar *socket_path,
                                 guint *view_count,
                                 GError **error)
{
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);
    g_autofree gchar *line = NULL;
    g_auto(GStrv) fields = NULL;
    gchar *end = NULL;
    guint64 parsed_count;

    if (fd < 0)
        return FALSE;
    if (!mux_send_line(fd, "CTL\tSTATUS")) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "send STATUS", saved_errno);
        return FALSE;
    }
    line = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
    close(fd);
    if (line == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_TIMED_OUT,
                            "timed out waiting for STATUS response");
        return FALSE;
    }

    fields = g_strsplit(line, "\t", -1);
    if (g_strv_length(fields) < 5 ||
        g_strcmp0(fields[0], "STATUS") != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "unexpected STATUS response: %s",
                    line);
        return FALSE;
    }
    errno = 0;
    parsed_count = g_ascii_strtoull(fields[4], &end, 10);
    if (errno != 0 || end == fields[4] || *end != '\0' ||
        parsed_count > G_MAXUINT) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "invalid STATUS view count: %s",
                    fields[4]);
        return FALSE;
    }
    *view_count = (guint)parsed_count;
    return TRUE;
}

static gchar *query_view_layer(const gchar *socket_path,
                               const gchar *view_id,
                               GError **error)
{
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);
    gchar *layer = NULL;

    if (fd < 0)
        return NULL;
    if (!mux_send_line(fd, "CTL\tLIST")) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "send LIST", saved_errno);
        return NULL;
    }

    for (guint i = 0; i < 256; i++) {
        g_autofree gchar *line = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
        g_auto(GStrv) fields = NULL;

        if (line == NULL) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "timed out reading LIST response");
            break;
        }
        if (g_strcmp0(line, "END") == 0)
            break;
        fields = g_strsplit(line, "\t", -1);
        if (g_strv_length(fields) >= 3 &&
            g_strcmp0(fields[0], "VIEW") == 0) {
            g_autofree gchar *candidate_id = mux_decode(fields[1]);

            if (g_strcmp0(candidate_id, view_id) == 0) {
                g_free(layer);
                layer = mux_decode(fields[2]);
            }
        }
    }
    close(fd);
    if (layer == NULL && error != NULL && *error == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    "view %s was absent from LIST response",
                    view_id);
    }
    return layer;
}

static gboolean query_active_state(const gchar *socket_path,
                                   gchar **active_id,
                                   gchar **current_layer,
                                   GError **error)
{
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);
    g_autofree gchar *line = NULL;
    g_auto(GStrv) fields = NULL;

    *active_id = NULL;
    *current_layer = NULL;
    if (fd < 0)
        return FALSE;
    if (!mux_send_line(fd, "CTL\tSTATUS")) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "send STATUS", saved_errno);
        return FALSE;
    }
    line = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
    close(fd);
    if (line == NULL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_TIMED_OUT,
                            "timed out waiting for STATUS response");
        return FALSE;
    }
    fields = g_strsplit(line, "\t", -1);
    if (g_strv_length(fields) < 5 ||
        g_strcmp0(fields[0], "STATUS") != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "unexpected STATUS response: %s",
                    line);
        return FALSE;
    }
    *active_id = mux_decode(fields[2]);
    *current_layer = mux_decode(fields[3]);
    return TRUE;
}

static gboolean wait_for_active_state(const gchar *socket_path,
                                      const gchar *expected_active,
                                      const gchar *expected_layer,
                                      guint timeout_ms,
                                      GError **error)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while (remaining_ms(deadline_us) > 0) {
        g_autofree gchar *active = NULL;
        g_autofree gchar *layer = NULL;
        g_autoptr(GError) local_error = NULL;

        if (query_active_state(socket_path,
                               &active,
                               &layer,
                               &local_error) &&
            g_strcmp0(active, expected_active) == 0 &&
            g_strcmp0(layer, expected_layer) == 0)
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "active state did not become active=%s layer=%s",
                expected_active,
                expected_layer);
    return FALSE;
}

static gboolean set_current_layer(const gchar *socket_path,
                                  const gchar *layer,
                                  GError **error)
{
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);
    g_autofree gchar *encoded_layer = mux_encode(layer);
    g_autofree gchar *response = NULL;

    if (fd < 0)
        return FALSE;
    if (!mux_send_line(fd, "CTL\tLAYER\t%s", encoded_layer)) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "send LAYER", saved_errno);
        return FALSE;
    }
    response = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
    close(fd);
    if (g_strcmp0(response, "OK") != 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "unexpected LAYER response: %s",
                    response != NULL ? response : "(none)");
        return FALSE;
    }
    return TRUE;
}

static int request_move(const gchar *socket_path,
                        const gchar *view_id,
                        const gchar *layer,
                        GError **error)
{
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);
    g_autofree gchar *encoded_id = mux_encode(view_id);
    g_autofree gchar *encoded_layer = mux_encode(layer);

    if (fd < 0)
        return -1;
    if (!mux_send_line(fd,
                       "CTL\tMOVE\t%s\t%s",
                       encoded_id,
                       encoded_layer)) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "send MOVE", saved_errno);
        return -1;
    }
    return fd;
}

static gboolean wait_for_path(const gchar *path, guint timeout_ms)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while (remaining_ms(deadline_us) > 0) {
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

static gboolean wait_for_path_absent(const gchar *path, guint timeout_ms)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while (remaining_ms(deadline_us) > 0) {
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }
    return !g_file_test(path, G_FILE_TEST_EXISTS);
}

static gboolean pid_is_terminated(pid_t pid)
{
    g_autofree gchar *path =
        g_strdup_printf("/proc/%ld/stat", (long)pid);
    g_autofree gchar *contents = NULL;
    gchar *command_end;

    errno = 0;
    if (kill(pid, 0) < 0)
        return errno == ESRCH;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;

    command_end = strrchr(contents, ')');
    return command_end != NULL &&
        command_end[1] == ' ' &&
        command_end[2] == 'Z' &&
        (command_end[3] == ' ' || command_end[3] == '\0');
}

static gboolean wait_for_pid_terminated(pid_t pid, guint timeout_ms)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while (remaining_ms(deadline_us) > 0) {
        if (pid_is_terminated(pid))
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }
    return pid_is_terminated(pid);
}

static gboolean publish_marker(const gchar *path, const gchar *contents)
{
    GError *error = NULL;
    gboolean result = g_file_set_contents(path, contents, -1, &error);

    g_clear_error(&error);
    return result;
}

static pid_t spawn_stoppable_view(const gchar *socket_path,
                                  const gchar *proposed_id,
                                  const gchar *ready_path,
                                  GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork stoppable view", errno);
        return -1;
    }
    if (pid == 0) {
        GError *child_error = NULL;
        gchar *assigned_id = NULL;
        int fd = connect_unix_socket(socket_path,
                                     CONNECT_TIMEOUT_MS,
                                     &child_error);

        if (fd < 0 ||
            !register_view(fd, proposed_id, &assigned_id, &child_error) ||
            assigned_id == NULL ||
            !publish_marker(ready_path, assigned_id))
            _exit(2);
        for (;;) {
            gchar *line = mux_read_line(fd, -1);

            if (line == NULL)
                _exit(3);
            if (g_strcmp0(line, "DO\tQUIT\t") == 0) {
                g_free(line);
                if (!mux_send_line(fd, "BYE"))
                    _exit(4);
                close(fd);
                g_free(assigned_id);
                g_clear_error(&child_error);
                _exit(0);
            }
            g_free(line);
        }
    }
    return pid;
}

static pid_t spawn_transport_breaking_view(const gchar *socket_path,
                                           const gchar *ready_path,
                                           const gchar *broken_path,
                                           GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork transport-breaking view", errno);
        return -1;
    }
    if (pid == 0) {
        GError *child_error = NULL;
        gchar *assigned_id = NULL;
        int fd;

        signal(SIGTERM, SIG_IGN);
        fd = connect_unix_socket(socket_path,
                                 CONNECT_TIMEOUT_MS,
                                 &child_error);
        if (fd < 0 ||
            !register_view(fd, "", &assigned_id, &child_error) ||
            assigned_id == NULL ||
            !publish_marker(ready_path, assigned_id))
            _exit(2);
        for (;;) {
            gchar *line = mux_read_line(fd, -1);

            if (line == NULL)
                _exit(3);
            if (g_strcmp0(line, "DO\tQUIT\t") == 0) {
                g_free(line);
                close(fd);
                if (!publish_marker(broken_path, "broken"))
                    _exit(4);
                for (;;)
                    pause();
            }
            g_free(line);
        }
    }
    return pid;
}

static pid_t spawn_registered_engine(const gchar *socket_path,
                                     const gchar *profile,
                                     const gchar *ready_path,
                                     gboolean graceful,
                                     GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork registered engine", errno);
        return -1;
    }
    if (pid == 0) {
        GError *child_error = NULL;
        g_autofree gchar *encoded_profile = mux_encode(profile);
        int fd;

        if (!graceful)
            signal(SIGTERM, SIG_IGN);
        fd = connect_unix_socket(socket_path,
                                 CONNECT_TIMEOUT_MS,
                                 &child_error);
        if (fd < 0 ||
            !mux_send_line(fd,
                           "ENGINE\t1\t%s\t%ld",
                           encoded_profile,
                           (long)getpid()))
            _exit(2);
        {
            g_autofree gchar *response =
                mux_read_line(fd, RESPONSE_TIMEOUT_MS);

            if (g_strcmp0(response, "ENGINE_OK\t1") != 0)
                _exit(3);
        }
        if (!publish_marker(ready_path, "ready"))
            _exit(4);

        for (;;) {
            g_autofree gchar *line = mux_read_line(fd, -1);

            if (line == NULL) {
                if (graceful)
                    _exit(5);
                for (;;)
                    pause();
            }
            if (g_strcmp0(line, "ENGINE_STOP") != 0)
                continue;
            if (!graceful)
                continue;
            if (!mux_send_line(fd, "ENGINE_BYE"))
                _exit(6);
            close(fd);
            _exit(0);
        }
    }
    return pid;
}

static gboolean engine_registration_denied(const gchar *socket_path,
                                           const gchar *profile,
                                           pid_t claimed_pid,
                                           GError **error)
{
    g_autofree gchar *encoded_profile = mux_encode(profile);
    g_autofree gchar *response = NULL;
    int fd = connect_unix_socket(socket_path,
                                 RESPONSE_TIMEOUT_MS,
                                 error);

    if (fd < 0)
        return FALSE;
    if (!mux_send_line(fd,
                       "ENGINE\t1\t%s\t%ld",
                       encoded_profile,
                       (long)claimed_pid)) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error,
                        "send rejected engine registration",
                        saved_errno);
        return FALSE;
    }
    response = mux_read_line(fd, RESPONSE_TIMEOUT_MS);
    close(fd);
    if (response && g_str_has_prefix(response, "ERR\t"))
        return TRUE;
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "engine registration was not denied: %s",
                response ? response : "(connection closed)");
    return FALSE;
}

static pid_t spawn_stoppable_bar(const gchar *socket_path,
                                 const gchar *ready_path,
                                 const gchar *closed_path,
                                 GError **error)
{
    pid_t pid = fork();

    if (pid < 0) {
        set_errno_error(error, "fork stoppable bar", errno);
        return -1;
    }
    if (pid == 0) {
        GError *child_error = NULL;
        int fd = connect_unix_socket(socket_path,
                                     CONNECT_TIMEOUT_MS,
                                     &child_error);
        gchar *id = mux_encode("stop-integration-bar");
        gchar *window = mux_encode("902");
        gchar *kitty_socket = mux_encode("integration-kitty");
        gchar *layer = mux_encode("main");
        gboolean complete = FALSE;

        if (fd < 0 ||
            !mux_send_line(fd,
                           "BAR\t%s\t%ld\t%s\t%s\t%s",
                           id,
                           (long)getpid(),
                           window,
                           kitty_socket,
                           layer))
            _exit(2);
        g_free(layer);
        g_free(kitty_socket);
        g_free(window);
        g_free(id);

        for (guint i = 0; i < 256; i++) {
            gchar *line = mux_read_line(fd, RESPONSE_TIMEOUT_MS);

            if (line == NULL)
                _exit(3);
            complete = g_strcmp0(line, "END") == 0;
            g_free(line);
            if (complete)
                break;
        }
        if (!complete || !publish_marker(ready_path, "ready"))
            _exit(4);
        while (TRUE) {
            gchar *line = mux_read_line(fd, -1);

            if (line == NULL)
                break;
            g_free(line);
        }
        close(fd);
        if (!publish_marker(closed_path, "closed"))
            _exit(5);
        g_clear_error(&child_error);
        _exit(0);
    }
    return pid;
}

static pid_t spawn_silent_muxd(const gchar *socket_path,
                               guint hold_ms,
                               GError **error)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int listener;
    pid_t pid;

    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        set_errno_error(error, "create fake muxd socket", errno);
        return -1;
    }
    g_strlcpy(address.sun_path, socket_path, sizeof(address.sun_path));
    (void)g_unlink(socket_path);
    if (bind(listener,
             (const struct sockaddr *)&address,
             sizeof(address)) < 0 ||
        listen(listener, 1) < 0 || g_chmod(socket_path, 0600) < 0) {
        int saved_errno = errno;

        close(listener);
        (void)g_unlink(socket_path);
        set_errno_error(error, "start fake muxd", saved_errno);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        int saved_errno = errno;

        close(listener);
        (void)g_unlink(socket_path);
        set_errno_error(error, "fork fake muxd", saved_errno);
        return -1;
    }
    if (pid == 0) {
        gchar request[256];
        int client;

        do {
            client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        } while (client < 0 && errno == EINTR);
        if (client < 0)
            _exit(1);
        do {
            errno = 0;
        } while (recv(client, request, sizeof(request), 0) < 0 &&
                 errno == EINTR);
        if (hold_ms > 0)
            (void)poll(NULL, 0, (gint)hold_ms);
        close(client);
        close(listener);
        _exit(0);
    }

    close(listener);
    return pid;
}

static gboolean wait_for_view_count(const gchar *socket_path,
                                    guint expected_count,
                                    guint timeout_ms,
                                    GError **error)
{
    gint64 deadline_us =
        g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    while (remaining_ms(deadline_us) > 0) {
        g_autoptr(GError) local_error = NULL;
        guint actual_count = G_MAXUINT;

        if (query_view_count(socket_path,
                             &actual_count,
                             &local_error) &&
            actual_count == expected_count)
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 10));
    }

    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "view count did not become %u within %u ms",
                expected_count,
                timeout_ms);
    return FALSE;
}

static gboolean stop_identified_daemon(const gchar *socket_path,
                                       pid_t expected_pid,
                                       GError **error)
{
    struct ucred credentials;
    int verification_fd = connect_unix_socket(socket_path, 250, error);
    gint64 deadline_us;

    if (verification_fd < 0)
        return FALSE;
    if (!get_peer_credentials(verification_fd, &credentials, error)) {
        close(verification_fd);
        return FALSE;
    }
    close(verification_fd);
    if (credentials.pid != expected_pid || credentials.uid != geteuid()) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "refusing to terminate unverified peer pid %ld uid %u",
                    (long)credentials.pid,
                    (guint)credentials.uid);
        return FALSE;
    }
    if (kill(expected_pid, SIGTERM) < 0) {
        set_errno_error(error, "kill daemon", errno);
        return FALSE;
    }

    deadline_us =
        g_get_monotonic_time() + ((gint64)CONNECT_TIMEOUT_MS * 1000);
    while (remaining_ms(deadline_us) > 0) {
        struct stat status;

        if (lstat(socket_path, &status) < 0 && errno == ENOENT)
            return TRUE;
        (void)poll(NULL, 0, MIN(remaining_ms(deadline_us), 20));
    }
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_TIMED_OUT,
                        "identified muxd did not remove its socket");
    return FALSE;
}

static void remove_tree(const gchar *path)
{
    struct stat status;

    if (path == NULL || lstat(path, &status) < 0)
        return;
    if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode)) {
        GDir *directory = g_dir_open(path, 0, NULL);

        if (directory != NULL) {
            const gchar *name;

            while ((name = g_dir_read_name(directory)) != NULL) {
                g_autofree gchar *child =
                    g_build_filename(path, name, NULL);
                remove_tree(child);
            }
            g_dir_close(directory);
        }
        (void)g_rmdir(path);
    } else {
        (void)g_unlink(path);
    }
}

static void restore_environment(const gchar *name, const gchar *value)
{
    if (value != NULL)
        g_setenv(name, value, TRUE);
    else
        g_unsetenv(name);
}

#define REQUIRE(condition, ...)                                              \
    G_STMT_START {                                                          \
        if (!(condition)) {                                                 \
            g_test_message(__VA_ARGS__);                                    \
            g_test_fail();                                                  \
            goto cleanup;                                                   \
        }                                                                   \
    } G_STMT_END

#define REQUIRE_CALL(expression, description)                               \
    G_STMT_START {                                                          \
        if (!(expression)) {                                                \
            g_test_message("%s: %s",                                      \
                           description,                                     \
                           error != NULL ? error->message : "unknown error"); \
            g_clear_error(&error);                                          \
            g_test_fail();                                                  \
            goto cleanup;                                                   \
        }                                                                   \
    } G_STMT_END

static void test_muxd_identity_lifecycle(void)
{
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *state_dir = NULL;
    gchar *socket_path = NULL;
    gchar *first_id = NULL;
    gchar *reclaimed_id = NULL;
    gchar *post_bye_id = NULL;
    gchar *muxctl_output = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *old_state = g_strdup(g_getenv("XDG_STATE_HOME"));
    gchar *old_ephemeral = g_strdup(g_getenv("MUX_EPHEMERAL"));
    GError *error = NULL;
    struct ucred daemon_credentials;
    struct ucred view_credentials;
    struct stat socket_status;
    int guard_fd = -1;
    int view_fd = -1;
    pid_t daemon_pid = -1;
    mode_t old_umask = umask(0077);

    REQUIRE(g_file_test(muxd_executable, G_FILE_TEST_IS_EXECUTABLE),
            "muxd is not executable: %s",
            muxd_executable);
    REQUIRE(g_file_test(muxctl_executable, G_FILE_TEST_IS_EXECUTABLE),
            "muxctl is not executable: %s",
            muxctl_executable);

    root = g_dir_make_tmp("muxd-integration-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create temporary root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    state_dir = g_build_filename(root, "state", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(state_dir, 0700) == 0,
            "create state directory: %s",
            g_strerror(errno));
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set XDG_RUNTIME_DIR");
    REQUIRE(g_setenv("XDG_STATE_HOME", state_dir, TRUE),
            "set XDG_STATE_HOME");
    REQUIRE(g_setenv("MUX_EPHEMERAL", "0", TRUE),
            "set persistent peer identity");
    socket_path =
        g_build_filename(runtime_dir, "mux", "muxd.sock", NULL);

    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "start muxd");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect to muxd");
    REQUIRE_CALL(get_peer_credentials(guard_fd,
                                      &daemon_credentials,
                                      &error),
                 "identify muxd peer");
    REQUIRE(daemon_credentials.uid == geteuid() &&
                daemon_credentials.pid > 1,
            "unexpected muxd credentials pid=%ld uid=%u",
            (long)daemon_credentials.pid,
            (guint)daemon_credentials.uid);
    daemon_pid = daemon_credentials.pid;

    REQUIRE(lstat(socket_path, &socket_status) == 0,
            "stat muxd socket: %s",
            g_strerror(errno));
    REQUIRE(S_ISSOCK(socket_status.st_mode),
            "muxd endpoint is not a Unix socket");
    REQUIRE(socket_status.st_uid == geteuid(),
            "muxd socket owner is %u, expected %u",
            (guint)socket_status.st_uid,
            (guint)geteuid());
    REQUIRE((socket_status.st_mode & 0777) == 0600,
            "muxd socket mode is %03o, expected 600",
            (guint)(socket_status.st_mode & 0777));
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     0,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "query initial status");

    muxctl_output = run_command_capture(muxctl_executable,
                                        "status",
                                        CHILD_TIMEOUT_MS,
                                        &error);
    REQUIRE_CALL(muxctl_output != NULL, "run muxctl status");
    REQUIRE(g_str_has_prefix(muxctl_output, "revision=") &&
                strstr(muxctl_output, "views=0") != NULL,
            "unexpected muxctl status output: %s",
            muxctl_output);
    g_clear_pointer(&muxctl_output, g_free);

    view_fd = connect_unix_socket(socket_path,
                                  RESPONSE_TIMEOUT_MS,
                                  &error);
    REQUIRE_CALL(view_fd >= 0, "connect first view");
    REQUIRE_CALL(get_peer_credentials(view_fd,
                                      &view_credentials,
                                      &error),
                 "identify first view peer");
    REQUIRE(view_credentials.pid == daemon_pid,
            "view connected to muxd pid %ld, expected %ld",
            (long)view_credentials.pid,
            (long)daemon_pid);
    REQUIRE_CALL(register_view(view_fd, "", &first_id, &error),
                 "register first view");
    REQUIRE(persistent_view_id_is_valid(first_id),
            "muxd did not assign a persistent view-N identity");
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     1,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for first view");

    close(view_fd);
    view_fd = -1;
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     0,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for abrupt disconnect");

    view_fd = connect_unix_socket(socket_path,
                                  RESPONSE_TIMEOUT_MS,
                                  &error);
    REQUIRE_CALL(view_fd >= 0, "connect reclaiming view");
    REQUIRE_CALL(register_view(view_fd,
                               first_id,
                               &reclaimed_id,
                               &error),
                 "reclaim disconnected view");
    REQUIRE(g_strcmp0(reclaimed_id, first_id) == 0,
            "abrupt reconnect assigned %s instead of %s",
            reclaimed_id != NULL ? reclaimed_id : "(none)",
            first_id);
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     1,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for reclaimed view");

    REQUIRE(mux_send_line(view_fd, "BYE"),
            "send BYE: %s",
            g_strerror(errno));
    close(view_fd);
    view_fd = -1;
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     0,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for BYE deletion");

    view_fd = connect_unix_socket(socket_path,
                                  RESPONSE_TIMEOUT_MS,
                                  &error);
    REQUIRE_CALL(view_fd >= 0, "connect post-BYE view");
    REQUIRE_CALL(register_view(view_fd,
                               first_id,
                               &post_bye_id,
                               &error),
                 "register deleted identity proposal");
    REQUIRE(persistent_view_id_is_valid(post_bye_id),
            "post-BYE registration did not receive view-N identity");
    REQUIRE(g_strcmp0(post_bye_id, first_id) != 0,
            "BYE-deleted identity %s was incorrectly reclaimed",
            first_id);
    REQUIRE(mux_send_line(view_fd, "BYE"),
            "send final BYE: %s",
            g_strerror(errno));
    close(view_fd);
    view_fd = -1;
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     0,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for final view deletion");

cleanup:
    if (view_fd >= 0)
        close(view_fd);
    if (daemon_pid > 1 && socket_path != NULL) {
        GError *stop_error = NULL;

        if (!stop_identified_daemon(socket_path,
                                    daemon_pid,
                                    &stop_error)) {
            g_test_message("stop identified muxd: %s",
                           stop_error != NULL
                               ? stop_error->message
                               : "unknown error");
            g_test_fail();
        }
        g_clear_error(&stop_error);
    }
    if (guard_fd >= 0)
        close(guard_fd);
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    restore_environment("XDG_STATE_HOME", old_state);
    restore_environment("MUX_EPHEMERAL", old_ephemeral);
    (void)umask(old_umask);
    g_free(muxctl_output);
    g_free(post_bye_id);
    g_free(reclaimed_id);
    g_free(first_id);
    g_free(socket_path);
    g_free(state_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_ephemeral);
    g_free(old_state);
    g_free(old_runtime);
}

static void test_muxd_async_move_and_queued_replies(void)
{
    static const gchar kitten_script[] =
        "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *\"--use-password=always\"*\"detach-window\"*\"--match id:301\"*\"--target-tab window_id:401\"*)\n"
        "    : > \"$MUX_TEST_KITTEN_MARKER\"\n"
        "    sleep 1\n"
        "    exit 0\n"
        "    ;;\n"
        "  *\"--use-password=always\"*\"detach-window\"*\"--match id:302\"*\"--target-tab window_id:401\"*)\n"
        "    : > \"$MUX_TEST_KITTEN_MARKER\"\n"
        "    exec sleep 10\n"
        "    ;;\n"
        "esac\n"
        "exit 64\n";
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *state_dir = NULL;
    gchar *bin_dir = NULL;
    gchar *kitten_path = NULL;
    gchar *marker_path = NULL;
    gchar *socket_path = NULL;
    gchar *path_value = NULL;
    gchar *target_id = NULL;
    gchar *source_id = NULL;
    gchar *timeout_source_id = NULL;
    gchar *fallback_id = NULL;
    gchar *move_response = NULL;
    gchar *observed_layer = NULL;
    gchar *error_message = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *old_state = g_strdup(g_getenv("XDG_STATE_HOME"));
    gchar *old_ephemeral = g_strdup(g_getenv("MUX_EPHEMERAL"));
    gchar *old_path = g_strdup(g_getenv("PATH"));
    gchar *old_marker = g_strdup(g_getenv("MUX_TEST_KITTEN_MARKER"));
    GError *error = NULL;
    struct ucred daemon_credentials;
    int guard_fd = -1;
    int target_fd = -1;
    int source_fd = -1;
    int timeout_source_fd = -1;
    int fallback_fd = -1;
    int move_fd = -1;
    pid_t daemon_pid = -1;
    mode_t old_umask = umask(0077);

    root = g_dir_make_tmp("muxd-move-integration-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create temporary root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    state_dir = g_build_filename(root, "state", NULL);
    bin_dir = g_build_filename(root, "bin", NULL);
    marker_path = g_build_filename(root, "kitty-started", NULL);
    kitten_path = g_build_filename(bin_dir, "kitten", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(state_dir, 0700) == 0,
            "create state directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(bin_dir, 0700) == 0,
            "create bin directory: %s",
            g_strerror(errno));
    REQUIRE_CALL(g_file_set_contents(kitten_path,
                                     kitten_script,
                                     -1,
                                     &error),
                 "write fake kitten");
    REQUIRE(g_chmod(kitten_path, 0700) == 0,
            "chmod fake kitten: %s",
            g_strerror(errno));

    path_value = g_strdup_printf("%s:%s",
                                 bin_dir,
                                 old_path != NULL ? old_path : "");
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set XDG_RUNTIME_DIR");
    REQUIRE(g_setenv("XDG_STATE_HOME", state_dir, TRUE),
            "set XDG_STATE_HOME");
    REQUIRE(g_setenv("MUX_EPHEMERAL", "0", TRUE),
            "set persistent peer identity");
    REQUIRE(g_setenv("PATH", path_value, TRUE), "set PATH");
    REQUIRE(g_setenv("MUX_TEST_KITTEN_MARKER", marker_path, TRUE),
            "set fake kitten marker");
    socket_path =
        g_build_filename(runtime_dir, "mux", "muxd.sock", NULL);

    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "start muxd");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect to muxd");
    REQUIRE_CALL(get_peer_credentials(guard_fd,
                                      &daemon_credentials,
                                      &error),
                 "identify muxd peer");
    daemon_pid = daemon_credentials.pid;

    target_fd = connect_unix_socket(socket_path,
                                    RESPONSE_TIMEOUT_MS,
                                    &error);
    REQUIRE_CALL(target_fd >= 0, "connect target view");
    REQUIRE_CALL(register_view_with_identity(target_fd,
                                             "",
                                             "401",
                                             "integration-kitty",
                                             "target",
                                             &target_id,
                                             &error),
                 "register target view");
    source_fd = connect_unix_socket(socket_path,
                                    RESPONSE_TIMEOUT_MS,
                                    &error);
    REQUIRE_CALL(source_fd >= 0, "connect successful source view");
    REQUIRE_CALL(register_view_with_identity(source_fd,
                                             "",
                                             "301",
                                             "integration-kitty",
                                             "main",
                                             &source_id,
                                             &error),
                 "register successful source view");
    timeout_source_fd = connect_unix_socket(socket_path,
                                            RESPONSE_TIMEOUT_MS,
                                            &error);
    REQUIRE_CALL(timeout_source_fd >= 0, "connect timeout source view");
    REQUIRE_CALL(register_view_with_identity(timeout_source_fd,
                                             "",
                                             "302",
                                             "integration-kitty",
                                             "main",
                                             &timeout_source_id,
                                             &error),
                 "register timeout source view");
    fallback_fd = connect_unix_socket(socket_path,
                                      RESPONSE_TIMEOUT_MS,
                                      &error);
    REQUIRE_CALL(fallback_fd >= 0, "connect fallback view");
    REQUIRE_CALL(register_view_with_identity(fallback_fd,
                                             "",
                                             "303",
                                             "integration-kitty",
                                             "main",
                                             &fallback_id,
                                             &error),
                 "register fallback view");

    move_fd = request_move(socket_path, source_id, "target", &error);
    REQUIRE_CALL(move_fd >= 0, "request successful move");
    REQUIRE(wait_for_path(marker_path, 1000),
            "fake kitten did not start successful move");
    REQUIRE(mux_send_line(source_fd, "FOCUS\t1"),
            "send delayed source focus: %s",
            g_strerror(errno));
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       source_id,
                                       "main",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "observe delayed focus during move");
    {
        guint count = 0;
        gint64 started_us = g_get_monotonic_time();

        REQUIRE_CALL(query_view_count(socket_path, &count, &error),
                     "query status while move is pending");
        REQUIRE(count == 4, "pending move changed view count to %u", count);
        REQUIRE(g_get_monotonic_time() - started_us < 500 * 1000,
                "pending move blocked daemon control traffic");
    }
    move_response = mux_read_line(move_fd, 3000);
    REQUIRE(move_response != NULL && g_strcmp0(move_response, "OK") == 0,
            "unexpected successful MOVE response: %s",
            move_response != NULL ? move_response : "(none)");
    close(move_fd);
    move_fd = -1;
    g_clear_pointer(&move_response, g_free);
    observed_layer = query_view_layer(socket_path, source_id, &error);
    REQUIRE_CALL(observed_layer != NULL, "query successful moved layer");
    REQUIRE(g_strcmp0(observed_layer, "target") == 0,
            "successful move recorded layer %s",
            observed_layer);
    g_clear_pointer(&observed_layer, g_free);
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       timeout_source_id,
                                       "main",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "reconcile active view after move");

    REQUIRE_CALL(set_current_layer(socket_path, "empty", &error),
                 "select empty layer");
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       "",
                                       "empty",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "clear active view on empty layer");
    REQUIRE_CALL(set_current_layer(socket_path, "main", &error),
                 "restore main layer");
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       timeout_source_id,
                                       "main",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "select deterministic main-layer view");

    REQUIRE(mux_send_line(fallback_fd, "FOCUS\t1"),
            "focus fallback view: %s",
            g_strerror(errno));
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       fallback_id,
                                       "main",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "observe focused fallback view");
    close(fallback_fd);
    fallback_fd = -1;
    REQUIRE_CALL(wait_for_view_count(socket_path,
                                     3,
                                     STATE_TIMEOUT_MS,
                                     &error),
                 "wait for focused fallback removal");
    REQUIRE_CALL(wait_for_active_state(socket_path,
                                       timeout_source_id,
                                       "main",
                                       STATE_TIMEOUT_MS,
                                       &error),
                 "reconcile active view after removal");

    (void)g_unlink(marker_path);
    move_fd = request_move(socket_path,
                           timeout_source_id,
                           "target",
                           &error);
    REQUIRE_CALL(move_fd >= 0, "request timing-out move");
    REQUIRE(wait_for_path(marker_path, 1000),
            "fake kitten did not start timing-out move");
    move_response = mux_read_line(move_fd, 3500);
    REQUIRE(move_response != NULL,
            "timing-out MOVE returned no response");
    {
        gchar **fields = g_strsplit(move_response, "\t", -1);

        if (g_strv_length(fields) >= 2 &&
            g_strcmp0(fields[0], "ERR") == 0)
            error_message = mux_decode(fields[1]);
        g_strfreev(fields);
    }
    REQUIRE(g_strcmp0(error_message, "Kitty layer move timed out") == 0,
            "unexpected timeout response: %s",
            error_message != NULL ? error_message : move_response);
    close(move_fd);
    move_fd = -1;
    g_clear_pointer(&move_response, g_free);
    g_clear_pointer(&error_message, g_free);
    observed_layer = query_view_layer(socket_path,
                                      timeout_source_id,
                                      &error);
    REQUIRE_CALL(observed_layer != NULL, "query timed-out moved layer");
    REQUIRE(g_strcmp0(observed_layer, "main") == 0,
            "timed-out move changed metadata to layer %s",
            observed_layer);

cleanup:
    if (move_fd >= 0)
        close(move_fd);
    if (fallback_fd >= 0)
        close(fallback_fd);
    if (timeout_source_fd >= 0)
        close(timeout_source_fd);
    if (source_fd >= 0)
        close(source_fd);
    if (target_fd >= 0)
        close(target_fd);
    if (daemon_pid > 1 && socket_path != NULL) {
        GError *stop_error = NULL;

        if (!stop_identified_daemon(socket_path,
                                    daemon_pid,
                                    &stop_error)) {
            g_test_message("stop identified muxd: %s",
                           stop_error != NULL
                               ? stop_error->message
                               : "unknown error");
            g_test_fail();
        }
        g_clear_error(&stop_error);
    }
    if (guard_fd >= 0)
        close(guard_fd);
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    restore_environment("XDG_STATE_HOME", old_state);
    restore_environment("MUX_EPHEMERAL", old_ephemeral);
    restore_environment("PATH", old_path);
    restore_environment("MUX_TEST_KITTEN_MARKER", old_marker);
    (void)umask(old_umask);
    g_free(error_message);
    g_free(observed_layer);
    g_free(move_response);
    g_free(fallback_id);
    g_free(timeout_source_id);
    g_free(source_id);
    g_free(target_id);
    g_free(path_value);
    g_free(socket_path);
    g_free(marker_path);
    g_free(kitten_path);
    g_free(bin_dir);
    g_free(state_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_marker);
    g_free(old_path);
    g_free(old_ephemeral);
    g_free(old_state);
    g_free(old_runtime);
}

static void test_muxctl_requires_complete_response(void)
{
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *mux_dir = NULL;
    gchar *socket_path = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    GError *error = NULL;
    pid_t server_pid = -1;
    mode_t old_umask = umask(0077);

    root = g_dir_make_tmp("muxctl-response-integration-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create muxctl response root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    mux_dir = g_build_filename(runtime_dir, "mux", NULL);
    socket_path = g_build_filename(mux_dir, "muxd.sock", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(mux_dir, 0700) == 0,
            "create mux runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set XDG_RUNTIME_DIR");

    server_pid = spawn_silent_muxd(socket_path, 0, &error);
    REQUIRE_CALL(server_pid > 0, "start EOF fake muxd");
    REQUIRE(!run_command(muxctl_executable,
                         "status",
                         CHILD_TIMEOUT_MS,
                         &error),
            "muxctl accepted EOF without a complete response");
    g_clear_error(&error);
    REQUIRE_CALL(wait_child_success(server_pid,
                                    CHILD_TIMEOUT_MS,
                                    &error),
                 "reap EOF fake muxd");
    server_pid = -1;
    (void)g_unlink(socket_path);

    server_pid = spawn_silent_muxd(socket_path, 5500, &error);
    REQUIRE_CALL(server_pid > 0, "start timeout fake muxd");
    {
        gint64 started_us = g_get_monotonic_time();

        REQUIRE(!run_command(muxctl_executable,
                             "status",
                             7000,
                             &error),
                "muxctl accepted a response timeout as success");
        REQUIRE(g_get_monotonic_time() - started_us >= 4500 * 1000,
                "muxctl response deadline was not above muxd move timeout");
    }
    g_clear_error(&error);
    REQUIRE_CALL(wait_child_success(server_pid,
                                    CHILD_TIMEOUT_MS,
                                    &error),
                 "reap timeout fake muxd");
    server_pid = -1;

cleanup:
    if (server_pid > 0)
        reap_forcefully(server_pid);
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    (void)umask(old_umask);
    g_free(socket_path);
    g_free(mux_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_runtime);
}

static void test_muxctl_stop_lifecycle(void)
{
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *state_dir = NULL;
    gchar *socket_path = NULL;
    gchar *view_ready_path = NULL;
    gchar *bar_ready_path = NULL;
    gchar *bar_closed_path = NULL;
    gchar *broken_ready_path = NULL;
    gchar *broken_closed_path = NULL;
    gchar *reclaim_ready_path = NULL;
    gchar *view_id = NULL;
    gchar *reclaimed_id = NULL;
    gchar *output = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *old_state = g_strdup(g_getenv("XDG_STATE_HOME"));
    gchar *old_ephemeral = g_strdup(g_getenv("MUX_EPHEMERAL"));
    GError *error = NULL;
    struct ucred credentials;
    int guard_fd = -1;
    int malformed_fd = -1;
    pid_t daemon_pid = -1;
    pid_t view_pid = -1;
    pid_t bar_pid = -1;
    pid_t broken_pid = -1;
    pid_t reclaim_pid = -1;
    mode_t old_umask = umask(0077);

    root = g_dir_make_tmp("muxctl-stop-integration-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create muxctl stop root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    state_dir = g_build_filename(root, "state", NULL);
    socket_path =
        g_build_filename(runtime_dir, "mux", "muxd.sock", NULL);
    view_ready_path = g_build_filename(root, "view-ready", NULL);
    bar_ready_path = g_build_filename(root, "bar-ready", NULL);
    bar_closed_path = g_build_filename(root, "bar-closed", NULL);
    broken_ready_path = g_build_filename(root, "broken-ready", NULL);
    broken_closed_path = g_build_filename(root, "broken-closed", NULL);
    reclaim_ready_path = g_build_filename(root, "reclaim-ready", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(state_dir, 0700) == 0,
            "create state directory: %s",
            g_strerror(errno));
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set XDG_RUNTIME_DIR");
    REQUIRE(g_setenv("XDG_STATE_HOME", state_dir, TRUE),
            "set XDG_STATE_HOME");
    REQUIRE(g_setenv("MUX_EPHEMERAL", "0", TRUE),
            "set persistent peer identity");

    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "start muxd for stop test");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect stop-test guard");
    REQUIRE_CALL(get_peer_credentials(guard_fd, &credentials, &error),
                 "identify stop-test muxd");
    daemon_pid = credentials.pid;

    malformed_fd = connect_unix_socket(socket_path,
                                       RESPONSE_TIMEOUT_MS,
                                       &error);
    REQUIRE_CALL(malformed_fd >= 0, "connect malformed STOP client");
    REQUIRE(mux_send_line(malformed_fd, "CTL\tSTOP\textra"),
            "send malformed STOP: %s",
            g_strerror(errno));
    {
        g_autofree gchar *line =
            mux_read_line(malformed_fd, RESPONSE_TIMEOUT_MS);
        g_auto(GStrv) fields = line != NULL
            ? g_strsplit(line, "\t", -1)
            : NULL;
        g_autofree gchar *message =
            fields != NULL && g_strv_length(fields) >= 2
            ? mux_decode(fields[1])
            : NULL;

        REQUIRE(g_strcmp0(message, "unknown or malformed command") == 0,
                "unexpected malformed STOP response: %s",
                line != NULL ? line : "(none)");
    }
    close(malformed_fd);
    malformed_fd = -1;
    REQUIRE(!run_command_with_extra_argument(muxctl_executable,
                                             "stop",
                                             "extra",
                                             CHILD_TIMEOUT_MS,
                                             &error),
            "muxctl accepted an extra STOP argument");
    g_clear_error(&error);
    {
        guint count = G_MAXUINT;

        REQUIRE_CALL(query_view_count(socket_path, &count, &error),
                     "query daemon after rejected STOP commands");
        REQUIRE(count == 0,
                "rejected STOP command changed view count to %u",
                count);
    }

    view_pid = spawn_stoppable_view(socket_path,
                                    "",
                                    view_ready_path,
                                    &error);
    REQUIRE_CALL(view_pid > 0, "spawn stoppable view");
    bar_pid = spawn_stoppable_bar(socket_path,
                                  bar_ready_path,
                                  bar_closed_path,
                                  &error);
    REQUIRE_CALL(bar_pid > 0, "spawn stoppable bar");
    broken_pid = spawn_transport_breaking_view(socket_path,
                                               broken_ready_path,
                                               broken_closed_path,
                                               &error);
    REQUIRE_CALL(broken_pid > 0, "spawn transport-breaking view");
    REQUIRE(wait_for_path(view_ready_path, STATE_TIMEOUT_MS),
            "stoppable view did not become ready");
    REQUIRE(wait_for_path(bar_ready_path, STATE_TIMEOUT_MS),
            "stoppable bar did not become ready");
    REQUIRE(wait_for_path(broken_ready_path, STATE_TIMEOUT_MS),
            "transport-breaking view did not become ready");
    REQUIRE_CALL(g_file_get_contents(view_ready_path,
                                     &view_id,
                                     NULL,
                                     &error),
                 "read persistent view ID");
    REQUIRE(persistent_view_id_is_valid(view_id),
            "stoppable view received invalid ID %s",
            view_id);

    output = run_command_capture(muxctl_executable,
                                 "stop",
                                 7000,
                                 &error);
    REQUIRE_CALL(output != NULL, "run muxctl stop");
    REQUIRE(g_strcmp0(output, "muxd stopped\n") == 0,
            "unexpected muxctl stop output: %s",
            output);
    g_clear_pointer(&output, g_free);
    REQUIRE_CALL(reap_child_exit_now(view_pid, 0, &error),
                 "graceful view was alive after successful stop");
    view_pid = -1;
    REQUIRE_CALL(reap_child_signal_now(broken_pid, SIGKILL, &error),
                 "transport-breaking view survived successful stop");
    broken_pid = -1;
    close(guard_fd);
    guard_fd = -1;
    REQUIRE_CALL(wait_child_success(bar_pid,
                                    CHILD_TIMEOUT_MS,
                                    &error),
                 "reap transport-stopped bar");
    bar_pid = -1;
    REQUIRE(wait_for_path(bar_closed_path, STATE_TIMEOUT_MS),
            "bar did not observe daemon transport closure");
    REQUIRE(wait_for_path(broken_closed_path, STATE_TIMEOUT_MS),
            "transport-breaking view did not close its control socket");
    REQUIRE(wait_for_path_absent(socket_path, STATE_TIMEOUT_MS),
            "muxctl stop left the daemon socket behind");
    REQUIRE(wait_for_pid_terminated(daemon_pid, STATE_TIMEOUT_MS),
            "muxctl stop left muxd pid %ld running",
            (long)daemon_pid);
    daemon_pid = -1;

    output = run_command_capture(muxctl_executable,
                                 "stop",
                                 CHILD_TIMEOUT_MS,
                                 &error);
    REQUIRE_CALL(output != NULL, "repeat muxctl stop while absent");
    REQUIRE(g_strcmp0(output, "muxd is not running\n") == 0,
            "unexpected absent stop output: %s",
            output);
    g_clear_pointer(&output, g_free);

    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "restart muxd after controlled stop");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect replacement muxd");
    REQUIRE_CALL(get_peer_credentials(guard_fd, &credentials, &error),
                 "identify replacement muxd");
    daemon_pid = credentials.pid;
    reclaim_pid = spawn_stoppable_view(socket_path,
                                       view_id,
                                       reclaim_ready_path,
                                       &error);
    REQUIRE_CALL(reclaim_pid > 0, "spawn reclaiming view");
    REQUIRE(wait_for_path(reclaim_ready_path, STATE_TIMEOUT_MS),
            "reclaiming view did not become ready");
    REQUIRE_CALL(g_file_get_contents(reclaim_ready_path,
                                     &reclaimed_id,
                                     NULL,
                                     &error),
                 "read reclaimed persistent view ID");
    REQUIRE(g_strcmp0(reclaimed_id, view_id) == 0,
            "controlled stop lost session view %s and returned %s",
            view_id,
            reclaimed_id);

    output = run_command_capture(muxctl_executable,
                                 "stop",
                                 7000,
                                 &error);
    REQUIRE_CALL(output != NULL, "stop replacement muxd");
    REQUIRE(g_strcmp0(output, "muxd stopped\n") == 0,
            "unexpected replacement stop output: %s",
            output);
    g_clear_pointer(&output, g_free);
    close(guard_fd);
    guard_fd = -1;
    REQUIRE_CALL(wait_child_success(reclaim_pid,
                                    CHILD_TIMEOUT_MS,
                                    &error),
                 "reap reclaiming view");
    reclaim_pid = -1;
    REQUIRE(wait_for_pid_terminated(daemon_pid, STATE_TIMEOUT_MS),
            "replacement muxd pid %ld remained running",
            (long)daemon_pid);
    daemon_pid = -1;

cleanup:
    if (malformed_fd >= 0)
        close(malformed_fd);
    if (guard_fd >= 0)
        close(guard_fd);
    if (reclaim_pid > 0)
        reap_forcefully(reclaim_pid);
    if (broken_pid > 0)
        reap_forcefully(broken_pid);
    if (bar_pid > 0)
        reap_forcefully(bar_pid);
    if (view_pid > 0)
        reap_forcefully(view_pid);
    if (daemon_pid > 1 && socket_path != NULL) {
        GError *stop_error = NULL;

        if (!stop_identified_daemon(socket_path,
                                    daemon_pid,
                                    &stop_error)) {
            g_test_message("stop muxctl-stop test daemon: %s",
                           stop_error != NULL
                               ? stop_error->message
                               : "unknown error");
            g_test_fail();
        }
        g_clear_error(&stop_error);
    }
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    restore_environment("XDG_STATE_HOME", old_state);
    restore_environment("MUX_EPHEMERAL", old_ephemeral);
    (void)umask(old_umask);
    g_free(output);
    g_free(reclaimed_id);
    g_free(view_id);
    g_free(reclaim_ready_path);
    g_free(broken_closed_path);
    g_free(broken_ready_path);
    g_free(bar_closed_path);
    g_free(bar_ready_path);
    g_free(view_ready_path);
    g_free(socket_path);
    g_free(state_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_ephemeral);
    g_free(old_state);
    g_free(old_runtime);
}

static void test_muxctl_engine_lifecycle(void)
{
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *state_dir = NULL;
    gchar *socket_path = NULL;
    gchar *graceful_ready = NULL;
    gchar *stubborn_ready = NULL;
    gchar *output = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *old_state = g_strdup(g_getenv("XDG_STATE_HOME"));
    GError *error = NULL;
    struct ucred credentials;
    int guard_fd = -1;
    pid_t daemon_pid = -1;
    pid_t graceful_pid = -1;
    pid_t stubborn_pid = -1;
    mode_t old_umask = umask(0077);

    root = g_dir_make_tmp("muxctl-engine-integration-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create engine lifecycle root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    state_dir = g_build_filename(root, "state", NULL);
    socket_path = g_build_filename(runtime_dir, "mux", "muxd.sock", NULL);
    graceful_ready = g_build_filename(root, "graceful-ready", NULL);
    stubborn_ready = g_build_filename(root, "stubborn-ready", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create engine runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(state_dir, 0700) == 0,
            "create engine state directory: %s",
            g_strerror(errno));
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set engine XDG_RUNTIME_DIR");
    REQUIRE(g_setenv("XDG_STATE_HOME", state_dir, TRUE),
            "set engine XDG_STATE_HOME");

    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "start muxd for engine lifecycle");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect engine lifecycle guard");
    REQUIRE_CALL(get_peer_credentials(guard_fd, &credentials, &error),
                 "identify engine lifecycle muxd");
    daemon_pid = credentials.pid;

    REQUIRE_CALL(engine_registration_denied(socket_path,
                                            "spoofed",
                                            getpid() + 1,
                                            &error),
                 "deny spoofed engine pid");
    REQUIRE_CALL(engine_registration_denied(socket_path,
                                            "../malformed",
                                            getpid(),
                                            &error),
                 "deny malformed engine profile");

    graceful_pid = spawn_registered_engine(socket_path,
                                           "graceful",
                                           graceful_ready,
                                           TRUE,
                                           &error);
    REQUIRE_CALL(graceful_pid > 0, "spawn graceful engine");
    stubborn_pid = spawn_registered_engine(socket_path,
                                           "stubborn",
                                           stubborn_ready,
                                           FALSE,
                                           &error);
    REQUIRE_CALL(stubborn_pid > 0, "spawn stubborn engine");
    REQUIRE(wait_for_path(graceful_ready, STATE_TIMEOUT_MS),
            "graceful engine did not register");
    REQUIRE(wait_for_path(stubborn_ready, STATE_TIMEOUT_MS),
            "stubborn engine did not register");

    output = run_command_capture(muxctl_executable,
                                 "stop",
                                 7000,
                                 &error);
    REQUIRE_CALL(output != NULL, "stop muxd with registered engines");
    REQUIRE(g_strcmp0(output, "muxd stopped\n") == 0,
            "unexpected engine stop output: %s",
            output);
    g_clear_pointer(&output, g_free);
    REQUIRE_CALL(reap_child_exit_now(graceful_pid, 0, &error),
                 "graceful engine was alive after successful stop");
    graceful_pid = -1;
    REQUIRE_CALL(reap_child_signal_now(stubborn_pid, SIGKILL, &error),
                 "stubborn engine did not reach pidfd SIGKILL");
    stubborn_pid = -1;
    close(guard_fd);
    guard_fd = -1;
    REQUIRE(wait_for_path_absent(socket_path, STATE_TIMEOUT_MS),
            "engine lifecycle stop left muxd socket behind");
    REQUIRE(wait_for_pid_terminated(daemon_pid, STATE_TIMEOUT_MS),
            "engine lifecycle stop left muxd running");
    daemon_pid = -1;

    output = run_command_capture(muxctl_executable,
                                 "stop",
                                 CHILD_TIMEOUT_MS,
                                 &error);
    REQUIRE_CALL(output != NULL, "repeat engine lifecycle stop");
    REQUIRE(g_strcmp0(output, "muxd is not running\n") == 0,
            "unexpected repeated engine stop output: %s",
            output);

cleanup:
    if (graceful_pid > 0)
        reap_forcefully(graceful_pid);
    if (stubborn_pid > 0)
        reap_forcefully(stubborn_pid);
    if (daemon_pid > 1 && socket_path != NULL) {
        GError *stop_error = NULL;

        if (!stop_identified_daemon(socket_path,
                                    daemon_pid,
                                    &stop_error))
            g_test_message("stop engine lifecycle muxd: %s",
                           stop_error ? stop_error->message : "unknown error");
        g_clear_error(&stop_error);
    }
    if (guard_fd >= 0)
        close(guard_fd);
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    restore_environment("XDG_STATE_HOME", old_state);
    (void)umask(old_umask);
    g_free(output);
    g_free(stubborn_ready);
    g_free(graceful_ready);
    g_free(socket_path);
    g_free(state_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_state);
    g_free(old_runtime);
}

static void test_muxd_ensure_timeout_reaps_owned_child(void)
{
    gchar *root = NULL;
    gchar *runtime_dir = NULL;
    gchar *state_dir = NULL;
    gchar *socket_path = NULL;
    gchar *pid_path = NULL;
    gchar *pid_text = NULL;
    gchar *old_runtime = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *old_state = g_strdup(g_getenv("XDG_STATE_HOME"));
    gchar *old_delay =
        g_strdup(g_getenv("MUX_TEST_ENSURE_STARTUP_DELAY_MS"));
    gchar *old_pid_file =
        g_strdup(g_getenv("MUX_TEST_ENSURE_CHILD_PID_FILE"));
    GError *error = NULL;
    struct ucred daemon_credentials;
    int guard_fd = -1;
    pid_t daemon_pid = -1;
    pid_t delayed_pid = -1;
    mode_t old_umask = umask(0077);

    root = g_dir_make_tmp("muxd-ensure-timeout-XXXXXX", &error);
    REQUIRE_CALL(root != NULL, "create ensure timeout root");
    runtime_dir = g_build_filename(root, "runtime", NULL);
    state_dir = g_build_filename(root, "state", NULL);
    socket_path =
        g_build_filename(runtime_dir, "mux", "muxd.sock", NULL);
    pid_path = g_build_filename(root, "delayed-child.pid", NULL);
    REQUIRE(g_mkdir(runtime_dir, 0700) == 0,
            "create runtime directory: %s",
            g_strerror(errno));
    REQUIRE(g_mkdir(state_dir, 0700) == 0,
            "create state directory: %s",
            g_strerror(errno));
    REQUIRE(g_setenv("XDG_RUNTIME_DIR", runtime_dir, TRUE),
            "set XDG_RUNTIME_DIR");
    REQUIRE(g_setenv("XDG_STATE_HOME", state_dir, TRUE),
            "set XDG_STATE_HOME");
    REQUIRE(g_setenv("MUX_TEST_ENSURE_STARTUP_DELAY_MS", "10000", TRUE),
            "set ensure startup delay");
    REQUIRE(g_setenv("MUX_TEST_ENSURE_CHILD_PID_FILE", pid_path, TRUE),
            "set ensure child pid file");

    REQUIRE(!run_command(muxd_executable,
                         "--ensure",
                         CHILD_TIMEOUT_MS,
                         &error),
            "delayed muxd --ensure unexpectedly succeeded");
    g_clear_error(&error);
    REQUIRE(wait_for_path(pid_path, STATE_TIMEOUT_MS),
            "delayed ensure child did not publish its pid");
    REQUIRE_CALL(g_file_get_contents(pid_path,
                                     &pid_text,
                                     NULL,
                                     &error),
                 "read delayed ensure child pid");
    delayed_pid = (pid_t)g_ascii_strtoll(pid_text, NULL, 10);
    REQUIRE(delayed_pid > 1, "invalid delayed child pid: %s", pid_text);
    errno = 0;
    REQUIRE(kill(delayed_pid, 0) < 0 && errno == ESRCH,
            "delayed ensure child %ld was not terminated and reaped",
            (long)delayed_pid);
    REQUIRE(wait_for_path_absent(socket_path, STATE_TIMEOUT_MS),
            "owned stale muxd socket was not removed");

    g_unsetenv("MUX_TEST_ENSURE_STARTUP_DELAY_MS");
    g_unsetenv("MUX_TEST_ENSURE_CHILD_PID_FILE");
    REQUIRE_CALL(run_command(muxd_executable,
                             "--ensure",
                             CHILD_TIMEOUT_MS,
                             &error),
                 "start muxd after failed ensure cleanup");
    guard_fd = connect_unix_socket(socket_path,
                                   CONNECT_TIMEOUT_MS,
                                   &error);
    REQUIRE_CALL(guard_fd >= 0, "connect replacement muxd");
    REQUIRE_CALL(get_peer_credentials(guard_fd,
                                      &daemon_credentials,
                                      &error),
                 "identify replacement muxd");
    daemon_pid = daemon_credentials.pid;

cleanup:
    if (daemon_pid > 1 && socket_path != NULL) {
        GError *stop_error = NULL;

        if (!stop_identified_daemon(socket_path,
                                    daemon_pid,
                                    &stop_error)) {
            g_test_message("stop replacement muxd: %s",
                           stop_error != NULL
                               ? stop_error->message
                               : "unknown error");
            g_test_fail();
        }
        g_clear_error(&stop_error);
    }
    if (guard_fd >= 0)
        close(guard_fd);
    g_clear_error(&error);
    remove_tree(root);
    restore_environment("XDG_RUNTIME_DIR", old_runtime);
    restore_environment("XDG_STATE_HOME", old_state);
    restore_environment("MUX_TEST_ENSURE_STARTUP_DELAY_MS", old_delay);
    restore_environment("MUX_TEST_ENSURE_CHILD_PID_FILE", old_pid_file);
    (void)umask(old_umask);
    g_free(pid_text);
    g_free(pid_path);
    g_free(socket_path);
    g_free(state_dir);
    g_free(runtime_dir);
    g_free(root);
    g_free(old_pid_file);
    g_free(old_delay);
    g_free(old_state);
    g_free(old_runtime);
}

int main(int argc, char **argv)
{
    int result;

    if (argc < 3) {
        g_printerr("usage: %s [GLib test options] MUXD_PATH MUXCTL_PATH\n",
                   argv[0]);
        return EXIT_FAILURE;
    }

    muxd_executable = g_canonicalize_filename(argv[argc - 2], NULL);
    muxctl_executable = g_canonicalize_filename(argv[argc - 1], NULL);
    argc -= 2;
    argv[argc] = NULL;

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/muxd/integration/identity-lifecycle",
                    test_muxd_identity_lifecycle);
    g_test_add_func("/muxd/integration/async-move-and-queued-replies",
                    test_muxd_async_move_and_queued_replies);
    g_test_add_func("/muxd/integration/muxctl-complete-response",
                    test_muxctl_requires_complete_response);
    g_test_add_func("/muxd/integration/muxctl-stop-lifecycle",
                    test_muxctl_stop_lifecycle);
    g_test_add_func("/muxd/integration/muxctl-engine-lifecycle",
                    test_muxctl_engine_lifecycle);
    g_test_add_func("/muxd/integration/ensure-timeout-reaps-owned-child",
                    test_muxd_ensure_timeout_reaps_owned_child);
    result = g_test_run();

    g_free(muxctl_executable);
    g_free(muxd_executable);
    return result;
}
