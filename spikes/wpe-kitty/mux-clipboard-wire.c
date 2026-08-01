#include "mux-clipboard-wire.h"

#include <string.h>

#define WIRE_MAGIC 0x4d584342U
#define BEGIN_FIXED_SIZE 16U
#define ITEM_FIXED_SIZE 16U

struct _MuxClipboardWireTransfer {
    guint64 transaction_id;
    guint32 flags;
    gchar *profile;
    gchar *source_origin;
    guint64 source_view_id;
    gint64 created_us;
    MuxClipboardSnapshot *snapshot;
};

struct _MuxClipboardWireAssembler {
    gsize max_snapshot_bytes;
    guint64 transaction_id;
    guint32 flags;
    guint64 serial;
    guint64 source_view_id;
    gint64 created_us;
    gchar *profile;
    gchar *source_origin;
    guint32 expected_items;
    guint32 next_item;
    gsize expected_total;
    gsize completed_total;
    gchar *item_mime;
    gsize item_expected;
    GByteArray *item_data;
    MuxClipboardSnapshot *snapshot;
    gint64 deadline_us;
};

G_DEFINE_QUARK(mux-clipboard-wire-error-quark,
               mux_clipboard_wire_error)

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

static void
append_u32(GByteArray *array, guint32 value)
{
    guint8 encoded[4];

    write_u32(encoded, value);
    g_byte_array_append(array, encoded, sizeof(encoded));
}

static void
append_u64(GByteArray *array, guint64 value)
{
    guint8 encoded[8];

    write_u64(encoded, value);
    g_byte_array_append(array, encoded, sizeof(encoded));
}

static gboolean
valid_type(guint16 type)
{
    return type >= MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN &&
           type <= MUX_CLIPBOARD_WIRE_REMOTE_ERROR;
}

static gboolean
set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_CLIPBOARD_WIRE_ERROR,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID,
                        message);
    return FALSE;
}

static gboolean
set_limit(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_CLIPBOARD_WIRE_ERROR,
                        MUX_CLIPBOARD_WIRE_ERROR_LIMIT,
                        message);
    return FALSE;
}

static gboolean
set_state(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_CLIPBOARD_WIRE_ERROR,
                        MUX_CLIPBOARD_WIRE_ERROR_STATE,
                        message);
    return FALSE;
}

GBytes *
mux_clipboard_wire_record_encode(const MuxClipboardWireRecord *record,
                                 GError **error)
{
    GByteArray *packet;
    const guint8 *payload = NULL;
    gsize payload_length = 0;
    guint8 header[MUX_CLIPBOARD_WIRE_HEADER_SIZE] = { 0 };

    g_return_val_if_fail(record != NULL, NULL);
    if (!valid_type(record->type) || record->transaction_id == 0)
        return set_invalid(error, "invalid clipboard wire record") ? NULL : NULL;
    if (record->flags & ~MUX_CLIPBOARD_WIRE_FLAGS_ALL)
        return set_invalid(error, "unknown clipboard wire flags") ? NULL : NULL;

    if (record->payload != NULL)
        payload = g_bytes_get_data(record->payload, &payload_length);
    if (payload_length > MUX_CLIPBOARD_WIRE_MAX_PACKET -
                             MUX_CLIPBOARD_WIRE_HEADER_SIZE)
        return set_limit(error, "clipboard wire payload is too large") ? NULL : NULL;

    write_u32(header + 0, WIRE_MAGIC);
    write_u16(header + 4, MUX_CLIPBOARD_WIRE_VERSION);
    write_u16(header + 6, record->type);
    write_u32(header + 8, record->flags);
    write_u32(header + 12, MUX_CLIPBOARD_WIRE_HEADER_SIZE);
    write_u64(header + 16, record->transaction_id);
    write_u64(header + 24, record->serial);
    write_u64(header + 32, record->source_view_id);
    write_u64(header + 40, (guint64)record->created_us);
    write_u32(header + 48, record->item_index);
    write_u32(header + 52, record->item_count);
    write_u32(header + 56, payload_length);

    packet = g_byte_array_sized_new(MUX_CLIPBOARD_WIRE_HEADER_SIZE +
                                    payload_length);
    g_byte_array_append(packet, header, sizeof(header));
    if (payload_length > 0)
        g_byte_array_append(packet, payload, payload_length);
    return g_byte_array_free_to_bytes(packet);
}

