#define _GNU_SOURCE

#include "mux-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static gboolean send_all(int fd, const gchar *data, gsize length)
{
    while (length) {
        ssize_t written = send(fd, data, length, MSG_NOSIGNAL);
        if (written > 0) {
            data += written;
            length -= (gsize)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return FALSE;
    }
    return TRUE;
}

gchar *mux_socket_path(void)
{
    const gchar *runtime = g_get_user_runtime_dir();
    gchar *directory = NULL;

    if (runtime && *runtime)
        directory = g_build_filename(runtime, "mux", NULL);
    else
        directory = g_strdup_printf("%s/mux-%u", g_get_tmp_dir(), (guint)getuid());

    gchar *path = g_build_filename(directory, "muxd.sock", NULL);
    g_free(directory);
    return path;
}

int mux_connect_socket(void)
{
    gchar *path = mux_socket_path();
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_free(path);
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_free(path);
        return -1;
    }

    struct sockaddr_un address = { 0 };
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    g_free(path);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
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
        int saved_errno = errno ? errno : EPERM;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

gboolean mux_send_line(int fd, const gchar *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    gchar *body = g_strdup_vprintf(format, arguments);
    va_end(arguments);

    gchar *line = g_strconcat(body, "\n", NULL);
    gboolean ok = send_all(fd, line, strlen(line));
    g_free(line);
    g_free(body);
    return ok;
}

gchar *mux_read_line(int fd, int timeout_ms)
{
    GString *line = g_string_new(NULL);

    while (TRUE) {
        struct pollfd poll_fd = {
            .fd = fd,
            .events = POLLIN,
        };
        int result;
        do {
            result = poll(&poll_fd, 1, timeout_ms);
        } while (result < 0 && errno == EINTR);

        if (result <= 0 || !(poll_fd.revents & (POLLIN | POLLHUP))) {
            g_string_free(line, TRUE);
            return NULL;
        }

        gchar byte = 0;
        ssize_t count = recv(fd, &byte, 1, 0);
        if (count <= 0) {
            g_string_free(line, TRUE);
            return NULL;
        }
        if (byte == '\n')
            return g_string_free(line, FALSE);
        if (byte != '\r')
            g_string_append_c(line, byte);
        if (line->len > 1024 * 1024) {
            g_string_free(line, TRUE);
            errno = EMSGSIZE;
            return NULL;
        }
    }
}

gchar *mux_encode(const gchar *value)
{
    if (!value)
        value = "";
    return g_base64_encode((const guchar *)value, strlen(value));
}

gchar *mux_decode(const gchar *value)
{
    if (!value || !*value)
        return g_strdup("");

    gsize length = 0;
    guchar *decoded = g_base64_decode(value, &length);
    gchar *result = g_strndup((const gchar *)decoded, length);
    g_free(decoded);
    return result;
}
