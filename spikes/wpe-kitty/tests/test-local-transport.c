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

typedef struct {
    MuxLocalConnection *connection;
    gint peer_fd;
} TestPair;

typedef struct {
    GPtrArray *packets;
    GBytes *reply;
    gboolean reject;
} PacketCapture;

static void
bytes_unref_pointer(gpointer data)
{
    g_bytes_unref(data);
}

static TestPair
test_pair_new(gsize max_packet, gsize queue_limit)
{
    g_autoptr(GError) error = NULL;
    TestPair pair = { NULL, -1 };
    gint descriptors[2] = { -1, -1 };
    gint flags;

    g_assert_cmpint(socketpair(AF_UNIX,
                               SOCK_SEQPACKET | SOCK_CLOEXEC,
                               0,
                               descriptors),
                    ==,
                    0);

    pair.connection = mux_local_connection_new_take_fd(descriptors[0],
                                                       geteuid(),
                                                       max_packet,
                                                       queue_limit,
                                                       &error);
    g_assert_no_error(error);
    g_assert_nonnull(pair.connection);

    pair.peer_fd = descriptors[1];
    flags = fcntl(pair.peer_fd, F_GETFL);
    g_assert_cmpint(flags, >=, 0);
    g_assert_cmpint(fcntl(pair.peer_fd, F_SETFL, flags | O_NONBLOCK), ==, 0);

    return pair;
}

static void
test_pair_clear(TestPair *pair)
{
    if (pair->connection != NULL)
        mux_local_connection_unref(pair->connection);
    if (pair->peer_fd >= 0)
        close(pair->peer_fd);

    pair->connection = NULL;
    pair->peer_fd = -1;
}

static PacketCapture
packet_capture_new(void)
{
    PacketCapture capture = {
        .packets = g_ptr_array_new_with_free_func(bytes_unref_pointer),
        .reply = NULL,
        .reject = FALSE,
    };

    return capture;
}

static void
packet_capture_clear(PacketCapture *capture)
{
    g_clear_pointer(&capture->packets, g_ptr_array_unref);
}

static void
assert_bytes_equal(GBytes *bytes,
                   const guint8 *expected,
                   gsize expected_size)
{
    gsize actual_size = 0;
    gconstpointer actual = g_bytes_get_data(bytes, &actual_size);

    g_assert_cmpuint(actual_size, ==, expected_size);
    g_assert_cmpmem(actual, actual_size, expected, expected_size);
}

static gboolean
capture_packet(MuxLocalConnection *connection,
               GBytes *packet,
               gpointer user_data,
               GError **error)
{
    PacketCapture *capture = user_data;

    g_ptr_array_add(capture->packets, g_bytes_ref(packet));
    if (capture->reply != NULL &&
        !mux_local_connection_queue(connection, capture->reply, error))
        return FALSE;

    return !capture->reject;
}

static void
assert_peer_packet(gint fd, const guint8 *expected, gsize expected_size)
{
    g_autofree guint8 *buffer = g_malloc(expected_size);
    ssize_t received;

    do {
        received = recv(fd, buffer, expected_size, MSG_DONTWAIT);
    } while (received < 0 && errno == EINTR);

    g_assert_cmpint(received, ==, (ssize_t) expected_size);
    g_assert_cmpmem(buffer, (gsize) received, expected, expected_size);
}

static guint
drain_peer_packets(gint fd, const guint8 *expected, gsize expected_size)
{
    g_autofree guint8 *buffer = g_malloc(expected_size);
    guint count = 0;

    for (;;) {
        ssize_t received;

        do {
            received = recv(fd, buffer, expected_size, MSG_DONTWAIT);
        } while (received < 0 && errno == EINTR);

        if (received < 0) {
            g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
            return count;
        }

        g_assert_cmpint(received, ==, (ssize_t) expected_size);
        g_assert_cmpmem(buffer, (gsize) received, expected, expected_size);
        count++;
    }
}

