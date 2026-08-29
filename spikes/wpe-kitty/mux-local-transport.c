#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mux-local-transport.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct _MuxLocalListener {
    gatomicrefcount references;
    gint fd;
    gint lock_fd;
    gchar *path;
    dev_t device;
    ino_t inode;
};

struct _MuxLocalConnection {
    gatomicrefcount references;
    gint fd;
    gboolean closed;
    gsize max_packet;
    gsize queue_limit;
    gsize queued_bytes;
    GQueue outgoing;
    guint8 *read_buffer;
    MuxLocalPeerCredentials peer;
};

static void
set_errno_error(GError **error, gint saved_errno, const gchar *operation)
{
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(saved_errno),
                "%s: %s",
                operation,
                g_strerror(saved_errno));
}

static gboolean
service_name_is_valid(const gchar *service)
{
    const guchar *cursor;

    if (service == NULL || *service == '\0' ||
        g_str_equal(service, ".") || g_str_equal(service, "..") ||
        strstr(service, "..") != NULL)
        return FALSE;

    for (cursor = (const guchar *) service; *cursor != '\0'; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '-' ||
              *cursor == '_' || *cursor == '.'))
            return FALSE;
    }

    return TRUE;
}

static gchar *
ensure_runtime_directory(GError **error)
{
    const gchar *runtime_directory = g_get_user_runtime_dir();
    g_autofree gchar *fallback = NULL;
    gchar *directory;
    struct stat status;

    if (runtime_directory == NULL || *runtime_directory == '\0') {
        fallback = g_strdup_printf("%s/mux-%lu",
                                   g_get_tmp_dir(),
                                   (gulong) geteuid());
        directory = g_steal_pointer(&fallback);
    } else {
        directory = g_build_filename(runtime_directory, "mux", NULL);
    }

    if (g_mkdir_with_parents(directory, 0700) < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "create mux runtime directory");
        g_free(directory);
        return NULL;
    }

    if (lstat(directory, &status) < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "inspect mux runtime directory");
        g_free(directory);
        return NULL;
    }

    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077) != 0 ||
        (status.st_mode & 0700) != 0700) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "mux runtime directory is not an owner-only directory: %s",
                    directory);
        g_free(directory);
        return NULL;
    }

    return directory;
}

gchar *
mux_local_transport_socket_path(const gchar *service, GError **error)
{
    g_autofree gchar *directory = NULL;
    gchar *path;
    struct sockaddr_un address = { 0 };

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    if (!service_name_is_valid(service)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid local service name");
        return NULL;
    }

    directory = ensure_runtime_directory(error);
    if (directory == NULL)
        return NULL;

    path = g_build_filename(directory, service, NULL);
    if (strlen(path) >= sizeof(address.sun_path)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FILENAME_TOO_LONG,
                    "local socket path is too long: %s",
                    path);
        g_free(path);
        return NULL;
    }

    return path;
}

static gboolean
set_descriptor_flags(gint fd, GError **error)
{
    gint flags;

    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "set close-on-exec");
        return FALSE;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "set nonblocking mode");
        return FALSE;
    }

    return TRUE;
}

static gint
open_seqpacket_socket(GError **error)
{
    gint fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    if (fd < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "create local seqpacket socket");
        return -1;
    }

    if (!set_descriptor_flags(fd, error)) {
        close(fd);
        return -1;
    }

    return fd;
}

static gboolean
socket_path_is_stale(const gchar *path,
                     struct stat *identity,
                     GError **error)
{
    struct stat status;
    struct sockaddr_un address = { 0 };
    gint probe;
    gint result;
    gint saved_errno;

    if (lstat(path, &status) < 0) {
        if (errno == ENOENT)
            return TRUE;
        saved_errno = errno;
        set_errno_error(error, saved_errno, "inspect existing local socket");
        return FALSE;
    }

    if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid()) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "refusing to replace untrusted socket path: %s",
                    path);
        return FALSE;
    }
    *identity = status;

    probe = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (probe < 0) {
        saved_errno = errno;
        set_errno_error(error, saved_errno, "probe existing local socket");
        return FALSE;
    }

    if (fcntl(probe, F_SETFL, fcntl(probe, F_GETFL) | O_NONBLOCK) < 0) {
        saved_errno = errno;
        close(probe);
        set_errno_error(error,
                        saved_errno,
                        "make local socket probe nonblocking");
        return FALSE;
    }

    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    do {
        result = connect(probe,
                         (const struct sockaddr *) &address,
                         sizeof(address));
    } while (result < 0 && errno == EINTR);
    saved_errno = errno;
    close(probe);

    if (result == 0 || saved_errno == EINPROGRESS ||
        saved_errno == EALREADY || saved_errno == EAGAIN) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_ADDRESS_IN_USE,
                    "local service is already running: %s",
                    path);
        return FALSE;
    }

    if (saved_errno != ECONNREFUSED && saved_errno != ENOENT) {
        set_errno_error(error, saved_errno, "probe existing local socket");
        return FALSE;
    }

    return TRUE;
}