gboolean
mux_clipboard_wire_record_decode(const guint8 *packet,
                                 gsize packet_length,
                                 MuxClipboardWireRecord *record,
                                 GError **error)
{
    guint16 type;
    guint32 payload_length;
    guint64 created_us;

    g_return_val_if_fail(record != NULL, FALSE);
    memset(record, 0, sizeof(*record));

    if (packet == NULL ||
        packet_length < MUX_CLIPBOARD_WIRE_HEADER_SIZE ||
        packet_length > MUX_CLIPBOARD_WIRE_MAX_PACKET)
        return set_invalid(error, "invalid clipboard wire packet length");
    if (read_u32(packet + 0) != WIRE_MAGIC ||
        read_u16(packet + 4) != MUX_CLIPBOARD_WIRE_VERSION ||
        read_u32(packet + 12) != MUX_CLIPBOARD_WIRE_HEADER_SIZE ||
        read_u32(packet + 60) != 0)
        return set_invalid(error, "invalid clipboard wire header");

    type = read_u16(packet + 6);
    if (!valid_type(type))
        return set_invalid(error, "unknown clipboard wire record type");
    record->flags = read_u32(packet + 8);
    if (record->flags & ~MUX_CLIPBOARD_WIRE_FLAGS_ALL)
        return set_invalid(error, "unknown clipboard wire flags");

    payload_length = read_u32(packet + 56);
    if (payload_length != packet_length - MUX_CLIPBOARD_WIRE_HEADER_SIZE)
        return set_invalid(error, "clipboard wire payload length mismatch");

    record->type = type;
    record->transaction_id = read_u64(packet + 16);
    record->serial = read_u64(packet + 24);
    record->source_view_id = read_u64(packet + 32);
    created_us = read_u64(packet + 40);
    if (record->transaction_id == 0 || created_us > G_MAXINT64)
        return set_invalid(error, "invalid clipboard wire identifier or timestamp");
    record->created_us = (gint64)created_us;
    record->item_index = read_u32(packet + 48);
    record->item_count = read_u32(packet + 52);
    if (payload_length == 0)
        record->payload = g_bytes_new_static("", 0);
    else
        record->payload = g_bytes_new(packet + MUX_CLIPBOARD_WIRE_HEADER_SIZE,
                                      payload_length);
    return TRUE;
}

void
mux_clipboard_wire_record_clear(MuxClipboardWireRecord *record)
{
    if (record == NULL)
        return;
    g_clear_pointer(&record->payload, g_bytes_unref);
    memset(record, 0, sizeof(*record));
}

static gboolean
valid_metadata_text(const gchar *text, gsize limit)
{
    gsize length;

    if (text == NULL)
        return TRUE;
    length = strlen(text);
    return length <= limit && g_utf8_validate(text, length, NULL);
}

static gboolean
send_record(const MuxClipboardWireRecord *record,
            MuxClipboardWireSendFunc send_func,
            gpointer user_data,
            GError **error)
{
    GBytes *packet;
    gboolean result;

    packet = mux_clipboard_wire_record_encode(record, error);
    if (packet == NULL)
        return FALSE;
    result = send_func(packet, user_data, error);
    g_bytes_unref(packet);
    if (!result && error != NULL && *error == NULL)
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "clipboard wire sender rejected a packet");
    return result;
}

