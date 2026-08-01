#include "../src/mux-engine-protocol.h"

#include <glib.h>

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
put_u16(guint8 *target, guint16 value)
{
    target[0] = (guint8)(value >> 8);
    target[1] = (guint8)value;
}

static void
put_u32(guint8 *target, guint32 value)
{
    target[0] = (guint8)(value >> 24);
    target[1] = (guint8)(value >> 16);
    target[2] = (guint8)(value >> 8);
    target[3] = (guint8)value;
}

static void
init_header(guint8 *packet)
{
    memset(packet, 0, MUX_ENGINE_HEADER_SIZE);
    put_u32(packet, MUX_ENGINE_MAGIC);
    put_u16(packet + 4, MUX_ENGINE_VERSION);
    put_u16(packet + 6, MUX_ENGINE_MESSAGE_PING);
}

static void
open_socket_pair(int sockets[2])
{
    g_assert_cmpint(socketpair(AF_UNIX,
                               SOCK_SEQPACKET | SOCK_CLOEXEC,
                               0,
                               sockets),
                    ==,
                    0);
}

static void
close_socket_pair(int sockets[2])
{
    g_assert_cmpint(close(sockets[0]), ==, 0);
    g_assert_cmpint(close(sockets[1]), ==, 0);
}

static void
assert_protocol_rejected(const guint8 *packet, gsize packet_size)
{
    int sockets[2];
    ssize_t sent;
    MuxEngineMessage message = {0};
    g_autoptr(GError) error = NULL;

    open_socket_pair(sockets);
    sent = send(sockets[0], packet, packet_size, 0);
    g_assert_cmpint(sent, ==, (ssize_t)packet_size);

    g_assert_false(mux_engine_receive_message(sockets[1], &message, &error));
    g_assert_error(error, MUX_ENGINE_ERROR, MUX_ENGINE_ERROR_PROTOCOL);
    g_assert_null(message.payload);

    mux_engine_message_clear(&message);
    close_socket_pair(sockets);
}

static void
test_builder_cursor_round_trip(void)
{
    static const guint8 raw[] = {0x00, 0x7f, 0xff};
    static const guint8 expected[] = {
        0x12, 0x34,
        0x89, 0xab, 0xcd, 0xef,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x00, 0x7f, 0xff,
        0x00, 0x00, 0x00, 0x03, 'm', 'u', 'x',
        0x00, 0x00, 0x00, 0x00,
    };
    MuxEngineBuilder builder = {0};
    MuxEngineCursor cursor;
    guint16 value16 = 0;
    guint32 value32 = 0;
    guint64 value64 = 0;
    const guint8 *decoded_raw = NULL;
    const guint8 *encoded;
    gsize encoded_size;
    g_autofree gchar *text = NULL;
    g_autofree gchar *empty = NULL;
    g_autoptr(GBytes) payload = NULL;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u16(&builder, 0x1234);
    mux_engine_builder_put_u32(&builder, 0x89abcdef);
    mux_engine_builder_put_u64(&builder,
                               G_GUINT64_CONSTANT(0x0123456789abcdef));
    mux_engine_builder_put_bytes(&builder, raw, sizeof(raw));
    mux_engine_builder_put_string(&builder, "mux");
    mux_engine_builder_put_string(&builder, "");
    payload = mux_engine_builder_finish(&builder);

    encoded = g_bytes_get_data(payload, &encoded_size);
    g_assert_cmpuint(encoded_size, ==, sizeof(expected));
    g_assert_cmpmem(encoded, encoded_size, expected, sizeof(expected));

    mux_engine_cursor_init(&cursor, payload);
    g_assert_true(mux_engine_cursor_get_u16(&cursor, &value16));
    g_assert_cmpuint(value16, ==, 0x1234);
    g_assert_true(mux_engine_cursor_get_u32(&cursor, &value32));
    g_assert_cmpuint(value32, ==, 0x89abcdef);
    g_assert_true(mux_engine_cursor_get_u64(&cursor, &value64));
    g_assert_cmpuint(value64,
                     ==,
                     G_GUINT64_CONSTANT(0x0123456789abcdef));
    g_assert_true(mux_engine_cursor_get_bytes(&cursor,
                                              sizeof(raw),
                                              &decoded_raw));
    g_assert_cmpmem(decoded_raw, sizeof(raw), raw, sizeof(raw));
    g_assert_true(mux_engine_cursor_get_string(&cursor, &text));
    g_assert_cmpstr(text, ==, "mux");
    g_assert_true(mux_engine_cursor_get_string(&cursor, &empty));
    g_assert_cmpstr(empty, ==, "");
    g_assert_true(mux_engine_cursor_done(&cursor));

    mux_engine_builder_clear(&builder);
}