static gboolean
socket_path_matches(const gchar *path, const struct stat *identity)
{
    struct stat current;

    return identity != NULL && lstat(path, &current) == 0 &&
           S_ISSOCK(current.st_mode) && current.st_uid == geteuid() &&
           current.st_dev == identity->st_dev &&
           current.st_ino == identity->st_ino;
}

static gint
acquire_service_lock(const gchar *socket_path, GError **error)
{
    g_autofree gchar *lock_path = g_strconcat(socket_path, ".lock", NULL);
    struct stat status;
    gint fd;

    fd = open(lock_path,
              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0) {
        set_errno_error(error, errno, "open local service lock");
        return -1;
    }
    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || fchmod(fd, 0600) < 0) {
        gint saved_errno = errno != 0 ? errno : EPERM;

        close(fd);
        set_errno_error(error, saved_errno, "validate local service lock");
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        gint saved_errno = errno;

        close(fd);
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_ADDRESS_IN_USE,
                        "local service is already starting or running: %s",
                        socket_path);
        } else {
            set_errno_error(error,
                            saved_errno,
                            "acquire local service lock");
        }
        return -1;
    }
    return fd;
}

MuxLocalListener *
mux_local_listener_new(const gchar *service, guint backlog, GError **error)
{
    g_autofree gchar *path = NULL;
    struct sockaddr_un address = { 0 };
    struct stat status;
    struct stat stale_identity;
    struct stat bound_identity;
    MuxLocalListener *listener;
    gint fd;
    gint lock_fd;
    gboolean bound_identity_valid = FALSE;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    path = mux_local_transport_socket_path(service, error);
    if (path == NULL)
        return NULL;

    lock_fd = acquire_service_lock(path, error);
    if (lock_fd < 0)
        return NULL;

    fd = open_seqpacket_socket(error);
    if (fd < 0) {
        close(lock_fd);
        return NULL;
    }

    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

    if (bind(fd, (const struct sockaddr *) &address, sizeof(address)) < 0) {
        gint saved_errno = errno;

        if (saved_errno != EADDRINUSE ||
            !socket_path_is_stale(path, &stale_identity, error) ||
            !socket_path_matches(path, &stale_identity) ||
            g_unlink(path) < 0 ||
            bind(fd, (const struct sockaddr *) &address, sizeof(address)) < 0) {
            if (error == NULL || *error == NULL) {
                saved_errno = errno;
                set_errno_error(error, saved_errno, "bind local socket");
            }
            close(fd);
            close(lock_fd);
            return NULL;
        }
    }

    if (lstat(path, &bound_identity) == 0 &&
        S_ISSOCK(bound_identity.st_mode) &&
        bound_identity.st_uid == geteuid())
        bound_identity_valid = TRUE;
    if (!bound_identity_valid || g_chmod(path, 0600) < 0 ||
        listen(fd, (gint) CLAMP(backlog, 1u, 128u)) < 0 ||
        lstat(path, &status) < 0 || !S_ISSOCK(status.st_mode) ||
        status.st_uid != geteuid() ||
        status.st_dev != bound_identity.st_dev ||
        status.st_ino != bound_identity.st_ino) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "finish local listener setup");
        close(fd);
        if (bound_identity_valid &&
            socket_path_matches(path, &bound_identity))
            g_unlink(path);
        close(lock_fd);
        return NULL;
    }

    listener = g_new0(MuxLocalListener, 1);
    g_atomic_ref_count_init(&listener->references);
    listener->fd = fd;
    listener->lock_fd = lock_fd;
    listener->path = g_steal_pointer(&path);
    listener->device = status.st_dev;
    listener->inode = status.st_ino;
    return listener;
}