static void
send_cancel(guint64 transaction_id,
            MuxClipboardWireSendFunc send_func,
            gpointer user_data)
{
    MuxClipboardWireRecord record = {
        .type = MUX_CLIPBOARD_WIRE_CANCEL,
        .transaction_id = transaction_id
    };

    send_record(&record, send_func, user_data, NULL);
}

gboolean
mux_clipboard_wire_send_snapshot(guint64 transaction_id,
                                 guint32 flags,
                                 const gchar *profile,
                                 const gchar *source_origin,
                                 guint64 source_view_id,
                                 gint64 created_us,
                                 const MuxClipboardSnapshot *snapshot,
                                 MuxClipboardWireSendFunc send_func,
                                 gpointer user_data,
                                 GError **error)
{
    MuxClipboardWireRecord record = { 0 };
    GByteArray *metadata;
    GBytes *payload;
    gsize profile_length;
    gsize origin_length;
    guint i;
    gboolean began = FALSE;

    g_return_val_if_fail(snapshot != NULL, FALSE);
    g_return_val_if_fail(send_func != NULL, FALSE);
    if (transaction_id == 0 || flags & ~MUX_CLIPBOARD_WIRE_FLAGS_ALL)
        return set_invalid(error, "invalid clipboard snapshot transfer");
    if (!valid_metadata_text(profile, MUX_CLIPBOARD_WIRE_MAX_PROFILE) ||
        !valid_metadata_text(source_origin, MUX_CLIPBOARD_WIRE_MAX_ORIGIN))
        return set_invalid(error, "invalid clipboard transfer metadata");
    if (mux_clipboard_snapshot_get_total_bytes(snapshot) >
        MUX_CLIPBOARD_MAX_TOTAL_BYTES)
        return set_limit(error, "clipboard snapshot is too large to transfer");

    profile_length = profile != NULL ? strlen(profile) : 0;
    origin_length = source_origin != NULL ? strlen(source_origin) : 0;
    metadata = g_byte_array_sized_new(BEGIN_FIXED_SIZE +
                                      profile_length + origin_length);
    append_u32(metadata, profile_length);
    append_u32(metadata, origin_length);
    append_u64(metadata, mux_clipboard_snapshot_get_total_bytes(snapshot));
    if (profile_length > 0)
        g_byte_array_append(metadata,
                            (const guint8 *)profile,
                            profile_length);
    if (origin_length > 0)
        g_byte_array_append(metadata,
                            (const guint8 *)source_origin,
                            origin_length);
    payload = g_byte_array_free_to_bytes(metadata);

    record.type = MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN;
    record.flags = flags;
    record.transaction_id = transaction_id;
    record.serial = mux_clipboard_snapshot_get_serial(snapshot);
    record.source_view_id = source_view_id;
    record.created_us = created_us > 0 ? created_us : g_get_monotonic_time();
    record.item_count = mux_clipboard_snapshot_get_count(snapshot);
    record.payload = payload;
    if (!send_record(&record, send_func, user_data, error)) {
        g_bytes_unref(payload);
        return FALSE;
    }
    g_bytes_unref(payload);
    record.payload = NULL;
    began = TRUE;

    for (i = 0; i < mux_clipboard_snapshot_get_count(snapshot); i++) {
        const gchar *mime = NULL;
        GBytes *bytes = NULL;
        GByteArray *item;
        const guint8 *data;
        gsize length;
        gsize mime_length;
        gsize offset;

        mux_clipboard_snapshot_get_item(snapshot, i, &mime, &bytes);
        mime_length = strlen(mime);
        data = g_bytes_get_data(bytes, &length);

        item = g_byte_array_sized_new(ITEM_FIXED_SIZE + mime_length);
        append_u32(item, mime_length);
        append_u32(item, 0);
        append_u64(item, length);
        g_byte_array_append(item, (const guint8 *)mime, mime_length);
        payload = g_byte_array_free_to_bytes(item);

        memset(&record, 0, sizeof(record));
        record.type = MUX_CLIPBOARD_WIRE_ITEM_BEGIN;
        record.transaction_id = transaction_id;
        record.item_index = i;
        record.payload = payload;
        if (!send_record(&record, send_func, user_data, error)) {
            g_bytes_unref(payload);
            goto fail;
        }
        g_bytes_unref(payload);

        for (offset = 0; offset < length;) {
            gsize amount = MIN((gsize)MUX_CLIPBOARD_WIRE_MAX_CHUNK,
                               length - offset);

            payload = g_bytes_new(data + offset, amount);
            memset(&record, 0, sizeof(record));
            record.type = MUX_CLIPBOARD_WIRE_ITEM_DATA;
            record.transaction_id = transaction_id;
            record.item_index = i;
            record.payload = payload;
            if (!send_record(&record, send_func, user_data, error)) {
                g_bytes_unref(payload);
                goto fail;
            }
            g_bytes_unref(payload);
            offset += amount;
        }
    }

    memset(&record, 0, sizeof(record));
    record.type = MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT;
    record.transaction_id = transaction_id;
    if (!send_record(&record, send_func, user_data, error))
        goto fail;
    return TRUE;

fail:
    if (began)
        send_cancel(transaction_id, send_func, user_data);
    return FALSE;
}