static void
test_cursor_rejects_embedded_nul(void)
{
    static const guint8 encoded[] = {
        0x00, 0x00, 0x00, 0x03, 'a', 0x00, 'b',
    };
    g_autoptr(GBytes) payload = g_bytes_new_static(encoded, sizeof(encoded));
    MuxEngineCursor cursor;
    g_autofree gchar *value = NULL;

    mux_engine_cursor_init(&cursor, payload);
    g_assert_false(mux_engine_cursor_get_string(&cursor, &value));
    g_assert_null(value);
}

static void
test_cursor_rejects_truncated_string(void)
{
    static const guint8 encoded[] = {
        0x00, 0x00, 0x00, 0x04, 'm', 'u',
    };
    g_autoptr(GBytes) payload = g_bytes_new_static(encoded, sizeof(encoded));
    MuxEngineCursor cursor;
    g_autofree gchar *value = NULL;

    mux_engine_cursor_init(&cursor, payload);
    g_assert_false(mux_engine_cursor_get_string(&cursor, &value));
    g_assert_null(value);
}

static void
test_cursor_detects_trailing_data(void)
{
    static const guint8 encoded[] = {
        0x00, 0x00, 0x00, 0x03, 'm', 'u', 'x', 0xff,
    };
    g_autoptr(GBytes) payload = g_bytes_new_static(encoded, sizeof(encoded));
    MuxEngineCursor cursor;
    g_autofree gchar *value = NULL;
    const guint8 *trailing = NULL;

    mux_engine_cursor_init(&cursor, payload);
    g_assert_true(mux_engine_cursor_get_string(&cursor, &value));
    g_assert_cmpstr(value, ==, "mux");
    g_assert_false(mux_engine_cursor_done(&cursor));
    g_assert_true(mux_engine_cursor_get_bytes(&cursor, 1, &trailing));
    g_assert_cmphex(trailing[0], ==, 0xff);
    g_assert_true(mux_engine_cursor_done(&cursor));
}

