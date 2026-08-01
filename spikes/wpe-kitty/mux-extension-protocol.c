#include "mux-extension-protocol.h"

#include <string.h>

#define EXTENSION_MAGIC 0x4d584558U

G_DEFINE_QUARK(mux-extension-error-quark, mux_extension_error)

static guint16
read_u16(const guint8 *data)
{
    guint16 value;

    memcpy(&value, data, sizeof(value));
    return GUINT16_FROM_BE(value);
}

static guint32
read_u32(const guint8 *data)
{
    guint32 value;

    memcpy(&value, data, sizeof(value));
    return GUINT32_FROM_BE(value);
}

static void
write_u16(guint8 *data, guint16 value)
{
    value = GUINT16_TO_BE(value);
    memcpy(data, &value, sizeof(value));
}

static void
write_u32(guint8 *data, guint32 value)
{
    value = GUINT32_TO_BE(value);
    memcpy(data, &value, sizeof(value));
}

static gboolean
set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_EXTENSION_ERROR,
                        MUX_EXTENSION_ERROR_INVALID,
                        message);
    return FALSE;
}

GBytes *
mux_extension_record_encode(const MuxExtensionRecord *record,
                            GError **error)
{
    guint8 header[MUX_EXTENSION_HEADER_SIZE] = { 0 };
    GByteArray *packet;
    const guint8 *payload = NULL;
    gsize payload_length = 0;

    g_return_val_if_fail(record != NULL, NULL);
    if (record->channel == 0 ||
        record->channel > MUX_EXTENSION_MAX_CHANNEL ||
        record->flags != 0) {
        set_invalid(error, "invalid extension record channel or flags");
        return NULL;
    }
    if (record->payload != NULL)
        payload = g_bytes_get_data(record->payload, &payload_length);
    if (payload_length > MUX_EXTENSION_MAX_PAYLOAD) {
        g_set_error_literal(error,
                            MUX_EXTENSION_ERROR,
                            MUX_EXTENSION_ERROR_LIMIT,
                            "extension payload is too large");
        return NULL;
    }

    write_u32(header + 0, EXTENSION_MAGIC);
    write_u16(header + 4, MUX_EXTENSION_VERSION);
    write_u16(header + 6, record->channel);
    write_u32(header + 8, record->flags);
    write_u32(header + 12, MUX_EXTENSION_HEADER_SIZE);
    write_u32(header + 16, payload_length);

    packet = g_byte_array_sized_new(MUX_EXTENSION_HEADER_SIZE +
                                    payload_length);
    g_byte_array_append(packet, header, sizeof(header));
    if (payload_length > 0)
        g_byte_array_append(packet, payload, payload_length);
    return g_byte_array_free_to_bytes(packet);
}

gboolean
mux_extension_record_decode(const guint8 *packet,
                            gsize packet_length,
                            MuxExtensionRecord *record,
                            GError **error)
{
    guint32 payload_length;

    g_return_val_if_fail(record != NULL, FALSE);
    memset(record, 0, sizeof(*record));

    if (packet == NULL || packet_length < MUX_EXTENSION_HEADER_SIZE ||
        packet_length > MUX_EXTENSION_MAX_PACKET)
        return set_invalid(error, "invalid extension packet length");
    if (read_u32(packet + 0) != EXTENSION_MAGIC ||
        read_u16(packet + 4) != MUX_EXTENSION_VERSION ||
        read_u32(packet + 12) != MUX_EXTENSION_HEADER_SIZE ||
        read_u32(packet + 20) != 0)
        return set_invalid(error, "invalid extension packet header");

    record->channel = read_u16(packet + 6);
    record->flags = read_u32(packet + 8);
    payload_length = read_u32(packet + 16);
    if (record->channel == 0 ||
        record->channel > MUX_EXTENSION_MAX_CHANNEL ||
        record->flags != 0 ||
        payload_length != packet_length - MUX_EXTENSION_HEADER_SIZE)
        return set_invalid(error, "invalid extension packet fields");

    if (payload_length == 0)
        record->payload = g_bytes_new_static("", 0);
    else
        record->payload = g_bytes_new(packet + MUX_EXTENSION_HEADER_SIZE,
                                      payload_length);
    return TRUE;
}

void
mux_extension_record_clear(MuxExtensionRecord *record)
{
    if (record == NULL)
        return;
    g_clear_pointer(&record->payload, g_bytes_unref);
    memset(record, 0, sizeof(*record));
}