void
mux_clipboard_wire_assembler_reset(MuxClipboardWireAssembler *assembler)
{
    g_return_if_fail(assembler != NULL);

    assembler->transaction_id = 0;
    assembler->flags = 0;
    assembler->serial = 0;
    assembler->source_view_id = 0;
    assembler->created_us = 0;
    g_clear_pointer(&assembler->profile, g_free);
    g_clear_pointer(&assembler->source_origin, g_free);
    assembler->expected_items = 0;
    assembler->next_item = 0;
    assembler->expected_total = 0;
    assembler->completed_total = 0;
    g_clear_pointer(&assembler->item_mime, g_free);
    assembler->item_expected = 0;
    g_clear_pointer(&assembler->item_data, g_byte_array_unref);
    g_clear_pointer(&assembler->snapshot, mux_clipboard_snapshot_unref);
    assembler->deadline_us = 0;
}

MuxClipboardWireAssembler *
mux_clipboard_wire_assembler_new(gsize max_snapshot_bytes)
{
    MuxClipboardWireAssembler *assembler;

    if (max_snapshot_bytes == 0)
        max_snapshot_bytes = MUX_CLIPBOARD_MAX_TOTAL_BYTES;
    g_return_val_if_fail(max_snapshot_bytes <=
                             MUX_CLIPBOARD_MAX_TOTAL_BYTES,
                         NULL);

    assembler = g_new0(MuxClipboardWireAssembler, 1);
    assembler->max_snapshot_bytes = max_snapshot_bytes;
    return assembler;
}

void
mux_clipboard_wire_assembler_free(MuxClipboardWireAssembler *assembler)
{
    if (assembler == NULL)
        return;
    mux_clipboard_wire_assembler_reset(assembler);
    g_free(assembler);
}

static gint64
feed_deadline(gint64 monotonic_us)
{
    if (monotonic_us <= 0)
        monotonic_us = g_get_monotonic_time();
    return monotonic_us + ((gint64)MUX_CLIPBOARD_WIRE_TIMEOUT_MS * 1000);
}

