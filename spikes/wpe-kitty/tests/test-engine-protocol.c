#include "../src/mux-engine-protocol.h"
#include "../src/mux-shortcuts.h"
#include "../mux-uri.h"

#include <gio/gio.h>
#include <glib.h>
#include <wpe/webkit.h>

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

gboolean mux_engine_test_prepare_initial_uri(gboolean popup_claim,
                                             const gchar *uri,
                                             const gchar *search_url,
                                             gchar **normalized_uri,
                                             GError **error);
gboolean mux_engine_test_view_capacity(guint active_views,
                                       guint pending_views);
guint mux_engine_test_frame_backpressure_retry_delay_ms(
    guint rejection_count);
WebKitNetworkSession *mux_engine_test_private_network_session_new(void);
gboolean mux_engine_test_idle_fallback_should_arm(gboolean had_owned_views,
                                                   guint active_views,
                                                   gboolean muxd_connected,
                                                   gboolean shutting_down);
guint mux_engine_test_logical_dimension(guint physical,
                                        guint scale_milli);
gdouble mux_engine_test_physical_milli_to_logical(gint32 physical_milli,
                                                   guint scale_milli);
guint mux_engine_test_find_shortcut(guint32 modifiers, guint32 keyval);
gboolean mux_pane_test_schedule_retry_at(gint64 now_us,
                                         gint64 *retry_us,
                                         guint *backoff_ms);
gboolean mux_pane_test_decode_engine_error(GBytes *payload,
                                           guint32 *code,
                                           gchar **safe_detail);
gint mux_pane_test_parse_kitty_graphics_response(const guint8 *sequence,
                                                 gsize length,
                                                 guint *image_id,
                                                 gchar **detail);
gchar *mux_pane_test_build_kitty_frame_command(gboolean image_present,
                                                guint32 message_flags,
                                                guint32 width,
                                                guint32 height,
                                                guint32 x,
                                                guint32 y,
                                                guint32 rectangle_width,
                                                guint32 rectangle_height,
                                                guint64 shm_size,
                                                guint image_id,
                                                const gchar *encoded_name);