void
mux_local_listener_unref(MuxLocalListener *listener)
{
    struct stat status;

    if (listener == NULL ||
        !g_atomic_ref_count_dec(&listener->references))
        return;

    if (listener->fd >= 0)
        close(listener->fd);

    if (listener->path != NULL && lstat(listener->path, &status) == 0 &&
        S_ISSOCK(status.st_mode) && status.st_uid == geteuid() &&
        status.st_dev == listener->device && status.st_ino == listener->inode)
        g_unlink(listener->path);
    if (listener->lock_fd >= 0)
        close(listener->lock_fd);

    g_free(listener->path);
    g_free(listener);
}

MuxLocalListener *
mux_local_listener_ref(MuxLocalListener *listener)
{
    g_return_val_if_fail(listener != NULL, NULL);
    g_atomic_ref_count_inc(&listener->references);
    return listener;
}

void
mux_local_listener_free(MuxLocalListener *listener)
{
    mux_local_listener_unref(listener);
}

gint
mux_local_listener_get_fd(const MuxLocalListener *listener)
{
    g_return_val_if_fail(listener != NULL, -1);
    return listener->fd;
}

const gchar *
mux_local_listener_get_path(const MuxLocalListener *listener)
{
    g_return_val_if_fail(listener != NULL, NULL);
    return listener->path;
}

static gboolean
read_peer_credentials(gint fd,
                      MuxLocalPeerCredentials *credentials,
                      GError **error)
{
#if defined(__linux__)
    struct ucred native_credentials;
    socklen_t size = sizeof(native_credentials);

    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_PEERCRED,
                   &native_credentials,
                   &size) < 0 || size != sizeof(native_credentials)) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "read peer credentials");
        return FALSE;
    }

    credentials->pid = native_credentials.pid;
    credentials->uid = native_credentials.uid;
    credentials->gid = native_credentials.gid;
    return TRUE;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__)
    uid_t uid;
    gid_t gid;

    if (getpeereid(fd, &uid, &gid) < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "read peer credentials");
        return FALSE;
    }

    credentials->pid = 0;
    credentials->uid = uid;
    credentials->gid = gid;
    return TRUE;
#else
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_NOT_SUPPORTED,
                "peer credential authentication is unsupported on this host");
    return FALSE;
#endif
}

MuxLocalConnection *
mux_local_connection_new_take_fd(gint fd,
                                 uid_t expected_uid,
                                 gsize max_packet,
                                 gsize queue_limit,
                                 GError **error)
{
    MuxLocalConnection *connection;
    gint socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    if (fd < 0 || max_packet == 0 || max_packet >= G_MAXSSIZE ||
        queue_limit < max_packet) {
        if (fd >= 0)
            close(fd);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid local connection limits or descriptor");
        return NULL;
    }

    if (getsockopt(fd,
                   SOL_SOCKET,
                   SO_TYPE,
                   &socket_type,
                   &socket_type_size) < 0) {
        gint saved_errno = errno;
        close(fd);
        set_errno_error(error, saved_errno, "inspect local socket type");
        return NULL;
    }

    if (socket_type != SOCK_SEQPACKET) {
        close(fd);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "descriptor is not a seqpacket socket");
        return NULL;
    }

    connection = g_new0(MuxLocalConnection, 1);
    g_atomic_ref_count_init(&connection->references);
    connection->fd = fd;
    connection->max_packet = max_packet;
    connection->queue_limit = queue_limit;
    g_queue_init(&connection->outgoing);

    if (!set_descriptor_flags(fd, error) ||
        !read_peer_credentials(fd, &connection->peer, error)) {
        mux_local_connection_unref(connection);
        return NULL;
    }

    if (connection->peer.uid != expected_uid) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    "rejected local peer uid %lu; expected %lu",
                    (gulong) connection->peer.uid,
                    (gulong) expected_uid);
        mux_local_connection_unref(connection);
        return NULL;
    }

    connection->read_buffer = g_malloc(max_packet);
    return connection;
}

