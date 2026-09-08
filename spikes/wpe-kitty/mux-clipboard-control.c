#include "mux-clipboard-control.h"

#include <string.h>

#define CONTROL_MAGIC 0x4d584343U
#define SUMMARY_FIXED_SIZE 32U

G_DEFINE_QUARK(mux-clipboard-control-error-quark,
               mux_clipboard_control_error)

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

static guint64
read_u64(const guint8 *data)
{
    guint64 value;

    memcpy(&value, data, sizeof(value));
    return GUINT64_FROM_BE(value);
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

static void
write_u64(guint8 *data, guint64 value)
{
    value = GUINT64_TO_BE(value);
    memcpy(data, &value, sizeof(value));
}

static gboolean
set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_CLIPBOARD_CONTROL_ERROR,
                        MUX_CLIPBOARD_CONTROL_ERROR_INVALID,
                        message);
    return FALSE;
}

static gboolean
valid_type(guint16 type)
{
    return type >= MUX_CLIPBOARD_CONTROL_HELLO &&
           type <= MUX_CLIPBOARD_CONTROL_BYE;
}

GBytes *
mux_clipboard_control_record_encode(
    const MuxClipboardControlRecord *record,
    GError **error)
{
    guint8 header[MUX_CLIPBOARD_CONTROL_HEADER_SIZE] = { 0 };
    GByteArray *packet;
    const guint8 *payload = NULL;
    gsize payload_length = 0;

    g_return_val_if_fail(record != NULL, NULL);
    if (!valid_type(record->type) || record->request_id == 0 ||
        record->flags & ~MUX_CLIPBOARD_CONTROL_FLAGS_ALL) {
        set_invalid(error, "invalid clipboard control record");
        return NULL;
    }
    if (record->created_us < 0) {
        set_invalid(error, "invalid clipboard control timestamp");
        return NULL;
    }
    if (record->payload != NULL)
        payload = g_bytes_get_data(record->payload, &payload_length);
    if (payload_length > MUX_CLIPBOARD_CONTROL_MAX_PAYLOAD) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_CONTROL_ERROR,
                            MUX_CLIPBOARD_CONTROL_ERROR_LIMIT,
                            "clipboard control payload is too large");
        return NULL;
    }

    write_u32(header + 0, CONTROL_MAGIC);
    write_u16(header + 4, MUX_CLIPBOARD_CONTROL_VERSION);
    write_u16(header + 6, record->type);
    write_u32(header + 8, record->flags);
    write_u32(header + 12, MUX_CLIPBOARD_CONTROL_HEADER_SIZE);
    write_u64(header + 16, record->request_id);
    write_u64(header + 24, record->entry_id);
    write_u64(header + 32, (guint64)record->created_us);
    write_u32(header + 40, payload_length);

    packet = g_byte_array_sized_new(MUX_CLIPBOARD_CONTROL_HEADER_SIZE +
                                    payload_length);
    g_byte_array_append(packet, header, sizeof(header));
    if (payload_length > 0)
        g_byte_array_append(packet, payload, payload_length);
    return g_byte_array_free_to_bytes(packet);
}

gboolean
mux_clipboard_control_record_decode(const guint8 *packet,
                                    gsize packet_length,
                                    MuxClipboardControlRecord *record,
                                    GError **error)
{
    guint16 type;
    guint32 payload_length;
    guint64 created_us;

    g_return_val_if_fail(record != NULL, FALSE);
    memset(record, 0, sizeof(*record));

    if (packet == NULL ||
        packet_length < MUX_CLIPBOARD_CONTROL_HEADER_SIZE ||
        packet_length > MUX_CLIPBOARD_CONTROL_MAX_PACKET)
        return set_invalid(error, "invalid clipboard control packet length");
    if (read_u32(packet + 0) != CONTROL_MAGIC ||
        read_u16(packet + 4) != MUX_CLIPBOARD_CONTROL_VERSION ||
        read_u32(packet + 12) != MUX_CLIPBOARD_CONTROL_HEADER_SIZE ||
        read_u32(packet + 44) != 0)
        return set_invalid(error, "invalid clipboard control header");

    type = read_u16(packet + 6);
    record->flags = read_u32(packet + 8);
    record->request_id = read_u64(packet + 16);
    record->entry_id = read_u64(packet + 24);
    created_us = read_u64(packet + 32);
    payload_length = read_u32(packet + 40);
    if (!valid_type(type) || record->request_id == 0 ||
        record->flags & ~MUX_CLIPBOARD_CONTROL_FLAGS_ALL ||
        created_us > G_MAXINT64 ||
        payload_length != packet_length -
                              MUX_CLIPBOARD_CONTROL_HEADER_SIZE)
        return set_invalid(error, "invalid clipboard control fields");

    record->type = type;
    record->created_us = (gint64)created_us;
    if (payload_length == 0)
        record->payload = g_bytes_new_static("", 0);
    else
        record->payload = g_bytes_new(
            packet + MUX_CLIPBOARD_CONTROL_HEADER_SIZE,
            payload_length);
    return TRUE;
}