static void
test_packet_round_trip(void)
{
    static const guint8 payload_data[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    int sockets[2];
    g_autoptr(GBytes) payload =
        g_bytes_new_static(payload_data, sizeof(payload_data));
    MuxEngineMessage outgoing = {0};
    MuxEngineMessage incoming = {0};
    g_autoptr(GError) error = NULL;

    open_socket_pair(sockets);
    mux_engine_message_init(&outgoing,
                            MUX_ENGINE_MESSAGE_EXTENSION,
                            MUX_ENGINE_FLAG_FULL_DAMAGE |
                                MUX_ENGINE_FLAG_EPHEMERAL,
                            G_GUINT64_CONSTANT(0x0123456789abcdef),
                            G_GUINT64_CONSTANT(0xfedcba9876543210),
                            payload);

    g_assert_true(mux_engine_send_message(sockets[0], &outgoing, &error));
    g_assert_no_error(error);
    g_assert_true(mux_engine_receive_message(sockets[1], &incoming, &error));
    g_assert_no_error(error);

    g_assert_cmpuint(incoming.type, ==, MUX_ENGINE_MESSAGE_EXTENSION);
    g_assert_cmpuint(incoming.flags,
                     ==,
                     MUX_ENGINE_FLAG_FULL_DAMAGE | MUX_ENGINE_FLAG_EPHEMERAL);
    g_assert_cmpuint(incoming.view_id,
                     ==,
                     G_GUINT64_CONSTANT(0x0123456789abcdef));
    g_assert_cmpuint(incoming.serial,
                     ==,
                     G_GUINT64_CONSTANT(0xfedcba9876543210));
    g_assert_true(g_bytes_equal(incoming.payload, payload));

    mux_engine_message_clear(&incoming);
    mux_engine_message_clear(&outgoing);
    close_socket_pair(sockets);
}

static void
test_close_handshake_round_trip(void)
{
    static const guint16 types[] = {
        MUX_ENGINE_MESSAGE_REQUEST_CLOSE,
        MUX_ENGINE_MESSAGE_CLOSE_READY,
    };

    for (gsize index = 0; index < G_N_ELEMENTS(types); index++) {
        int sockets[2];
        g_autoptr(GBytes) payload = g_bytes_new(NULL, 0);
        MuxEngineMessage outgoing = {0};
        MuxEngineMessage incoming = {0};
        g_autoptr(GError) error = NULL;

        open_socket_pair(sockets);
        mux_engine_message_init(&outgoing,
                                types[index],
                                MUX_ENGINE_FLAG_NONE,
                                G_GUINT64_CONSTANT(0x1020304050607080),
                                G_GUINT64_CONSTANT(0x8877665544332211),
                                payload);

        g_assert_true(mux_engine_send_message(sockets[0],
                                              &outgoing,
                                              &error));
        g_assert_no_error(error);
        g_assert_true(mux_engine_receive_message(sockets[1],
                                                 &incoming,
                                                 &error));
        g_assert_no_error(error);
        g_assert_cmpuint(incoming.type, ==, types[index]);
        g_assert_cmpuint(incoming.flags, ==, MUX_ENGINE_FLAG_NONE);
        g_assert_cmpuint(incoming.view_id,
                         ==,
                         G_GUINT64_CONSTANT(0x1020304050607080));
        g_assert_cmpuint(incoming.serial,
                         ==,
                         G_GUINT64_CONSTANT(0x8877665544332211));
        g_assert_cmpuint(g_bytes_get_size(incoming.payload), ==, 0);

        mux_engine_message_clear(&incoming);
        mux_engine_message_clear(&outgoing);
        close_socket_pair(sockets);
    }
}

static void
test_visibility_round_trip(void)
{
    static const guint32 visibility_states[] = {0, 1};

    for (gsize index = 0;
         index < G_N_ELEMENTS(visibility_states);
         index++) {
        int sockets[2];
        MuxEngineBuilder builder = {0};
        g_autoptr(GBytes) payload = NULL;
        MuxEngineMessage outgoing = {0};
        MuxEngineMessage incoming = {0};
        MuxEngineCursor cursor;
        guint32 visible = G_MAXUINT32;
        g_autoptr(GError) error = NULL;

        mux_engine_builder_init(&builder);
        mux_engine_builder_put_u32(&builder,
                                   visibility_states[index]);
        payload = mux_engine_builder_finish(&builder);
        open_socket_pair(sockets);
        mux_engine_message_init(&outgoing,
                                MUX_ENGINE_MESSAGE_SET_VISIBILITY,
                                MUX_ENGINE_FLAG_NONE,
                                G_GUINT64_CONSTANT(0x1020304050607080),
                                G_GUINT64_CONSTANT(0x8877665544332211),
                                payload);

        g_assert_true(mux_engine_send_message(sockets[0],
                                              &outgoing,
                                              &error));
        g_assert_no_error(error);
        g_assert_true(mux_engine_receive_message(sockets[1],
                                                 &incoming,
                                                 &error));
        g_assert_no_error(error);
        g_assert_cmpuint(incoming.type,
                         ==,
                         MUX_ENGINE_MESSAGE_SET_VISIBILITY);
        g_assert_cmpuint(incoming.flags, ==, MUX_ENGINE_FLAG_NONE);
        g_assert_cmpuint(incoming.view_id,
                         ==,
                         G_GUINT64_CONSTANT(0x1020304050607080));
        g_assert_cmpuint(incoming.serial,
                         ==,
                         G_GUINT64_CONSTANT(0x8877665544332211));
        mux_engine_cursor_init(&cursor, incoming.payload);
        g_assert_true(mux_engine_cursor_get_u32(&cursor, &visible));
        g_assert_cmpuint(visible, ==, visibility_states[index]);
        g_assert_true(mux_engine_cursor_done(&cursor));

        mux_engine_message_clear(&incoming);
        mux_engine_message_clear(&outgoing);
        close_socket_pair(sockets);
    }
}

static void
test_cancel_close_round_trip(void)
{
    int sockets[2];
    MuxEngineBuilder builder = {0};
    g_autoptr(GBytes) payload = NULL;
    MuxEngineMessage outgoing = {0};
    MuxEngineMessage incoming = {0};
    MuxEngineCursor cursor;
    guint64 close_serial = 0;
    g_autoptr(GError) error = NULL;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u64(&builder,
                               G_GUINT64_CONSTANT(0x123456789abcdef0));
    payload = mux_engine_builder_finish(&builder);
    open_socket_pair(sockets);
    mux_engine_message_init(&outgoing,
                            MUX_ENGINE_MESSAGE_CANCEL_CLOSE,
                            MUX_ENGINE_FLAG_NONE,
                            G_GUINT64_CONSTANT(0x1020304050607080),
                            G_GUINT64_CONSTANT(0x8877665544332211),
                            payload);

    g_assert_true(mux_engine_send_message(sockets[0], &outgoing, &error));
    g_assert_no_error(error);
    g_assert_true(mux_engine_receive_message(sockets[1],
                                             &incoming,
                                             &error));
    g_assert_no_error(error);
    g_assert_cmpuint(incoming.type, ==, MUX_ENGINE_MESSAGE_CANCEL_CLOSE);
    mux_engine_cursor_init(&cursor, incoming.payload);
    g_assert_true(mux_engine_cursor_get_u64(&cursor, &close_serial));
    g_assert_cmpuint(close_serial,
                     ==,
                     G_GUINT64_CONSTANT(0x123456789abcdef0));
    g_assert_true(mux_engine_cursor_done(&cursor));

    mux_engine_message_clear(&incoming);
    mux_engine_message_clear(&outgoing);
    close_socket_pair(sockets);
}

static void
test_legacy_protocol_version_rejected(void)
{
    guint8 packet[MUX_ENGINE_HEADER_SIZE];

    g_assert_cmpuint(MUX_ENGINE_VERSION, ==, 2);
    init_header(packet);
    put_u16(packet + 4, 1);
    assert_protocol_rejected(packet, sizeof(packet));
}

typedef enum {
    MALFORMED_WRONG_MAGIC,
    MALFORMED_WRONG_VERSION,
    MALFORMED_ZERO_TYPE,
    MALFORMED_LENGTH_MISMATCH,
    MALFORMED_TRUNCATED_HEADER,
    MALFORMED_TRAILING_DATA,
} MalformedKind;

typedef struct {
    const gchar *path;
    MalformedKind kind;
} MalformedCase;

static void
test_malformed_packet(gconstpointer user_data)
{
    const MalformedCase *test_case = user_data;
    guint8 packet[MUX_ENGINE_HEADER_SIZE + 1];
    gsize packet_size = MUX_ENGINE_HEADER_SIZE;

    init_header(packet);
    switch (test_case->kind) {
    case MALFORMED_WRONG_MAGIC:
        put_u32(packet, MUX_ENGINE_MAGIC ^ 1u);
        break;
    case MALFORMED_WRONG_VERSION:
        put_u16(packet + 4, MUX_ENGINE_VERSION + 1u);
        break;
    case MALFORMED_ZERO_TYPE:
        put_u16(packet + 6, 0);
        break;
    case MALFORMED_LENGTH_MISMATCH:
        put_u32(packet + 12, 1);
        break;
    case MALFORMED_TRUNCATED_HEADER:
        packet_size--;
        break;
    case MALFORMED_TRAILING_DATA:
        packet[MUX_ENGINE_HEADER_SIZE] = 0xff;
        packet_size++;
        break;
    }

    assert_protocol_rejected(packet, packet_size);
}

int
main(int argc, char **argv)
{
    static const MalformedCase malformed_cases[] = {
        {"/engine-protocol/packet/wrong-magic", MALFORMED_WRONG_MAGIC},
        {"/engine-protocol/packet/wrong-version", MALFORMED_WRONG_VERSION},
        {"/engine-protocol/packet/zero-type", MALFORMED_ZERO_TYPE},
        {"/engine-protocol/packet/length-mismatch",
         MALFORMED_LENGTH_MISMATCH},
        {"/engine-protocol/packet/truncated-header",
         MALFORMED_TRUNCATED_HEADER},
        {"/engine-protocol/packet/trailing-data", MALFORMED_TRAILING_DATA},
    };
    gsize index;

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/engine-protocol/builder-cursor/round-trip",
                    test_builder_cursor_round_trip);
    g_test_add_func("/engine-protocol/cursor/embedded-nul",
                    test_cursor_rejects_embedded_nul);
    g_test_add_func("/engine-protocol/cursor/truncated-string",
                    test_cursor_rejects_truncated_string);
    g_test_add_func("/engine-protocol/cursor/trailing-data",
                    test_cursor_detects_trailing_data);
    g_test_add_func("/engine-protocol/packet/round-trip",
                    test_packet_round_trip);
    g_test_add_func("/engine-protocol/packet/close-handshake",
                    test_close_handshake_round_trip);
    g_test_add_func("/engine-protocol/packet/visibility",
                    test_visibility_round_trip);
    g_test_add_func("/engine-protocol/packet/cancel-close",
                    test_cancel_close_round_trip);
    g_test_add_func("/engine-protocol/packet/reject-v1",
                    test_legacy_protocol_version_rejected);

    for (index = 0; index < G_N_ELEMENTS(malformed_cases); index++) {
        g_test_add_data_func(malformed_cases[index].path,
                             &malformed_cases[index],
                             test_malformed_packet);
    }

    return g_test_run();
}
