#include "mux-engine-protocol.h"

#include <errno.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void
write_u16(guint8 *target, guint16 value)
{
    target[0] = (guint8)(value >> 8);
    target[1] = (guint8)value;
}

static void
write_u32(guint8 *target, guint32 value)
{
    target[0] = (guint8)(value >> 24);
    target[1] = (guint8)(value >> 16);
    target[2] = (guint8)(value >> 8);
    target[3] = (guint8)value;
}

static void
write_u64(guint8 *target, guint64 value)
{
    write_u32(target, (guint32)(value >> 32));
    write_u32(target + 4, (guint32)value);
}

static guint16
read_u16(const guint8 *source)
{
    return ((guint16)source[0] << 8) | source[1];
}

static guint32
read_u32(const guint8 *source)
{
    return ((guint32)source[0] << 24) |
        ((guint32)source[1] << 16) |
        ((guint32)source[2] << 8) |
        source[3];
}

static guint64
read_u64(const guint8 *source)
{
    return ((guint64)read_u32(source) << 32) | read_u32(source + 4);
}

GQuark
mux_engine_error_quark(void)
{
    return g_quark_from_static_string("mux-engine-error-quark");
}

void
mux_engine_message_init(MuxEngineMessage *message,
                        guint16 type,
                        guint32 flags,
                        guint64 view_id,
                        guint64 serial,
                        GBytes *payload)
{
    g_return_if_fail(message != NULL);

    message->type = type;
    message->flags = flags;
    message->view_id = view_id;
    message->serial = serial;
    message->payload = payload ? g_bytes_ref(payload) : g_bytes_new(NULL, 0);
}

void
mux_engine_message_clear(MuxEngineMessage *message)
{
    if (!message)
        return;

    g_clear_pointer(&message->payload, g_bytes_unref);
    memset(message, 0, sizeof(*message));
}

static ssize_t
send_packet(int fd, const guint8 *data, gsize length)
{
    ssize_t result;

    do {
        result = send(fd, data, length, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);

    return result;
}

gboolean
mux_engine_send_message(int fd,
                        const MuxEngineMessage *message,
                        GError **error)
{
    const guint8 *payload;
    gsize payload_size;
    gsize packet_size;
    guint8 *packet;
    ssize_t sent;

    g_return_val_if_fail(message != NULL, FALSE);
    g_return_val_if_fail(message->payload != NULL, FALSE);

    payload = g_bytes_get_data(message->payload, &payload_size);
    if (payload_size > MUX_ENGINE_MAX_PAYLOAD) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_TOO_LARGE,
                    "engine payload is %" G_GSIZE_FORMAT " bytes; maximum is %u",
                    payload_size,
                    MUX_ENGINE_MAX_PAYLOAD);
        return FALSE;
    }

    if (!message->type) {
        g_set_error_literal(error,
                            MUX_ENGINE_ERROR,
                            MUX_ENGINE_ERROR_PROTOCOL,
                            "engine message type zero is invalid");
        return FALSE;
    }

    packet_size = MUX_ENGINE_HEADER_SIZE + payload_size;
    packet = g_malloc(packet_size);
    write_u32(packet, MUX_ENGINE_MAGIC);
    write_u16(packet + 4, MUX_ENGINE_VERSION);
    write_u16(packet + 6, message->type);
    write_u32(packet + 8, message->flags);
    write_u32(packet + 12, (guint32)payload_size);
    write_u64(packet + 16, message->view_id);
    write_u64(packet + 24, message->serial);
    write_u64(packet + 32, 0);
    if (payload_size)
        memcpy(packet + MUX_ENGINE_HEADER_SIZE, payload, payload_size);

    sent = send_packet(fd, packet, packet_size);
    g_free(packet);

    if (sent < 0) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_IO,
                    "send engine packet: %s",
                    g_strerror(errno));
        return FALSE;
    }

    if ((gsize)sent != packet_size) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_IO,
                    "short engine packet write: sent %zd of %" G_GSIZE_FORMAT,
                    sent,
                    packet_size);
        return FALSE;
    }

    return TRUE;
}

static ssize_t
peek_packet_size(int fd)
{
    guint8 byte;
    ssize_t result;

    do {
        result = recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_TRUNC);
    } while (result < 0 && errno == EINTR);

    return result;
}

static void
discard_packet(int fd)
{
    guint8 byte;

    while (recv(fd, &byte, sizeof(byte), 0) < 0 && errno == EINTR)
        ;
}

