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

static gboolean register_view(int fd,
                              const gchar *proposed_id,
                              gchar **assigned_id,
                              GError **error)
{
    g_autofree gchar *encoded_id =
        mux_encode(proposed_id != NULL ? proposed_id : "");
    g_autofree gchar *encoded_window = mux_encode("integration-window");
    g_autofree gchar *encoded_socket = mux_encode("integration-kitty");
    g_autofree gchar *encoded_layer = mux_encode("main");
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
    result = g_test_run();

    g_free(muxctl_executable);
    g_free(muxd_executable);
    return result;
}