static gboolean
decode_begin(MuxClipboardWireAssembler *assembler,
             const MuxClipboardWireRecord *record,
             GError **error)
{
    const guint8 *data;
    gsize length;
    guint32 profile_length;
    guint32 origin_length;
    guint64 total;

    data = g_bytes_get_data(record->payload, &length);
    if (length < BEGIN_FIXED_SIZE)
        return set_invalid(error, "clipboard begin metadata is truncated");

    profile_length = read_u32(data + 0);
    origin_length = read_u32(data + 4);
    total = read_u64(data + 8);
    if (profile_length > MUX_CLIPBOARD_WIRE_MAX_PROFILE ||
        origin_length > MUX_CLIPBOARD_WIRE_MAX_ORIGIN ||
        length != BEGIN_FIXED_SIZE + profile_length + origin_length ||
        total > assembler->max_snapshot_bytes || total > G_MAXSIZE)
        return set_limit(error, "clipboard begin metadata exceeds its limits");
    if (record->item_count > MUX_CLIPBOARD_MAX_ITEMS)
        return set_limit(error, "clipboard transfer has too many items");

    if ((profile_length > 0 &&
         (memchr(data + BEGIN_FIXED_SIZE, '\0', profile_length) != NULL ||
          !g_utf8_validate((const gchar *)data + BEGIN_FIXED_SIZE,
                           profile_length,
                           NULL))) ||
        (origin_length > 0 &&
         (memchr(data + BEGIN_FIXED_SIZE + profile_length,
                 '\0',
                 origin_length) != NULL ||
          !g_utf8_validate((const gchar *)data + BEGIN_FIXED_SIZE +
                               profile_length,
                           origin_length,
                           NULL))))
        return set_invalid(error, "clipboard begin metadata is not UTF-8");

    assembler->transaction_id = record->transaction_id;
    assembler->flags = record->flags;
    assembler->serial = record->serial;
    assembler->source_view_id = record->source_view_id;
    assembler->created_us = record->created_us;
    assembler->profile = profile_length > 0
                             ? g_strndup((const gchar *)data +
                                            BEGIN_FIXED_SIZE,
                                        profile_length)
                             : NULL;
    assembler->source_origin = origin_length > 0
                                   ? g_strndup((const gchar *)data +
                                                  BEGIN_FIXED_SIZE +
                                                  profile_length,
                                              origin_length)
                                   : NULL;
    assembler->expected_items = record->item_count;
    assembler->expected_total = total;
    assembler->snapshot = mux_clipboard_snapshot_new(record->serial);
    return TRUE;
}

static gboolean
finish_item(MuxClipboardWireAssembler *assembler, GError **error)
{
    GBytes *bytes;
    gboolean result;

    if (assembler->item_expected == 0)
        bytes = g_bytes_new_static("", 0);
    else
        bytes = g_byte_array_free_to_bytes(assembler->item_data);
    assembler->item_data = NULL;
    result = mux_clipboard_snapshot_add(assembler->snapshot,
                                        assembler->item_mime,
                                        bytes,
                                        error);
    g_bytes_unref(bytes);
    if (!result)
        return FALSE;

    assembler->completed_total += assembler->item_expected;
    assembler->next_item++;
    g_clear_pointer(&assembler->item_mime, g_free);
    assembler->item_expected = 0;
    return TRUE;
}

static gboolean
begin_item(MuxClipboardWireAssembler *assembler,
           const MuxClipboardWireRecord *record,
           GError **error)
{
    const guint8 *data;
    gsize length;
    guint32 mime_length;
    guint64 item_length;

    data = g_bytes_get_data(record->payload, &length);
    if (length < ITEM_FIXED_SIZE)
        return set_invalid(error, "clipboard item metadata is truncated");
    mime_length = read_u32(data + 0);
    if (read_u32(data + 4) != 0)
        return set_invalid(error, "clipboard item reserved field is nonzero");
    item_length = read_u64(data + 8);
    if (mime_length == 0 || mime_length > MUX_CLIPBOARD_MAX_MIME ||
        length != ITEM_FIXED_SIZE + mime_length ||
        item_length > MUX_CLIPBOARD_MAX_ITEM_BYTES ||
        item_length > G_MAXSIZE ||
        item_length > assembler->expected_total -
                          assembler->completed_total)
        return set_limit(error, "clipboard item metadata exceeds its limits");

    assembler->item_mime =
        g_strndup((const gchar *)data + ITEM_FIXED_SIZE, mime_length);
    if (!mux_clipboard_mime_is_valid(assembler->item_mime) ||
        mux_clipboard_snapshot_find(assembler->snapshot,
                                    assembler->item_mime) != NULL)
        return set_invalid(error, "clipboard item MIME type is invalid or duplicated");

    assembler->item_expected = item_length;
    assembler->item_data = g_byte_array_sized_new(item_length);
    if (item_length == 0)
        return finish_item(assembler, error);
    return TRUE;
}