gboolean
mux_engine_receive_message(int fd,
                           MuxEngineMessage *message,
                           GError **error)
{
    ssize_t peeked;
    ssize_t received;
    guint8 *packet;
    guint32 payload_size;
    gsize expected_size;
    GBytes *payload;

    g_return_val_if_fail(message != NULL, FALSE);
    memset(message, 0, sizeof(*message));

    peeked = peek_packet_size(fd);
    if (!peeked) {
        g_set_error_literal(error,
                            MUX_ENGINE_ERROR,
                            MUX_ENGINE_ERROR_CLOSED,
                            "engine connection closed");
        return FALSE;
    }
    if (peeked < 0) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_IO,
                    "peek engine packet: %s",
                    g_strerror(errno));
        return FALSE;
    }
    if ((gsize)peeked > MUX_ENGINE_HEADER_SIZE + MUX_ENGINE_MAX_PAYLOAD) {
        discard_packet(fd);
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_TOO_LARGE,
                    "engine packet is %zd bytes; maximum is %u",
                    peeked,
                    MUX_ENGINE_HEADER_SIZE + MUX_ENGINE_MAX_PAYLOAD);
        return FALSE;
    }

    packet = g_malloc((gsize)peeked);
    do {
        received = recv(fd, packet, (gsize)peeked, 0);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_IO,
                    "receive engine packet: %s",
                    g_strerror(errno));
        g_free(packet);
        return FALSE;
    }
    if (received != peeked) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_IO,
                    "engine packet changed while receiving: expected %zd, got %zd",
                    peeked,
                    received);
        g_free(packet);
        return FALSE;
    }
    if ((gsize)received < MUX_ENGINE_HEADER_SIZE) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_PROTOCOL,
                    "engine packet is shorter than its %u-byte header",
                    MUX_ENGINE_HEADER_SIZE);
        g_free(packet);
        return FALSE;
    }
    if (read_u32(packet) != MUX_ENGINE_MAGIC) {
        g_set_error_literal(error,
                            MUX_ENGINE_ERROR,
                            MUX_ENGINE_ERROR_PROTOCOL,
                            "engine packet has the wrong magic");
        g_free(packet);
        return FALSE;
    }
    if (read_u16(packet + 4) != MUX_ENGINE_VERSION) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_PROTOCOL,
                    "unsupported engine protocol version %u",
                    read_u16(packet + 4));
        g_free(packet);
        return FALSE;
    }

    payload_size = read_u32(packet + 12);
    expected_size = MUX_ENGINE_HEADER_SIZE + payload_size;
    if ((gsize)received != expected_size) {
        g_set_error(error,
                    MUX_ENGINE_ERROR,
                    MUX_ENGINE_ERROR_PROTOCOL,
                    "engine payload length mismatch: header says %u, packet has %" G_GSIZE_FORMAT,
                    payload_size,
                    (gsize)received - MUX_ENGINE_HEADER_SIZE);
        g_free(packet);
        return FALSE;
    }
    if (!read_u16(packet + 6)) {
        g_set_error_literal(error,
                            MUX_ENGINE_ERROR,
                            MUX_ENGINE_ERROR_PROTOCOL,
                            "engine message type zero is invalid");
        g_free(packet);
        return FALSE;
    }

    payload = g_bytes_new(packet + MUX_ENGINE_HEADER_SIZE, payload_size);
    message->type = read_u16(packet + 6);
    message->flags = read_u32(packet + 8);
    message->view_id = read_u64(packet + 16);
    message->serial = read_u64(packet + 24);
    message->payload = payload;
    g_free(packet);
    return TRUE;
}

void
mux_engine_builder_init(MuxEngineBuilder *builder)
{
    g_return_if_fail(builder != NULL);
    builder->bytes = g_byte_array_new();
}

void
mux_engine_builder_clear(MuxEngineBuilder *builder)
{
    if (!builder)
        return;
    g_clear_pointer(&builder->bytes, g_byte_array_unref);
}

void
mux_engine_builder_put_u16(MuxEngineBuilder *builder, guint16 value)
{
    guint8 encoded[2];

    g_return_if_fail(builder != NULL && builder->bytes != NULL);
    write_u16(encoded, value);
    g_byte_array_append(builder->bytes, encoded, sizeof(encoded));
}

void
mux_engine_builder_put_u32(MuxEngineBuilder *builder, guint32 value)
{
    guint8 encoded[4];

    g_return_if_fail(builder != NULL && builder->bytes != NULL);
    write_u32(encoded, value);
    g_byte_array_append(builder->bytes, encoded, sizeof(encoded));
}

void
mux_engine_builder_put_u64(MuxEngineBuilder *builder, guint64 value)
{
    guint8 encoded[8];

    g_return_if_fail(builder != NULL && builder->bytes != NULL);
    write_u64(encoded, value);
    g_byte_array_append(builder->bytes, encoded, sizeof(encoded));
}

void
mux_engine_builder_put_bytes(MuxEngineBuilder *builder,
                             const guint8 *data,
                             gsize length)
{
    g_return_if_fail(builder != NULL && builder->bytes != NULL);
    g_return_if_fail(data != NULL || !length);
    g_return_if_fail(length <= G_MAXUINT);

    if (length)
        g_byte_array_append(builder->bytes, data, (guint)length);
}

void
mux_engine_builder_put_string(MuxEngineBuilder *builder, const gchar *value)
{
    gsize length;

    g_return_if_fail(value != NULL);
    length = strlen(value);
    g_return_if_fail(length <= G_MAXUINT32);
    mux_engine_builder_put_u32(builder, (guint32)length);
    mux_engine_builder_put_bytes(builder, (const guint8 *)value, length);
}