gchar *mux_pane_test_find_overlay_label(const gchar *query,
                                        MuxEngineFindStatus status,
                                        guint matches,
                                        guint columns);

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
        MUX_ENGINE_MESSAGE_CLOSE_CANCELLED,
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
test_layer_round_trip(void)
{
    int sockets[2];
    MuxEngineBuilder builder = {0};
    g_autoptr(GBytes) payload = NULL;
    MuxEngineMessage outgoing = {0};
    MuxEngineMessage incoming = {0};
    MuxEngineCursor cursor;
    g_autofree gchar *layer = NULL;
    g_autoptr(GError) error = NULL;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_string(&builder, "research");
    payload = mux_engine_builder_finish(&builder);
    open_socket_pair(sockets);
    mux_engine_message_init(&outgoing,
                            MUX_ENGINE_MESSAGE_SET_LAYER,
                            MUX_ENGINE_FLAG_NONE,
                            42,
                            7,
                            payload);
    g_assert_true(mux_engine_send_message(sockets[0], &outgoing, &error));
    g_assert_no_error(error);
    g_assert_true(mux_engine_receive_message(sockets[1], &incoming, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(incoming.type, ==, MUX_ENGINE_MESSAGE_SET_LAYER);
    mux_engine_cursor_init(&cursor, incoming.payload);
    g_assert_true(mux_engine_cursor_get_string(&cursor, &layer));
    g_assert_cmpstr(layer, ==, "research");
    g_assert_true(mux_engine_cursor_done(&cursor));
    mux_engine_message_clear(&incoming);
    mux_engine_message_clear(&outgoing);
    close_socket_pair(sockets);
}

static void
test_frame_rejected_round_trip(void)
{
    int sockets[2];
    MuxEngineBuilder builder = {0};
    g_autoptr(GBytes) payload = NULL;
    MuxEngineMessage outgoing = {0};
    MuxEngineMessage incoming = {0};
    MuxEngineCursor cursor;
    guint32 reason = 0;
    g_autofree gchar *detail = NULL;
    g_autoptr(GError) error = NULL;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder,
                               MUX_ENGINE_FRAME_REJECTED_KITTY);
    mux_engine_builder_put_string(&builder, "EINVAL: bad image");
    payload = mux_engine_builder_finish(&builder);
    open_socket_pair(sockets);
    mux_engine_message_init(&outgoing,
                            MUX_ENGINE_MESSAGE_FRAME_REJECTED,
                            MUX_ENGINE_FLAG_NONE,
                            42,
                            99,
                            payload);
    g_assert_true(mux_engine_send_message(sockets[0], &outgoing, &error));
    g_assert_no_error(error);
    g_assert_true(mux_engine_receive_message(sockets[1], &incoming, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(incoming.type,
                     ==,
                     MUX_ENGINE_MESSAGE_FRAME_REJECTED);
    mux_engine_cursor_init(&cursor, incoming.payload);
    g_assert_true(mux_engine_cursor_get_u32(&cursor, &reason));
    g_assert_cmpuint(reason, ==, MUX_ENGINE_FRAME_REJECTED_KITTY);
    g_assert_true(mux_engine_cursor_get_string(&cursor, &detail));
    g_assert_cmpstr(detail, ==, "EINVAL: bad image");
    g_assert_true(mux_engine_cursor_done(&cursor));
    mux_engine_message_clear(&incoming);
    mux_engine_message_clear(&outgoing);
    close_socket_pair(sockets);
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

    g_assert_cmpuint(MUX_ENGINE_VERSION, ==, 3);
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

static void
test_create_view_initial_uri_preparation(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *normalized = NULL;

    g_assert_false(mux_engine_test_prepare_initial_uri(
        FALSE,
        "data:text/html,not-allowed",
        NULL,
        &normalized,
        &error));
    g_assert_error(error,
                   MUX_URI_ERROR,
                   MUX_URI_ERROR_DISALLOWED_SCHEME);
    g_assert_null(normalized);
    g_clear_error(&error);

    g_assert_true(mux_engine_test_prepare_initial_uri(FALSE,
                                                      "example.com",
                                                      NULL,
                                                      &normalized,
                                                      &error));
    g_assert_no_error(error);
    g_assert_cmpstr(normalized, ==, "https://example.com");
    g_clear_pointer(&normalized, g_free);

    g_assert_true(mux_engine_test_prepare_initial_uri(
        TRUE,
        "data:text/html,popup-uri-is-not-reloaded",
        NULL,
        &normalized,
        &error));
    g_assert_no_error(error);
    g_assert_null(normalized);
}

static void
test_pane_retry_backoff(void)
{
    static const guint expected_delays[] = {
        100, 200, 400, 800, 1600, 3200, 5000, 5000,
    };
    gint64 now_us = G_GINT64_CONSTANT(1000000);
    gint64 retry_us = 0;
    guint backoff_ms = 0;

    for (gsize index = 0;
         index < G_N_ELEMENTS(expected_delays);
         index++) {
        g_assert_true(mux_pane_test_schedule_retry_at(now_us,
                                                      &retry_us,
                                                      &backoff_ms));
        g_assert_cmpint(retry_us,
                        ==,
                        now_us +
                            (gint64)expected_delays[index] * 1000);
        g_assert_cmpuint(backoff_ms,
                         ==,
                         index + 1 < G_N_ELEMENTS(expected_delays)
                             ? expected_delays[index + 1]
                             : 5000);
        g_assert_false(mux_pane_test_schedule_retry_at(now_us + 1,
                                                       &retry_us,
                                                       &backoff_ms));
        retry_us = 0;
        now_us += G_GINT64_CONSTANT(10000000);
    }
}

static void
test_engine_error_payload_validation(void)
{
    MuxEngineBuilder builder = { 0 };
    g_autoptr(GBytes) payload = NULL;
    g_autofree gchar *safe_detail = NULL;
    guint32 code = 0;
    guint8 invalid_utf8[9] = { 0 };
    g_autoptr(GBytes) invalid_payload = NULL;
    g_autofree gchar *oversized = g_strnfill(4097, 'x');
    g_autoptr(GBytes) oversized_payload = NULL;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder,
                               MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE);
    mux_engine_builder_put_string(
        &builder,
        "bad\033[31m\nvalue\xE2\x80\xAE");
    payload = mux_engine_builder_finish(&builder);
    g_assert_true(mux_pane_test_decode_engine_error(payload,
                                                    &code,
                                                    &safe_detail));
    g_assert_cmpuint(code, ==, MUX_ENGINE_REMOTE_ERROR_BAD_MESSAGE);
    g_assert_cmpstr(safe_detail, ==, "bad?[31m?value?");
    g_assert_null(strchr(safe_detail, '\033'));
    g_assert_null(strchr(safe_detail, '\n'));

    put_u32(invalid_utf8, MUX_ENGINE_REMOTE_ERROR_INTERNAL);
    put_u32(invalid_utf8 + 4, 1);
    invalid_utf8[8] = 0xff;
    invalid_payload = g_bytes_new(invalid_utf8, sizeof(invalid_utf8));
    g_clear_pointer(&safe_detail, g_free);
    g_assert_false(mux_pane_test_decode_engine_error(invalid_payload,
                                                     &code,
                                                     &safe_detail));
    g_assert_null(safe_detail);

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, MUX_ENGINE_REMOTE_ERROR_INTERNAL);
    mux_engine_builder_put_string(&builder, oversized);
    oversized_payload = mux_engine_builder_finish(&builder);
    g_assert_false(mux_pane_test_decode_engine_error(oversized_payload,
                                                     &code,
                                                     &safe_detail));
    g_assert_null(safe_detail);
}

static void
test_kitty_graphics_response_classification(void)
{
    static const guint8 success[] = "\033_Ga=T,i=42;OK\033\\";
    static const guint8 failure[] =
        "\033_Gi=42;EINVAL:\033[31mbad image\033\\";
    static const guint8 missing_id[] = "\033_Ga=T;OK\033\\";
    guint image_id = 0;
    g_autofree gchar *detail = NULL;

    g_assert_cmpint(mux_pane_test_parse_kitty_graphics_response(
                        success,
                        sizeof(success) - 1,
                        &image_id,
                        &detail),
                    ==,
                    1);
    g_assert_cmpuint(image_id, ==, 42);
    g_assert_null(detail);

    image_id = 0;
    g_assert_cmpint(mux_pane_test_parse_kitty_graphics_response(
                        failure,
                        sizeof(failure) - 1,
                        &image_id,
                        &detail),
                    ==,
                    2);
    g_assert_cmpuint(image_id, ==, 42);
    g_assert_cmpstr(detail, ==, "EINVAL:");
    g_clear_pointer(&detail, g_free);

    g_assert_cmpint(mux_pane_test_parse_kitty_graphics_response(
                        missing_id,
                        sizeof(missing_id) - 1,
                        &image_id,
                        &detail),
                    ==,
                    0);
    g_assert_null(detail);
}

static void
test_kitty_frame_trusted_overlay_layering(void)
{
    static const gchar encoded_name[] = "L211eC1mcmFtZQ==";
    static const gchar full_expected[] =
        "\033[H\033_Ga=T,f=32,t=s,s=800,v=600,S=1920000,"
        "i=77,z=-1,q=0,C=1;L211eC1mcmFtZQ==\033\\";
    static const gchar partial_expected[] =
        "\033_Ga=f,f=32,t=s,s=30,v=40,S=4800,i=77,r=1,"
        "x=10,y=20,X=1,q=0;L211eC1mcmFtZQ==\033\\";
    g_autofree gchar *initial = NULL;
    g_autofree gchar *after_resize = NULL;
    g_autofree gchar *replacement = NULL;
    g_autofree gchar *partial = NULL;

    initial = mux_pane_test_build_kitty_frame_command(
        FALSE,
        MUX_ENGINE_FLAG_NONE,
        800,
        600,
        0,
        0,
        800,
        600,
        1920000,
        77,
        encoded_name);
    g_assert_cmpstr(initial, ==, full_expected);

    /* delete_image() clears image_present before a resized frame arrives. */
    after_resize = mux_pane_test_build_kitty_frame_command(
        FALSE,
        MUX_ENGINE_FLAG_NONE,
        800,
        600,
        0,
        0,
        800,
        600,
        1920000,
        77,
        encoded_name);
    g_assert_cmpstr(after_resize, ==, full_expected);

    replacement = mux_pane_test_build_kitty_frame_command(
        TRUE,
        MUX_ENGINE_FLAG_FULL_DAMAGE,
        800,
        600,
        0,
        0,
        800,
        600,
        1920000,
        77,
        encoded_name);
    g_assert_cmpstr(replacement, ==, full_expected);

    partial = mux_pane_test_build_kitty_frame_command(
        TRUE,
        MUX_ENGINE_FLAG_NONE,
        800,
        600,
        10,
        20,
        30,
        40,
        4800,
        77,
        encoded_name);
    g_assert_cmpstr(partial, ==, partial_expected);
    g_assert_null(strstr(partial, ",z="));
}

static void
test_engine_global_view_capacity(void)
{
    g_assert_true(mux_engine_test_view_capacity(0, 0));
    g_assert_true(mux_engine_test_view_capacity(31, 0));
    g_assert_false(mux_engine_test_view_capacity(31, 1));
    g_assert_false(mux_engine_test_view_capacity(0, 32));
    g_assert_false(mux_engine_test_view_capacity(32, 0));
    g_assert_false(mux_engine_test_view_capacity(G_MAXUINT, 1));
}

static void
test_private_network_sessions_are_distinct_and_ephemeral(void)
{
    g_autoptr(WebKitNetworkSession) first =
        mux_engine_test_private_network_session_new();
    g_autoptr(WebKitNetworkSession) second =
        mux_engine_test_private_network_session_new();

    g_assert_nonnull(first);
    g_assert_nonnull(second);
    g_assert_true(first != second);
    g_assert_true(webkit_network_session_is_ephemeral(first));
    g_assert_true(webkit_network_session_is_ephemeral(second));
    g_assert_true(webkit_network_session_get_itp_enabled(first));
    g_assert_true(webkit_network_session_get_itp_enabled(second));
    g_assert_false(
        webkit_network_session_get_persistent_credential_storage_enabled(
            first));
    g_assert_false(
        webkit_network_session_get_persistent_credential_storage_enabled(
            second));
}

static void
test_engine_idle_fallback_policy(void)
{
    g_assert_false(mux_engine_test_idle_fallback_should_arm(FALSE,
                                                            0,
                                                            FALSE,
                                                            FALSE));
    g_assert_true(mux_engine_test_idle_fallback_should_arm(TRUE,
                                                           0,
                                                           FALSE,
                                                           FALSE));
    g_assert_false(mux_engine_test_idle_fallback_should_arm(TRUE,
                                                            1,
                                                            FALSE,
                                                            FALSE));
    g_assert_false(mux_engine_test_idle_fallback_should_arm(TRUE,
                                                            0,
                                                            TRUE,
                                                            FALSE));
    g_assert_false(mux_engine_test_idle_fallback_should_arm(TRUE,
                                                            0,
                                                            FALSE,
                                                            TRUE));
}

static void
test_frame_backpressure_retry_is_bounded(void)
{
    g_assert_cmpuint(mux_engine_test_frame_backpressure_retry_delay_ms(0),
                     ==,
                     50);
    g_assert_cmpuint(mux_engine_test_frame_backpressure_retry_delay_ms(1),
                     ==,
                     50);
    g_assert_cmpuint(mux_engine_test_frame_backpressure_retry_delay_ms(2),
                     ==,
                     100);
    g_assert_cmpuint(mux_engine_test_frame_backpressure_retry_delay_ms(5),
                     ==,
                     800);
    g_assert_cmpuint(mux_engine_test_frame_backpressure_retry_delay_ms(6),
                     ==,
                     1000);
    g_assert_cmpuint(
        mux_engine_test_frame_backpressure_retry_delay_ms(G_MAXUINT),
        ==,
        1000);
}

static void
test_device_scale_configuration(void)
{
    static const struct {
        const gchar *value;
        guint expected;
    } valid[] = {
        { NULL, 1000 },
        { "0.500", 500 },
        { "1", 1000 },
        { "1.25", 1250 },
        { "2.000", 2000 },
        { "4", 4000 },
    };
    static const gchar *invalid[] = {
        "", ".5", "1.", " 1", "1 ", "+1", "-1", "1e0",
        "0.499", "4.001", "1.0000", "nan", "inf",
    };

    for (guint index = 0; index < G_N_ELEMENTS(valid); index++) {
        g_autoptr(GError) error = NULL;
        guint32 parsed = 0;

        g_assert_true(mux_engine_parse_device_scale(valid[index].value,
                                                    &parsed,
                                                    &error));
        g_assert_no_error(error);
        g_assert_cmpuint(parsed, ==, valid[index].expected);
    }
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        g_autoptr(GError) error = NULL;
        guint32 parsed = 0;

        g_assert_false(mux_engine_parse_device_scale(invalid[index],
                                                     &parsed,
                                                     &error));
        g_assert_error(error,
                       G_IO_ERROR,
                       G_IO_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_device_scale_geometry(void)
{
    g_assert_cmpuint(mux_engine_test_logical_dimension(1920, 1000),
                     ==,
                     1920);
    g_assert_cmpuint(mux_engine_test_logical_dimension(1920, 2000),
                     ==,
                     960);
    g_assert_cmpuint(mux_engine_test_logical_dimension(1919, 1250),
                     ==,
                     1535);
    g_assert_cmpuint(mux_engine_test_logical_dimension(1, 4000), ==, 1);
    g_assert_cmpfloat(
        mux_engine_test_physical_milli_to_logical(2000000, 2000),
        ==,
        1000.0);
    g_assert_cmpfloat(
        mux_engine_test_physical_milli_to_logical(-80000, 2000),
        ==,
        -40.0);
}

static void
test_find_shortcuts_and_trusted_label(void)
{
    const guint control = WPE_MODIFIER_KEYBOARD_CONTROL;
    const guint shift = WPE_MODIFIER_KEYBOARD_SHIFT;
    const guint alt = WPE_MODIFIER_KEYBOARD_ALT;
    const guint meta = WPE_MODIFIER_KEYBOARD_META;
    g_autofree gchar *label = NULL;
    g_autofree gchar *minimum = NULL;
    g_autofree gchar *zero = NULL;

    g_assert_cmpuint(mux_engine_test_find_shortcut(meta, 'f'), ==, 1);
    g_assert_cmpuint(mux_engine_test_find_shortcut(control, 'F'), ==, 1);
    g_assert_cmpuint(mux_engine_test_find_shortcut(meta, 'g'), ==, 2);
    g_assert_cmpuint(mux_engine_test_find_shortcut(meta | shift, 'g'),
                     ==,
                     3);
    g_assert_cmpuint(mux_engine_test_find_shortcut(control | shift, 'G'),
                     ==,
                     3);
    g_assert_cmpuint(mux_engine_test_find_shortcut(meta | alt, 'f'),
                     ==,
                     0);
    g_assert_cmpuint(mux_engine_test_find_shortcut(shift, 'g'), ==, 0);

    label = mux_pane_test_find_overlay_label(
        "site\033[31m\nquery",
        MUX_ENGINE_FIND_FOUND,
        2,
        32);
    g_assert_nonnull(strstr(label, "MUX FIND"));
    g_assert_nonnull(strstr(label, "2 matches"));
    g_assert_null(strchr(label, '\033'));
    g_assert_null(strchr(label, '\n'));
    g_assert_cmpuint(g_utf8_strlen(label, -1), <=, 32);
    g_assert_null(strstr(label, "site?[31m?query"));

    minimum = mux_pane_test_find_overlay_label(
        "discarded before status",
        MUX_ENGINE_FIND_FOUND,
        2,
        1);
    g_assert_cmpstr(minimum, ==, "2");
    zero = mux_pane_test_find_overlay_label(
        "discarded before status",
        MUX_ENGINE_FIND_NOT_FOUND,
        0,
        0);
    g_assert_cmpstr(zero, ==, "");
}

static void
test_shortcut_exact_modifier_matching(void)
{
    const guint control = MUX_SHORTCUT_MODIFIER_CONTROL;
    const guint shift = MUX_SHORTCUT_MODIFIER_SHIFT;
    const guint alt = MUX_SHORTCUT_MODIFIER_ALT;
    const guint meta = MUX_SHORTCUT_MODIFIER_META;

    g_assert_cmpuint(mux_shortcut_match_pane(meta, 'q'),
                     ==,
                     MUX_SHORTCUT_CLOSE);
    g_assert_cmpuint(mux_shortcut_match_pane(control, 'q'),
                     ==,
                     MUX_SHORTCUT_CLOSE);
    g_assert_cmpuint(mux_shortcut_match_pane(meta | shift, 'q'),
                     ==,
                     MUX_SHORTCUT_NONE);
    g_assert_cmpuint(mux_shortcut_match_pane(control | alt, 'q'),
                     ==,
                     MUX_SHORTCUT_NONE);
    g_assert_cmpuint(mux_shortcut_match_pane(meta | shift, 'v'),
                     ==,
                     MUX_SHORTCUT_CLIPBOARD_HISTORY);
    g_assert_cmpuint(mux_shortcut_match_pane(control | shift, 'v'),
                     ==,
                     MUX_SHORTCUT_CLIPBOARD_HISTORY);
    g_assert_cmpuint(mux_shortcut_match_pane(meta | shift | alt, 'v'),
                     ==,
                     MUX_SHORTCUT_NONE);

    g_assert_cmpuint(mux_shortcut_match_engine(meta | shift, 'p'),
                     ==,
                     MUX_SHORTCUT_COMMAND_PALETTE);
    g_assert_cmpuint(mux_shortcut_match_engine(control | shift, 'p'),
                     ==,
                     MUX_SHORTCUT_COMMAND_PALETTE);
    g_assert_cmpuint(mux_shortcut_match_engine(meta, 'd'),
                     ==,
                     MUX_SHORTCUT_BOOKMARK);
    g_assert_cmpuint(mux_shortcut_match_engine(control, 'd'),
                     ==,
                     MUX_SHORTCUT_BOOKMARK);
    g_assert_cmpuint(mux_shortcut_match_engine(meta | shift, 'd'),
                     ==,
                     MUX_SHORTCUT_NONE);
    g_assert_cmpuint(mux_shortcut_match_engine(alt, 0xff51u),
                     ==,
                     MUX_SHORTCUT_HISTORY_BACK);
    g_assert_cmpuint(mux_shortcut_match_engine(alt | shift, 0xff51u),
                     ==,
                     MUX_SHORTCUT_NONE);

    g_assert_cmpuint(mux_shortcut_match_bar(meta, 'l'),
                     ==,
                     MUX_SHORTCUT_LOCATION);
    g_assert_cmpuint(mux_shortcut_match_bar(control, 'l'),
                     ==,
                     MUX_SHORTCUT_LOCATION);
    g_assert_cmpuint(mux_shortcut_match_bar(meta, 'u'),
                     ==,
                     MUX_SHORTCUT_BAR_CLEAR);
    g_assert_cmpuint(mux_shortcut_match_bar(meta | alt, 'u'),
                     ==,
                     MUX_SHORTCUT_NONE);
}

static void
test_shortcut_lifecycle_and_unmatched_meta(void)
{
    MuxShortcut close = mux_shortcut_match_pane(
        MUX_SHORTCUT_MODIFIER_META,
        'q');
    gboolean execute = FALSE;

    g_assert_true(mux_shortcut_handle_event(close,
                                            MUX_SHORTCUT_EVENT_PRESS,
                                            &execute));
    g_assert_true(execute);
    g_assert_true(mux_shortcut_handle_event(close,
                                            MUX_SHORTCUT_EVENT_REPEAT,
                                            &execute));
    g_assert_false(execute);
    g_assert_true(mux_shortcut_handle_event(close,
                                            MUX_SHORTCUT_EVENT_RELEASE,
                                            &execute));
    g_assert_false(execute);

    g_assert_cmpuint(mux_shortcut_match_pane(
                         MUX_SHORTCUT_MODIFIER_META,
                         'x'),
                     ==,
                     MUX_SHORTCUT_NONE);
    g_assert_false(mux_shortcut_handle_event(MUX_SHORTCUT_NONE,
                                             MUX_SHORTCUT_EVENT_PRESS,
                                             &execute));
    g_assert_false(execute);
    g_assert_true(mux_shortcut_is_page_paste(
        MUX_SHORTCUT_MODIFIER_META,
        'v'));
    g_assert_true(mux_shortcut_is_page_paste(
        MUX_SHORTCUT_MODIFIER_CONTROL,
        'v'));
    g_assert_false(mux_shortcut_is_page_paste(
        MUX_SHORTCUT_MODIFIER_META | MUX_SHORTCUT_MODIFIER_ALT,
        'v'));

    g_assert_cmpuint(mux_shortcut_modifiers_from_kitty(8 + 1),
                     ==,
                     MUX_SHORTCUT_MODIFIER_META);
    g_assert_cmpuint(mux_shortcut_modifiers_from_kitty(32 + 1),
                     ==,
                     MUX_SHORTCUT_MODIFIER_META);
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
    g_test_add_func("/engine-protocol/packet/layer",
                    test_layer_round_trip);
    g_test_add_func("/engine-protocol/packet/frame-rejected",
                    test_frame_rejected_round_trip);
    g_test_add_func("/engine-protocol/packet/cancel-close",
                    test_cancel_close_round_trip);
    g_test_add_func("/engine-protocol/packet/reject-v1",
                    test_legacy_protocol_version_rejected);
    g_test_add_func("/engine-runtime/create-view/prepare-initial-uri",
                    test_create_view_initial_uri_preparation);
    g_test_add_func("/engine-runtime/reconnect/exponential-backoff",
                    test_pane_retry_backoff);
    g_test_add_func("/engine-runtime/error/bounded-sanitized-payload",
                    test_engine_error_payload_validation);
    g_test_add_func("/engine-runtime/graphics/kitty-response",
                    test_kitty_graphics_response_classification);
    g_test_add_func("/engine-runtime/graphics/trusted-overlay-layering",
                    test_kitty_frame_trusted_overlay_layering);
    g_test_add_func("/engine-runtime/popup/global-view-capacity",
                    test_engine_global_view_capacity);
    g_test_add_func("/engine-runtime/privacy/distinct-private-sessions",
                    test_private_network_sessions_are_distinct_and_ephemeral);
    g_test_add_func("/engine-runtime/lifecycle/idle-fallback-policy",
                    test_engine_idle_fallback_policy);
    g_test_add_func("/engine-runtime/graphics/backpressure-retry",
                    test_frame_backpressure_retry_is_bounded);
    g_test_add_func("/engine-runtime/scale/configuration",
                    test_device_scale_configuration);
    g_test_add_func("/engine-runtime/scale/logical-geometry",
                    test_device_scale_geometry);
    g_test_add_func("/engine-runtime/find/shortcuts-and-trusted-label",
                    test_find_shortcuts_and_trusted_label);
    g_test_add_func("/engine-runtime/shortcuts/exact-modifiers",
                    test_shortcut_exact_modifier_matching);
    g_test_add_func("/engine-runtime/shortcuts/lifecycle-and-forwarding",
                    test_shortcut_lifecycle_and_unmatched_meta);

    for (index = 0; index < G_N_ELEMENTS(malformed_cases); index++) {
        g_test_add_data_func(malformed_cases[index].path,
                             &malformed_cases[index],
                             test_malformed_packet);
    }

    return g_test_run();
}