void
mux_clipboard_control_record_clear(MuxClipboardControlRecord *record)
{
    if (record == NULL)
        return;
    g_clear_pointer(&record->payload, g_bytes_unref);
    memset(record, 0, sizeof(*record));
}

gboolean
mux_clipboard_control_payload_text(
    const MuxClipboardControlRecord *record,
    gsize max_length,
    gboolean allow_empty,
    gchar **text,
    GError **error)
{
    const guint8 *data;
    gsize length;

    g_return_val_if_fail(record != NULL, FALSE);
    g_return_val_if_fail(text != NULL, FALSE);
    *text = NULL;
    data = g_bytes_get_data(record->payload, &length);
    if (length > max_length || (!allow_empty && length == 0))
        return set_invalid(error, "clipboard control text has an invalid length");
    if (length > 0 &&
        (memchr(data, '\0', length) != NULL ||
         !g_utf8_validate((const gchar *)data, length, NULL)))
        return set_invalid(error, "clipboard control text is not UTF-8");

    *text = length > 0
                ? g_strndup((const gchar *)data, length)
                : g_strdup("");
    return TRUE;
}

GBytes *
mux_clipboard_control_summary_encode(
    guint64 request_id,
    const MuxClipboardControlSummary *summary,
    GError **error)
{
    MuxClipboardControlRecord record = { 0 };
    guint8 fixed[SUMMARY_FIXED_SIZE] = { 0 };
    GByteArray *payload;
    GBytes *payload_bytes;
    GBytes *packet;
    gsize origin_length;
    gsize preview_length;
    gsize mime_bytes = 0;
    guint32 encoded_mime_count = 0;
    guint32 i;

    g_return_val_if_fail(summary != NULL, NULL);
    if (summary->entry_id == 0 || summary->created_us < 0 ||
        summary->source_origin == NULL || summary->preview == NULL ||
        summary->mime_type_count > summary->format_count ||
        summary->format_count > MUX_CLIPBOARD_MAX_ITEMS ||
        (summary->mime_type_count > 0 && summary->mime_types == NULL) ||
        !g_utf8_validate(summary->source_origin, -1, NULL) ||
        !g_utf8_validate(summary->preview, -1, NULL)) {
        set_invalid(error, "invalid clipboard summary");
        return NULL;
    }

    origin_length = strlen(summary->source_origin);
    preview_length = strlen(summary->preview);
    for (i = 0; i < summary->mime_type_count; i++) {
        gsize mime_length;

        if (!mux_clipboard_mime_is_valid(summary->mime_types[i])) {
            set_invalid(error, "invalid MIME type in clipboard summary");
            return NULL;
        }
        mime_length = strlen(summary->mime_types[i]);
        if (SUMMARY_FIXED_SIZE + origin_length + preview_length +
                mime_bytes + 2 + mime_length >
            MUX_CLIPBOARD_CONTROL_MAX_PAYLOAD)
            break;
        mime_bytes += 2 + mime_length;
        encoded_mime_count++;
    }

    if (origin_length > MUX_CLIPBOARD_CONTROL_MAX_TEXT ||
        preview_length > MUX_CLIPBOARD_CONTROL_MAX_TEXT ||
        SUMMARY_FIXED_SIZE + origin_length + preview_length + mime_bytes >
            MUX_CLIPBOARD_CONTROL_MAX_PAYLOAD) {
        g_set_error_literal(error,
                            MUX_CLIPBOARD_CONTROL_ERROR,
                            MUX_CLIPBOARD_CONTROL_ERROR_LIMIT,
                            "clipboard summary is too large");
        return NULL;
    }

    write_u32(fixed + 0, origin_length);
    write_u32(fixed + 4, preview_length);
    write_u32(fixed + 8, summary->format_count);
    write_u32(fixed + 12, encoded_mime_count);
    write_u64(fixed + 16, summary->source_view_id);
    write_u64(fixed + 24, summary->total_bytes);
    payload = g_byte_array_sized_new(SUMMARY_FIXED_SIZE + origin_length +
                                     preview_length + mime_bytes);
    g_byte_array_append(payload, fixed, sizeof(fixed));
    g_byte_array_append(payload,
                        (const guint8 *)summary->source_origin,
                        origin_length);
    g_byte_array_append(payload,
                        (const guint8 *)summary->preview,
                        preview_length);
    for (i = 0; i < encoded_mime_count; i++) {
        guint8 encoded_length[2];
        gsize mime_length = strlen(summary->mime_types[i]);

        write_u16(encoded_length, (guint16)mime_length);
        g_byte_array_append(payload, encoded_length, sizeof(encoded_length));
        g_byte_array_append(payload,
                            (const guint8 *)summary->mime_types[i],
                            mime_length);
    }
    payload_bytes = g_byte_array_free_to_bytes(payload);

    record.type = MUX_CLIPBOARD_CONTROL_SUMMARY;
    record.flags = summary->pinned
                       ? MUX_CLIPBOARD_CONTROL_FLAG_PINNED
                       : 0;
    record.request_id = request_id;
    record.entry_id = summary->entry_id;
    record.created_us = summary->created_us;
    record.payload = payload_bytes;
    packet = mux_clipboard_control_record_encode(&record, error);
    g_bytes_unref(payload_bytes);
    return packet;
}