MuxLocalConnection *
mux_local_listener_accept(MuxLocalListener *listener,
                          uid_t expected_uid,
                          gsize max_packet,
                          gsize queue_limit,
                          gboolean *out_would_block,
                          GError **error)
{
    gint fd;

    g_return_val_if_fail(listener != NULL, NULL);
    g_return_val_if_fail(out_would_block != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    *out_would_block = FALSE;
    do {
        fd = accept(listener->fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        gint saved_errno = errno;

        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
            *out_would_block = TRUE;
            return NULL;
        }

        set_errno_error(error, saved_errno, "accept local connection");
        return NULL;
    }

    return mux_local_connection_new_take_fd(fd,
                                            expected_uid,
                                            max_packet,
                                            queue_limit,
                                            error);
}

MuxLocalConnection *
mux_local_connection_connect(const gchar *service,
                             uid_t expected_uid,
                             gsize max_packet,
                             gsize queue_limit,
                             GError **error)
{
    g_autofree gchar *path = NULL;
    struct sockaddr_un address = { 0 };
    gint fd;
    gint result;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    path = mux_local_transport_socket_path(service, error);
    if (path == NULL)
        return NULL;

    fd = socket(AF_UNIX,
                SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                0);
    if (fd < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "create local client socket");
        return NULL;
    }

    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    do {
        result = connect(fd,
                         (const struct sockaddr *) &address,
                         sizeof(address));
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        gint saved_errno = errno;
        set_errno_error(error, saved_errno, "connect local service");
        close(fd);
        return NULL;
    }

    return mux_local_connection_new_take_fd(fd,
                                            expected_uid,
                                            max_packet,
                                            queue_limit,
                                            error);
}

MuxLocalConnection *
mux_local_connection_ref(MuxLocalConnection *connection)
{
    g_return_val_if_fail(connection != NULL, NULL);
    g_atomic_ref_count_inc(&connection->references);
    return connection;
}

void
mux_local_connection_unref(MuxLocalConnection *connection)
{
    GBytes *packet;

    if (connection == NULL ||
        !g_atomic_ref_count_dec(&connection->references))
        return;

    mux_local_connection_close(connection);
    while ((packet = g_queue_pop_head(&connection->outgoing)) != NULL)
        g_bytes_unref(packet);
    g_free(connection->read_buffer);
    g_free(connection);
}

gint
mux_local_connection_get_fd(const MuxLocalConnection *connection)
{
    g_return_val_if_fail(connection != NULL, -1);
    return connection->fd;
}

const MuxLocalPeerCredentials *
mux_local_connection_get_peer(const MuxLocalConnection *connection)
{
    g_return_val_if_fail(connection != NULL, NULL);
    return &connection->peer;
}

gsize
mux_local_connection_get_queued_bytes(const MuxLocalConnection *connection)
{
    g_return_val_if_fail(connection != NULL, 0);
    return connection->queued_bytes;
}

GIOCondition
mux_local_connection_wanted_condition(const MuxLocalConnection *connection)
{
    GIOCondition condition = G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;

    g_return_val_if_fail(connection != NULL, 0);
    if (!connection->closed && connection->outgoing.head != NULL)
        condition |= G_IO_OUT;
    return condition;
}

gboolean
mux_local_connection_queue(MuxLocalConnection *connection,
                           GBytes *packet,
                           GError **error)
{
    gsize size;

    g_return_val_if_fail(connection != NULL, FALSE);
    g_return_val_if_fail(packet != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    size = g_bytes_get_size(packet);
    if (connection->closed) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_CLOSED,
                    "local connection is closed");
        return FALSE;
    }

    if (size == 0 || size > connection->max_packet) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_MESSAGE_TOO_LARGE,
                    "local packet size %" G_GSIZE_FORMAT
                    " is outside the allowed range",
                    size);
        return FALSE;
    }

    if (size > connection->queue_limit - connection->queued_bytes) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NO_SPACE,
                    "local connection output queue limit exceeded");
        return FALSE;
    }

    g_queue_push_tail(&connection->outgoing, g_bytes_ref(packet));
    connection->queued_bytes += size;
    return TRUE;
}

static MuxLocalDispatchResult
dispatch_failure(MuxLocalConnection *connection,
                 GError **error,
                 GIOErrorEnum code,
                 const gchar *message)
{
    if (error != NULL && *error == NULL)
        g_set_error_literal(error, G_IO_ERROR, code, message);
    mux_local_connection_close(connection);
    return MUX_LOCAL_DISPATCH_ERROR;
}