static MuxClipboardWireFeedResult
feed_error(MuxClipboardWireAssembler *assembler,
           GError **error,
           MuxClipboardWireError code,
           const gchar *message)
{
    g_set_error_literal(error, MUX_CLIPBOARD_WIRE_ERROR, code, message);
    mux_clipboard_wire_assembler_reset(assembler);
    return MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
}

MuxClipboardWireFeedResult
mux_clipboard_wire_assembler_feed(MuxClipboardWireAssembler *assembler,
                                  const guint8 *packet,
                                  gsize packet_length,
                                  gint64 monotonic_us,
                                  MuxClipboardWireTransfer **completed,
                                  GError **error)
{
    MuxClipboardWireRecord record = { 0 };
    MuxClipboardWireFeedResult result =
        MUX_CLIPBOARD_WIRE_FEED_ACCEPTED;

    g_return_val_if_fail(assembler != NULL,
                         MUX_CLIPBOARD_WIRE_FEED_CANCELLED);
    g_return_val_if_fail(completed != NULL,
                         MUX_CLIPBOARD_WIRE_FEED_CANCELLED);
    *completed = NULL;

    if (!mux_clipboard_wire_record_decode(packet,
                                          packet_length,
                                          &record,
                                          error)) {
        mux_clipboard_wire_assembler_reset(assembler);
        return MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
    }

    if (record.type == MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN) {
        if (assembler->transaction_id != 0) {
            g_set_error_literal(error,
                                MUX_CLIPBOARD_WIRE_ERROR,
                                MUX_CLIPBOARD_WIRE_ERROR_STATE,
                                "clipboard transfer is already active");
            result = MUX_CLIPBOARD_WIRE_FEED_REJECTED;
            goto out;
        }
        if (record.item_index != 0 ||
            !decode_begin(assembler, &record, error)) {
            mux_clipboard_wire_assembler_reset(assembler);
            result = MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
            goto out;
        }
        assembler->deadline_us = feed_deadline(monotonic_us);
        goto out;
    }

    if (assembler->transaction_id == 0 ||
        record.transaction_id != assembler->transaction_id) {
        result = feed_error(assembler,
                            error,
                            MUX_CLIPBOARD_WIRE_ERROR_STATE,
                            "clipboard record has no matching transfer");
        goto out;
    }
    assembler->deadline_us = feed_deadline(monotonic_us);

    switch (record.type) {
    case MUX_CLIPBOARD_WIRE_ITEM_BEGIN:
        if (assembler->item_mime != NULL ||
            record.item_index != assembler->next_item ||
            assembler->next_item >= assembler->expected_items ||
            !begin_item(assembler, &record, error)) {
            if (error == NULL || *error == NULL)
                set_state(error, "clipboard item arrived out of order");
            mux_clipboard_wire_assembler_reset(assembler);
            result = MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
        }
        break;
    case MUX_CLIPBOARD_WIRE_ITEM_DATA: {
        const guint8 *data;
        gsize length;

        data = g_bytes_get_data(record.payload, &length);
        if (assembler->item_mime == NULL ||
            record.item_index != assembler->next_item ||
            length == 0 || length > MUX_CLIPBOARD_WIRE_MAX_CHUNK ||
            length > assembler->item_expected - assembler->item_data->len) {
            result = feed_error(assembler,
                                error,
                                MUX_CLIPBOARD_WIRE_ERROR_STATE,
                                "clipboard data arrived out of order or exceeded its item");
            break;
        }
        g_byte_array_append(assembler->item_data, data, length);
        if (assembler->item_data->len == assembler->item_expected &&
            !finish_item(assembler, error)) {
            mux_clipboard_wire_assembler_reset(assembler);
            result = MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
        }
        break;
    }
    case MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT:
        if (g_bytes_get_size(record.payload) != 0 ||
            assembler->item_mime != NULL ||
            assembler->next_item != assembler->expected_items ||
            assembler->completed_total != assembler->expected_total) {
            result = feed_error(assembler,
                                error,
                                MUX_CLIPBOARD_WIRE_ERROR_STATE,
                                "clipboard transfer committed incomplete data");
            break;
        }
        mux_clipboard_snapshot_seal(assembler->snapshot);
        *completed = g_new0(MuxClipboardWireTransfer, 1);
        (*completed)->transaction_id = assembler->transaction_id;
        (*completed)->flags = assembler->flags;
        (*completed)->profile = assembler->profile;
        (*completed)->source_origin = assembler->source_origin;
        (*completed)->source_view_id = assembler->source_view_id;
        (*completed)->created_us = assembler->created_us;
        (*completed)->snapshot = assembler->snapshot;
        assembler->profile = NULL;
        assembler->source_origin = NULL;
        assembler->snapshot = NULL;
        mux_clipboard_wire_assembler_reset(assembler);
        result = MUX_CLIPBOARD_WIRE_FEED_COMPLETED;
        break;
    case MUX_CLIPBOARD_WIRE_CANCEL:
        mux_clipboard_wire_assembler_reset(assembler);
        result = MUX_CLIPBOARD_WIRE_FEED_CANCELLED;
        break;
    case MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN:
    case MUX_CLIPBOARD_WIRE_ACK:
    case MUX_CLIPBOARD_WIRE_REMOTE_ERROR:
    default:
        result = feed_error(assembler,
                            error,
                            MUX_CLIPBOARD_WIRE_ERROR_STATE,
                            "invalid record in clipboard snapshot transfer");
        break;
    }

out:
    mux_clipboard_wire_record_clear(&record);
    return result;
}