gboolean
mux_clipboard_control_summary_decode(
    const MuxClipboardControlRecord *record,
    MuxClipboardControlSummary *summary,
    GError **error)
{
    const guint8 *data;
    gsize length;
    guint32 origin_length;
    guint32 preview_length;
    guint32 mime_type_count;
    gsize offset;
    guint32 i;

    g_return_val_if_fail(record != NULL, FALSE);
    g_return_val_if_fail(summary != NULL, FALSE);
    memset(summary, 0, sizeof(*summary));
    if (record->type != MUX_CLIPBOARD_CONTROL_SUMMARY ||
        record->entry_id == 0 ||
        record->flags & ~MUX_CLIPBOARD_CONTROL_FLAG_PINNED)
        return set_invalid(error, "record is not a clipboard summary");

    data = g_bytes_get_data(record->payload, &length);
    if (length < SUMMARY_FIXED_SIZE)
        return set_invalid(error, "clipboard summary is truncated");
    origin_length = read_u32(data + 0);
    preview_length = read_u32(data + 4);
    mime_type_count = read_u32(data + 12);
    if (origin_length > MUX_CLIPBOARD_CONTROL_MAX_TEXT ||
        preview_length > MUX_CLIPBOARD_CONTROL_MAX_TEXT ||
        read_u32(data + 8) > MUX_CLIPBOARD_MAX_ITEMS ||
        mime_type_count > read_u32(data + 8) ||
        length < SUMMARY_FIXED_SIZE + origin_length + preview_length)
        return set_invalid(error, "clipboard summary length mismatch");
    if ((origin_length > 0 &&
         (memchr(data + SUMMARY_FIXED_SIZE, '\0', origin_length) != NULL ||
          !g_utf8_validate((const gchar *)data + SUMMARY_FIXED_SIZE,
                           origin_length,
                           NULL))) ||
        (preview_length > 0 &&
         (memchr(data + SUMMARY_FIXED_SIZE + origin_length,
                 '\0',
                 preview_length) != NULL ||
          !g_utf8_validate((const gchar *)data + SUMMARY_FIXED_SIZE +
                               origin_length,
                           preview_length,
                           NULL))))
        return set_invalid(error, "clipboard summary text is not UTF-8");

    summary->entry_id = record->entry_id;
    summary->created_us = record->created_us;
    summary->pinned = record->flags & MUX_CLIPBOARD_CONTROL_FLAG_PINNED;
    summary->source_origin =
        g_strndup((const gchar *)data + SUMMARY_FIXED_SIZE,
                  origin_length);
    summary->preview =
        g_strndup((const gchar *)data + SUMMARY_FIXED_SIZE + origin_length,
                  preview_length);
    summary->format_count = read_u32(data + 8);
    summary->source_view_id = read_u64(data + 16);
    summary->total_bytes = read_u64(data + 24);
    summary->mime_type_count = mime_type_count;
    summary->mime_types = g_new0(gchar *, mime_type_count + 1);
    offset = SUMMARY_FIXED_SIZE + origin_length + preview_length;
    for (i = 0; i < mime_type_count; i++) {
        guint16 mime_length;

        if (offset + 2 > length)
            goto invalid_mimes;
        mime_length = read_u16(data + offset);
        offset += 2;
        if (mime_length == 0 || mime_length > MUX_CLIPBOARD_MAX_MIME ||
            offset + mime_length > length ||
            memchr(data + offset, '\0', mime_length) != NULL) {
            goto invalid_mimes;
        }
        summary->mime_types[i] =
            g_strndup((const gchar *)data + offset, mime_length);
        if (!mux_clipboard_mime_is_valid(summary->mime_types[i]))
            goto invalid_mimes;
        offset += mime_length;
    }
    if (offset != length)
        goto invalid_mimes;
    return TRUE;

invalid_mimes:
    mux_clipboard_control_summary_clear(summary);
    return set_invalid(error, "clipboard summary MIME list is invalid");
}

void
mux_clipboard_control_summary_clear(MuxClipboardControlSummary *summary)
{
    if (summary == NULL)
        return;
    g_free(summary->source_origin);
    g_free(summary->preview);
    g_strfreev(summary->mime_types);
    memset(summary, 0, sizeof(*summary));
}