GBytes *
mux_engine_builder_finish(MuxEngineBuilder *builder)
{
    GByteArray *bytes;

    g_return_val_if_fail(builder != NULL && builder->bytes != NULL, NULL);
    bytes = g_steal_pointer(&builder->bytes);
    return g_byte_array_free_to_bytes(bytes);
}

void
mux_engine_cursor_init(MuxEngineCursor *cursor, GBytes *payload)
{
    g_return_if_fail(cursor != NULL);
    g_return_if_fail(payload != NULL);

    cursor->data = g_bytes_get_data(payload, &cursor->length);
    cursor->offset = 0;
}

gboolean
mux_engine_cursor_get_bytes(MuxEngineCursor *cursor,
                            gsize length,
                            const guint8 **data)
{
    g_return_val_if_fail(cursor != NULL, FALSE);
    g_return_val_if_fail(data != NULL, FALSE);

    if (cursor->offset > cursor->length ||
        length > cursor->length - cursor->offset)
        return FALSE;

    *data = length ? cursor->data + cursor->offset : (const guint8 *)"";
    cursor->offset += length;
    return TRUE;
}

gboolean
mux_engine_cursor_get_u16(MuxEngineCursor *cursor, guint16 *value)
{
    const guint8 *encoded;

    g_return_val_if_fail(value != NULL, FALSE);
    if (!mux_engine_cursor_get_bytes(cursor, 2, &encoded))
        return FALSE;
    *value = read_u16(encoded);
    return TRUE;
}

gboolean
mux_engine_cursor_get_u32(MuxEngineCursor *cursor, guint32 *value)
{
    const guint8 *encoded;

    g_return_val_if_fail(value != NULL, FALSE);
    if (!mux_engine_cursor_get_bytes(cursor, 4, &encoded))
        return FALSE;
    *value = read_u32(encoded);
    return TRUE;
}

gboolean
mux_engine_cursor_get_u64(MuxEngineCursor *cursor, guint64 *value)
{
    const guint8 *encoded;

    g_return_val_if_fail(value != NULL, FALSE);
    if (!mux_engine_cursor_get_bytes(cursor, 8, &encoded))
        return FALSE;
    *value = read_u64(encoded);
    return TRUE;
}

gboolean
mux_engine_cursor_get_string(MuxEngineCursor *cursor, gchar **value)
{
    guint32 length;
    const guint8 *data;

    g_return_val_if_fail(value != NULL, FALSE);
    *value = NULL;
    if (!mux_engine_cursor_get_u32(cursor, &length) ||
        !mux_engine_cursor_get_bytes(cursor, length, &data) ||
        memchr(data, '\0', length))
        return FALSE;

    *value = g_strndup((const gchar *)data, length);
    return TRUE;
}

gboolean
mux_engine_cursor_done(const MuxEngineCursor *cursor)
{
    g_return_val_if_fail(cursor != NULL, FALSE);
    return cursor->offset == cursor->length;
}

gboolean
mux_engine_parse_device_scale(const gchar *value,
                              guint32 *scale_milli,
                              GError **error)
{
    const gchar *cursor;
    guint64 whole = 0;
    guint32 fraction = 0;
    guint fraction_digits = 0;
    guint64 parsed;

    g_return_val_if_fail(scale_milli != NULL, FALSE);
    if (value == NULL) {
        *scale_milli = MUX_ENGINE_DEFAULT_SCALE_MILLI;
        return TRUE;
    }

    cursor = value;
    if (!g_ascii_isdigit(*cursor))
        goto invalid;
    while (g_ascii_isdigit(*cursor)) {
        whole = whole * 10u + (guint)(*cursor - '0');
        if (whole > MUX_ENGINE_MAX_SCALE_MILLI / 1000u)
            goto invalid;
        cursor++;
    }
    if (*cursor == '.') {
        cursor++;
        if (!g_ascii_isdigit(*cursor))
            goto invalid;
        while (g_ascii_isdigit(*cursor)) {
            if (fraction_digits == 3)
                goto invalid;
            fraction = fraction * 10u + (guint)(*cursor - '0');
            fraction_digits++;
            cursor++;
        }
    }
    if (*cursor != '\0')
        goto invalid;
    while (fraction_digits < 3) {
        fraction *= 10u;
        fraction_digits++;
    }
    parsed = whole * 1000u + fraction;
    if (parsed < MUX_ENGINE_MIN_SCALE_MILLI ||
        parsed > MUX_ENGINE_MAX_SCALE_MILLI)
        goto invalid;

    *scale_milli = (guint32)parsed;
    return TRUE;

invalid:
    g_set_error(error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "%s must be a decimal from %.3f through %.3f with at most "
                "three fractional digits",
                MUX_DEVICE_SCALE_ENV,
                MUX_ENGINE_MIN_SCALE_MILLI / 1000.0,
                MUX_ENGINE_MAX_SCALE_MILLI / 1000.0);
    return FALSE;
}