gboolean
mux_clipboard_wire_assembler_tick(MuxClipboardWireAssembler *assembler,
                                  gint64 monotonic_us)
{
    g_return_val_if_fail(assembler != NULL, FALSE);
    if (assembler->transaction_id == 0 ||
        assembler->deadline_us > monotonic_us)
        return FALSE;
    mux_clipboard_wire_assembler_reset(assembler);
    return TRUE;
}

void
mux_clipboard_wire_transfer_free(MuxClipboardWireTransfer *transfer)
{
    if (transfer == NULL)
        return;
    g_free(transfer->profile);
    g_free(transfer->source_origin);
    mux_clipboard_snapshot_unref(transfer->snapshot);
    g_free(transfer);
}

guint64
mux_clipboard_wire_transfer_get_transaction_id(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, 0);
    return transfer->transaction_id;
}

guint32
mux_clipboard_wire_transfer_get_flags(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, 0);
    return transfer->flags;
}

const gchar *
mux_clipboard_wire_transfer_get_profile(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, NULL);
    return transfer->profile;
}

const gchar *
mux_clipboard_wire_transfer_get_source_origin(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, NULL);
    return transfer->source_origin;
}

guint64
mux_clipboard_wire_transfer_get_source_view_id(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, 0);
    return transfer->source_view_id;
}

gint64
mux_clipboard_wire_transfer_get_created_us(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, 0);
    return transfer->created_us;
}

const MuxClipboardSnapshot *
mux_clipboard_wire_transfer_get_snapshot(
    const MuxClipboardWireTransfer *transfer)
{
    g_return_val_if_fail(transfer != NULL, NULL);
    return transfer->snapshot;
}