static MuxLocalDispatchResult
drain_reads(MuxLocalConnection *connection,
            MuxLocalPacketFunc packet_func,
            gpointer user_data,
            GError **error)
{
    for (;;) {
        struct iovec vector = {
            .iov_base = connection->read_buffer,
            .iov_len = connection->max_packet,
        };
        struct msghdr message = {
            .msg_iov = &vector,
            .msg_iovlen = 1,
        };
        ssize_t received;

        do {
            received = recvmsg(connection->fd, &message, 0);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            gint saved_errno = errno;

            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
                return MUX_LOCAL_DISPATCH_OK;
            set_errno_error(error, saved_errno, "receive local packet");
            mux_local_connection_close(connection);
            return MUX_LOCAL_DISPATCH_ERROR;
        }

        if (received == 0) {
            mux_local_connection_close(connection);
            return MUX_LOCAL_DISPATCH_CLOSED;
        }

        if ((message.msg_flags & MSG_TRUNC) != 0 ||
            (gsize) received > connection->max_packet) {
            return dispatch_failure(connection,
                                    error,
                                    G_IO_ERROR_MESSAGE_TOO_LARGE,
                                    "received oversized local packet");
        }

        if (packet_func != NULL) {
            g_autoptr(GBytes) packet =
                g_bytes_new(connection->read_buffer, (gsize) received);

            if (!packet_func(connection, packet, user_data, error)) {
                return dispatch_failure(connection,
                                        error,
                                        G_IO_ERROR_INVALID_DATA,
                                        "local packet handler rejected packet");
            }
        }

        if (connection->closed)
            return MUX_LOCAL_DISPATCH_CLOSED;
    }
}

static MuxLocalDispatchResult
flush_writes(MuxLocalConnection *connection, GError **error)
{
    while (!g_queue_is_empty(&connection->outgoing)) {
        GBytes *packet = g_queue_peek_head(&connection->outgoing);
        gsize size;
        gconstpointer data = g_bytes_get_data(packet, &size);
        ssize_t sent;

        do {
            sent = send(connection->fd, data, size, MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);

        if (sent < 0) {
            gint saved_errno = errno;

            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)
                return MUX_LOCAL_DISPATCH_OK;
            set_errno_error(error, saved_errno, "send local packet");
            mux_local_connection_close(connection);
            return MUX_LOCAL_DISPATCH_ERROR;
        }

        if ((gsize) sent != size) {
            return dispatch_failure(connection,
                                    error,
                                    G_IO_ERROR_FAILED,
                                    "seqpacket send did not preserve packet boundary");
        }

        g_queue_pop_head(&connection->outgoing);
        connection->queued_bytes -= size;
        g_bytes_unref(packet);
    }

    return MUX_LOCAL_DISPATCH_OK;
}

MuxLocalDispatchResult
mux_local_connection_dispatch(MuxLocalConnection *connection,
                              GIOCondition condition,
                              MuxLocalPacketFunc packet_func,
                              gpointer user_data,
                              GError **error)
{
    MuxLocalDispatchResult result = MUX_LOCAL_DISPATCH_OK;
    MuxLocalConnection *guard;

    g_return_val_if_fail(connection != NULL, MUX_LOCAL_DISPATCH_ERROR);
    g_return_val_if_fail(error == NULL || *error == NULL,
                         MUX_LOCAL_DISPATCH_ERROR);

    if (connection->closed)
        return MUX_LOCAL_DISPATCH_CLOSED;

    guard = mux_local_connection_ref(connection);

    if ((condition & G_IO_NVAL) != 0) {
        result = dispatch_failure(connection,
                                  error,
                                  G_IO_ERROR_CLOSED,
                                  "local connection descriptor is invalid");
        goto out;
    }

    if ((condition & (G_IO_IN | G_IO_HUP)) != 0) {
        result = drain_reads(connection, packet_func, user_data, error);
        if (result != MUX_LOCAL_DISPATCH_OK)
            goto out;
    }

    if ((condition & G_IO_ERR) != 0) {
        gint socket_error = 0;
        socklen_t size = sizeof(socket_error);

        if (getsockopt(connection->fd,
                       SOL_SOCKET,
                       SO_ERROR,
                       &socket_error,
                       &size) < 0)
            socket_error = errno;
        if (socket_error == 0)
            socket_error = EIO;
        set_errno_error(error, socket_error, "local connection failure");
        mux_local_connection_close(connection);
        result = MUX_LOCAL_DISPATCH_ERROR;
        goto out;
    }

    if (!g_queue_is_empty(&connection->outgoing))
        result = flush_writes(connection, error);

out:
    mux_local_connection_unref(guard);
    return result;
}

void
mux_local_connection_close(MuxLocalConnection *connection)
{
    GBytes *packet;

    if (connection == NULL || connection->closed)
        return;

    connection->closed = TRUE;
    if (connection->fd >= 0) {
        shutdown(connection->fd, SHUT_RDWR);
        close(connection->fd);
        connection->fd = -1;
    }

    while ((packet = g_queue_pop_head(&connection->outgoing)) != NULL)
        g_bytes_unref(packet);
    connection->queued_bytes = 0;
}