static void
test_send_receive_preserves_datagrams(void)
{
    static const guint8 outgoing[] = { 0x00, 0x01, 0x7f, 0x80, 0xff };
    static const guint8 incoming_one[] = { 'f', 'i', 'r', 's', 't' };
    static const guint8 incoming_two[] = { 's', 'e', 'c', 'o', 'n', 'd', 0x00 };
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) outgoing_packet =
        g_bytes_new(outgoing, sizeof(outgoing));
    PacketCapture capture = packet_capture_new();
    TestPair pair = test_pair_new(64, 256);
    const MuxLocalPeerCredentials *credentials;
    MuxLocalDispatchResult result;

    credentials = mux_local_connection_get_peer(pair.connection);
    g_assert_nonnull(credentials);
    g_assert_cmpuint(credentials->uid, ==, geteuid());
    g_assert_cmpuint(credentials->gid, ==, getegid());
#if defined(__linux__)
    g_assert_cmpint(credentials->pid, ==, getpid());
#endif

    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     0);
    g_assert_false((mux_local_connection_wanted_condition(pair.connection) &
                    G_IO_OUT) != 0);

    g_assert_true(mux_local_connection_queue(pair.connection,
                                             outgoing_packet,
                                             &error));
    g_assert_no_error(error);
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     sizeof(outgoing));
    g_assert_true((mux_local_connection_wanted_condition(pair.connection) &
                   G_IO_OUT) != 0);

    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_OUT,
                                           NULL,
                                           NULL,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     0);
    assert_peer_packet(pair.peer_fd, outgoing, sizeof(outgoing));

    g_assert_cmpint(send(pair.peer_fd,
                         incoming_one,
                         sizeof(incoming_one),
                         0),
                    ==,
                    (ssize_t) sizeof(incoming_one));
    g_assert_cmpint(send(pair.peer_fd,
                         incoming_two,
                         sizeof(incoming_two),
                         0),
                    ==,
                    (ssize_t) sizeof(incoming_two));

    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_IN,
                                           capture_packet,
                                           &capture,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    g_assert_cmpuint(capture.packets->len, ==, 2);
    assert_bytes_equal(g_ptr_array_index(capture.packets, 0),
                       incoming_one,
                       sizeof(incoming_one));
    assert_bytes_equal(g_ptr_array_index(capture.packets, 1),
                       incoming_two,
                       sizeof(incoming_two));

    packet_capture_clear(&capture);
    test_pair_clear(&pair);
}

static void
test_queue_backpressure_and_drain(void)
{
    enum {
        PACKET_SIZE = 1024,
        PACKET_COUNT = 512,
    };
    const gsize queue_limit = (gsize) PACKET_SIZE * PACKET_COUNT;
    g_autofree guint8 *payload = g_malloc(PACKET_SIZE);
    g_autoptr(GBytes) packet = NULL;
    g_autoptr(GError) error = NULL;
    TestPair pair = test_pair_new(PACKET_SIZE, queue_limit);
    MuxLocalDispatchResult result;
    guint received_count = 0;
    guint iterations = 0;
    gint send_buffer = 4096;

    memset(payload, 0xa5, PACKET_SIZE);
    packet = g_bytes_new(payload, PACKET_SIZE);

    g_assert_cmpint(setsockopt(mux_local_connection_get_fd(pair.connection),
                               SOL_SOCKET,
                               SO_SNDBUF,
                               &send_buffer,
                               sizeof(send_buffer)),
                    ==,
                    0);

    for (guint index = 0; index < PACKET_COUNT; index++) {
        g_assert_true(mux_local_connection_queue(pair.connection,
                                                 packet,
                                                 &error));
        g_assert_no_error(error);
    }
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     queue_limit);

    g_assert_false(mux_local_connection_queue(pair.connection,
                                              packet,
                                              &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);

    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_OUT,
                                           NULL,
                                           NULL,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     <,
                     queue_limit);
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     >,
                     0);
    g_assert_true((mux_local_connection_wanted_condition(pair.connection) &
                   G_IO_OUT) != 0);

    while (mux_local_connection_get_queued_bytes(pair.connection) > 0) {
        received_count += drain_peer_packets(pair.peer_fd,
                                             payload,
                                             PACKET_SIZE);
        result = mux_local_connection_dispatch(pair.connection,
                                               G_IO_OUT,
                                               NULL,
                                               NULL,
                                               &error);
        g_assert_no_error(error);
        g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
        iterations++;
        g_assert_cmpuint(iterations, <=, PACKET_COUNT);
    }

    received_count += drain_peer_packets(pair.peer_fd,
                                         payload,
                                         PACKET_SIZE);
    g_assert_cmpuint(received_count, ==, PACKET_COUNT);
    g_assert_false((mux_local_connection_wanted_condition(pair.connection) &
                    G_IO_OUT) != 0);

    test_pair_clear(&pair);
}

static void
test_peer_close_clears_queue(void)
{
    static const guint8 payload[] = { 'p', 'e', 'n', 'd', 'i', 'n', 'g' };
    g_autoptr(GBytes) packet = g_bytes_new(payload, sizeof(payload));
    g_autoptr(GError) error = NULL;
    TestPair pair = test_pair_new(64, 256);
    MuxLocalDispatchResult result;

    g_assert_true(mux_local_connection_queue(pair.connection, packet, &error));
    g_assert_no_error(error);
    g_assert_cmpint(close(pair.peer_fd), ==, 0);
    pair.peer_fd = -1;

    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_HUP,
                                           NULL,
                                           NULL,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_CLOSED);
    g_assert_cmpint(mux_local_connection_get_fd(pair.connection), ==, -1);
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     0);
    g_assert_false((mux_local_connection_wanted_condition(pair.connection) &
                    G_IO_OUT) != 0);

    g_assert_false(mux_local_connection_queue(pair.connection,
                                              packet,
                                              &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
    g_clear_error(&error);

    result = mux_local_connection_dispatch(pair.connection,
                                           0,
                                           NULL,
                                           NULL,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_CLOSED);

    test_pair_clear(&pair);
}

static void
test_oversized_datagram_closes_connection(void)
{
    static const guint8 oversized[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10,
    };
    g_autoptr(GBytes) oversized_packet =
        g_bytes_new(oversized, sizeof(oversized));
    g_autoptr(GBytes) empty_packet = g_bytes_new(NULL, 0);
    g_autoptr(GError) error = NULL;
    PacketCapture capture = packet_capture_new();
    TestPair pair = test_pair_new(16, 64);
    MuxLocalDispatchResult result;

    g_assert_false(mux_local_connection_queue(pair.connection,
                                              empty_packet,
                                              &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);

    g_assert_false(mux_local_connection_queue(pair.connection,
                                              oversized_packet,
                                              &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_clear_error(&error);

    g_assert_cmpint(send(pair.peer_fd,
                         oversized,
                         sizeof(oversized),
                         0),
                    ==,
                    (ssize_t) sizeof(oversized));
    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_IN,
                                           capture_packet,
                                           &capture,
                                           &error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_ERROR);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_assert_cmpuint(capture.packets->len, ==, 0);
    g_assert_cmpint(mux_local_connection_get_fd(pair.connection), ==, -1);

    packet_capture_clear(&capture);
    test_pair_clear(&pair);
}

static void
test_callback_rejection_is_protocol_error(void)
{
    static const guint8 payload[] = { 'r', 'e', 'j', 'e', 'c', 't' };
    g_autoptr(GError) error = NULL;
    PacketCapture capture = packet_capture_new();
    TestPair pair = test_pair_new(64, 256);
    MuxLocalDispatchResult result;

    capture.reject = TRUE;
    g_assert_cmpint(send(pair.peer_fd, payload, sizeof(payload), 0),
                    ==,
                    (ssize_t) sizeof(payload));

    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_IN,
                                           capture_packet,
                                           &capture,
                                           &error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_ERROR);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpuint(capture.packets->len, ==, 1);
    assert_bytes_equal(g_ptr_array_index(capture.packets, 0),
                       payload,
                       sizeof(payload));
    g_assert_cmpint(mux_local_connection_get_fd(pair.connection), ==, -1);

    packet_capture_clear(&capture);
    test_pair_clear(&pair);
}

static void
test_dispatch_flushes_and_queues_callback_reply(void)
{
    static const guint8 queued[] = { 't', 'i', 'c', 'k' };
    static const guint8 request[] = { 'r', 'e', 'q', 'u', 'e', 's', 't' };
    static const guint8 reply[] = { 'r', 'e', 'p', 'l', 'y' };
    g_autoptr(GBytes) queued_packet = g_bytes_new(queued, sizeof(queued));
    g_autoptr(GBytes) reply_packet = g_bytes_new(reply, sizeof(reply));
    g_autoptr(GError) error = NULL;
    PacketCapture capture = packet_capture_new();
    TestPair pair = test_pair_new(64, 256);
    MuxLocalDispatchResult result;

    g_assert_true(mux_local_connection_queue(pair.connection,
                                             queued_packet,
                                             &error));
    g_assert_no_error(error);

    result = mux_local_connection_dispatch(pair.connection,
                                           0,
                                           NULL,
                                           NULL,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    assert_peer_packet(pair.peer_fd, queued, sizeof(queued));

    capture.reply = reply_packet;
    g_assert_cmpint(send(pair.peer_fd, request, sizeof(request), 0),
                    ==,
                    (ssize_t) sizeof(request));
    result = mux_local_connection_dispatch(pair.connection,
                                           G_IO_IN,
                                           capture_packet,
                                           &capture,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    g_assert_cmpuint(capture.packets->len, ==, 1);
    assert_bytes_equal(g_ptr_array_index(capture.packets, 0),
                       request,
                       sizeof(request));
    assert_peer_packet(pair.peer_fd, reply, sizeof(reply));
    g_assert_cmpuint(mux_local_connection_get_queued_bytes(pair.connection),
                     ==,
                     0);

    result = mux_local_connection_dispatch(pair.connection,
                                           0,
                                           capture_packet,
                                           &capture,
                                           &error);
    g_assert_no_error(error);
    g_assert_cmpint(result, ==, MUX_LOCAL_DISPATCH_OK);
    g_assert_cmpuint(capture.packets->len, ==, 1);

    packet_capture_clear(&capture);
    test_pair_clear(&pair);
}

static gint
bind_test_socket(const gchar *path)
{
    struct sockaddr_un address = { 0 };
    gint fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    g_assert_cmpint(fd, >=, 0);
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    g_assert_cmpint(bind(fd, (const struct sockaddr *)&address,
                         sizeof(address)), ==, 0);
    return fd;
}

static void
test_listener_lock_lifetime(void)
{
    g_autoptr(GError) error = NULL;
    MuxLocalListener *listener = mux_local_listener_new("lifetime", 1, &error);
    MuxLocalListener *retained;
    MuxLocalListener *contender;

    g_assert_no_error(error);
    g_assert_nonnull(listener);
    retained = mux_local_listener_ref(listener);
    mux_local_listener_free(listener);
    contender = mux_local_listener_new("lifetime", 1, &error);
    g_assert_null(contender);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE);
    g_clear_error(&error);
    mux_local_listener_unref(retained);
    contender = mux_local_listener_new("lifetime", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(contender);
    mux_local_listener_free(contender);
}

static void
test_listener_reclaims_stale_socket(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = mux_local_transport_socket_path("stale", &error);
    MuxLocalListener *listener;
    gint fd;

    g_assert_no_error(error);
    fd = bind_test_socket(path);
    close(fd);
    listener = mux_local_listener_new("stale", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(listener);
    mux_local_listener_free(listener);
}

static void
test_listener_preserves_replacement(void)
{
    g_autoptr(GError) error = NULL;
    MuxLocalListener *listener = mux_local_listener_new("replace", 1, &error);
    g_autofree gchar *path = NULL;
    struct stat replacement;
    struct stat remaining;
    gint fd;

    g_assert_no_error(error);
    g_assert_nonnull(listener);
    path = g_strdup(mux_local_listener_get_path(listener));
    g_assert_cmpint(g_unlink(path), ==, 0);
    fd = bind_test_socket(path);
    g_assert_cmpint(lstat(path, &replacement), ==, 0);
    mux_local_listener_free(listener);
    g_assert_cmpint(lstat(path, &remaining), ==, 0);
    g_assert_cmpuint(remaining.st_dev, ==, replacement.st_dev);
    g_assert_cmpuint(remaining.st_ino, ==, replacement.st_ino);
    close(fd);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void
test_listener_respects_starting_owner(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = mux_local_transport_socket_path("starting", &error);
    g_autofree gchar *lock_path = g_strconcat(path, ".lock", NULL);
    MuxLocalListener *contender;
    struct stat before;
    struct stat after;
    gint lock_fd;
    gint socket_fd;

    g_assert_no_error(error);
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    g_assert_cmpint(lock_fd, >=, 0);
    g_assert_cmpint(flock(lock_fd, LOCK_EX | LOCK_NB), ==, 0);
    socket_fd = bind_test_socket(path);
    g_assert_cmpint(lstat(path, &before), ==, 0);

    /* The first process owns the lock and has bound but not listened yet. */
    contender = mux_local_listener_new("starting", 1, &error);
    g_assert_null(contender);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE);
    g_assert_cmpint(lstat(path, &after), ==, 0);
    g_assert_cmpuint(before.st_ino, ==, after.st_ino);
    close(socket_fd);
    close(lock_fd);

    g_clear_error(&error);
    contender = mux_local_listener_new("starting", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(contender);
    mux_local_listener_free(contender);
}

int
main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *runtime = g_dir_make_tmp("mux-transport-XXXXXX", &error);
    g_autofree gchar *directory = NULL;
    g_autoptr(GDir) entries = NULL;
    const gchar *name;
    gint result;

    g_assert_no_error(error);
    g_assert_true(g_setenv("XDG_RUNTIME_DIR", runtime, TRUE));
    directory = g_build_filename(runtime, "mux", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/local-transport/send-receive-datagrams",
                    test_send_receive_preserves_datagrams);
    g_test_add_func("/local-transport/queue-backpressure",
                    test_queue_backpressure_and_drain);
    g_test_add_func("/local-transport/peer-close",
                    test_peer_close_clears_queue);
    g_test_add_func("/local-transport/oversized-datagram",
                    test_oversized_datagram_closes_connection);
    g_test_add_func("/local-transport/callback-rejection",
                    test_callback_rejection_is_protocol_error);
    g_test_add_func("/local-transport/dispatch-semantics",
                    test_dispatch_flushes_and_queues_callback_reply);
    g_test_add_func("/local-transport/listener-lock-lifetime",
                    test_listener_lock_lifetime);
    g_test_add_func("/local-transport/listener-stale-socket",
                    test_listener_reclaims_stale_socket);
    g_test_add_func("/local-transport/listener-preserves-replacement",
                    test_listener_preserves_replacement);
    g_test_add_func("/local-transport/listener-starting-owner",
                    test_listener_respects_starting_owner);

    result = g_test_run();
    entries = g_dir_open(directory, 0, &error);
    g_assert_no_error(error);
    while ((name = g_dir_read_name(entries)) != NULL) {
        g_autofree gchar *entry = g_build_filename(directory, name, NULL);

        g_assert_cmpint(g_unlink(entry), ==, 0);
    }
    g_clear_pointer(&entries, g_dir_close);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(runtime), ==, 0);
    return result;
}
