#include "../mux-clipboard-wire.h"
#include "../mux-kitty-clipboard.h"

#include <string.h>

/* This test target already links mux-clipboard.c; include the two focused
 * implementations here so their state machine can be covered without adding
 * another build target. */
#include "../mux-osc5522.c"
#include "../mux-kitty-clipboard.c"

#define TEST_BEGIN_FIXED_SIZE 16U
#define TEST_ITEM_FIXED_SIZE 16U

typedef struct {
    GPtrArray *packets;
} PacketSink;

static void
write_u16(guint8 *destination, guint16 value)
{
    guint16 encoded = GUINT16_TO_BE(value);

    memcpy(destination, &encoded, sizeof(encoded));
}

static void
write_u32(guint8 *destination, guint32 value)
{
    guint32 encoded = GUINT32_TO_BE(value);

    memcpy(destination, &encoded, sizeof(encoded));
}

static void
write_u64(guint8 *destination, guint64 value)
{
    guint64 encoded = GUINT64_TO_BE(value);

    memcpy(destination, &encoded, sizeof(encoded));
}

static void
append_u32(GByteArray *array, guint32 value)
{
    guint8 encoded[sizeof(value)];

    write_u32(encoded, value);
    g_byte_array_append(array, encoded, sizeof(encoded));
}

static void
append_u64(GByteArray *array, guint64 value)
{
    guint8 encoded[sizeof(value)];

    write_u64(encoded, value);
    g_byte_array_append(array, encoded, sizeof(encoded));
}

static gboolean
collect_packet(GBytes *packet, gpointer user_data, GError **error)
{
    PacketSink *sink = user_data;

    (void)error;
    g_ptr_array_add(sink->packets, g_bytes_ref(packet));
    return TRUE;
}

static void
packet_sink_init(PacketSink *sink)
{
    sink->packets =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
}

static void
packet_sink_clear(PacketSink *sink)
{
    g_clear_pointer(&sink->packets, g_ptr_array_unref);
}

static GBytes *
encode_packet(MuxClipboardWireType type,
              guint64 transaction_id,
              guint32 flags,
              guint64 serial,
              guint64 source_view_id,
              gint64 created_us,
              guint32 item_index,
              guint32 item_count,
              GBytes *payload)
{
    MuxClipboardWireRecord record = {
        .type = type,
        .flags = flags,
        .transaction_id = transaction_id,
        .serial = serial,
        .source_view_id = source_view_id,
        .created_us = created_us,
        .item_index = item_index,
        .item_count = item_count,
        .payload = payload
    };
    g_autoptr(GError) error = NULL;
    GBytes *packet;

    packet = mux_clipboard_wire_record_encode(&record, &error);
    g_assert_no_error(error);
    g_assert_nonnull(packet);
    return packet;
}

static GBytes *
make_begin_metadata(guint32 profile_length,
                    guint32 origin_length,
                    guint64 total_bytes,
                    const guint8 *trailing_data,
                    gsize trailing_length)
{
    GByteArray *metadata = g_byte_array_new();

    append_u32(metadata, profile_length);
    append_u32(metadata, origin_length);
    append_u64(metadata, total_bytes);
    if (trailing_length > 0)
        g_byte_array_append(metadata, trailing_data, trailing_length);
    return g_byte_array_free_to_bytes(metadata);
}

static GBytes *
make_begin_packet(guint64 transaction_id,
                  guint64 serial,
                  guint32 flags,
                  guint64 source_view_id,
                  gint64 created_us,
                  guint32 item_count,
                  guint64 total_bytes,
                  const gchar *profile,
                  const gchar *origin)
{
    GByteArray *trailing = g_byte_array_new();
    gsize profile_length = profile != NULL ? strlen(profile) : 0;
    gsize origin_length = origin != NULL ? strlen(origin) : 0;
    GBytes *metadata;
    GBytes *packet;

    g_assert_cmpuint(profile_length, <=, G_MAXUINT32);
    g_assert_cmpuint(origin_length, <=, G_MAXUINT32);
    if (profile_length > 0) {
        g_byte_array_append(trailing,
                            (const guint8 *)profile,
                            profile_length);
    }
    if (origin_length > 0) {
        g_byte_array_append(trailing,
                            (const guint8 *)origin,
                            origin_length);
    }
    metadata = make_begin_metadata((guint32)profile_length,
                                   (guint32)origin_length,
                                   total_bytes,
                                   trailing->data,
                                   trailing->len);
    g_byte_array_unref(trailing);
    packet = encode_packet(MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN,
                           transaction_id,
                           flags,
                           serial,
                           source_view_id,
                           created_us,
                           0,
                           item_count,
                           metadata);
    g_bytes_unref(metadata);
    return packet;
}

static GBytes *
make_item_metadata(guint32 mime_length,
                   guint32 reserved,
                   guint64 item_length,
                   const guint8 *mime_data,
                   gsize mime_data_length)
{
    GByteArray *metadata = g_byte_array_new();

    append_u32(metadata, mime_length);
    append_u32(metadata, reserved);
    append_u64(metadata, item_length);
    if (mime_data_length > 0)
        g_byte_array_append(metadata, mime_data, mime_data_length);
    return g_byte_array_free_to_bytes(metadata);
}

static GBytes *
make_item_begin_packet_raw(guint64 transaction_id,
                           guint32 item_index,
                           guint32 mime_length,
                           guint32 reserved,
                           guint64 item_length,
                           const guint8 *mime_data,
                           gsize mime_data_length)
{
    GBytes *metadata;
    GBytes *packet;

    metadata = make_item_metadata(mime_length,
                                  reserved,
                                  item_length,
                                  mime_data,
                                  mime_data_length);
    packet = encode_packet(MUX_CLIPBOARD_WIRE_ITEM_BEGIN,
                           transaction_id,
                           0,
                           0,
                           0,
                           0,
                           item_index,
                           0,
                           metadata);
    g_bytes_unref(metadata);
    return packet;
}

static GBytes *
make_item_begin_packet(guint64 transaction_id,
                       guint32 item_index,
                       const gchar *mime,
                       guint64 item_length)
{
    gsize mime_length = strlen(mime);

    g_assert_cmpuint(mime_length, <=, G_MAXUINT32);
    return make_item_begin_packet_raw(transaction_id,
                                      item_index,
                                      (guint32)mime_length,
                                      0,
                                      item_length,
                                      (const guint8 *)mime,
                                      mime_length);
}

static GBytes *
make_data_packet(guint64 transaction_id,
                 guint32 item_index,
                 const guint8 *data,
                 gsize length)
{
    GBytes *payload;
    GBytes *packet;

    payload = length > 0 ? g_bytes_new(data, length)
                         : g_bytes_new_static("", 0);
    packet = encode_packet(MUX_CLIPBOARD_WIRE_ITEM_DATA,
                           transaction_id,
                           0,
                           0,
                           0,
                           0,
                           item_index,
                           0,
                           payload);
    g_bytes_unref(payload);
    return packet;
}

static GBytes *
make_control_packet(MuxClipboardWireType type, guint64 transaction_id)
{
    return encode_packet(type,
                         transaction_id,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         NULL);
}

static MuxClipboardWireFeedResult
feed_packet(MuxClipboardWireAssembler *assembler,
            GBytes *packet,
            gint64 monotonic_us,
            MuxClipboardWireTransfer **completed,
            GError **error)
{
    const guint8 *data;
    gsize length;

    data = g_bytes_get_data(packet, &length);
    return mux_clipboard_wire_assembler_feed(assembler,
                                             data,
                                             length,
                                             monotonic_us,
                                             completed,
                                             error);
}

static void
assert_feed_without_error(MuxClipboardWireAssembler *assembler,
                          GBytes *packet,
                          gint64 monotonic_us,
                          MuxClipboardWireFeedResult expected)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxClipboardWireTransfer) completed = NULL;
    MuxClipboardWireFeedResult result;

    result = feed_packet(assembler,
                         packet,
                         monotonic_us,
                         &completed,
                         &error);
    g_assert_cmpint(result, ==, expected);
    g_assert_null(completed);
    g_assert_no_error(error);
}

static void
assert_feed_error(MuxClipboardWireAssembler *assembler,
                  GBytes *packet,
                  gint64 monotonic_us,
                  MuxClipboardWireFeedResult expected_result,
                  MuxClipboardWireError expected_error)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxClipboardWireTransfer) completed = NULL;
    MuxClipboardWireFeedResult result;

    result = feed_packet(assembler,
                         packet,
                         monotonic_us,
                         &completed,
                         &error);
    g_assert_cmpint(result, ==, expected_result);
    g_assert_null(completed);
    g_assert_error(error,
                   MUX_CLIPBOARD_WIRE_ERROR,
                   (gint)expected_error);
}

static void
assert_decode_error(const guint8 *packet,
                    gsize packet_length,
                    MuxClipboardWireError expected_error)
{
    MuxClipboardWireRecord record = { 0 };
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_clipboard_wire_record_decode(packet,
                                                    packet_length,
                                                    &record,
                                                    &error));
    g_assert_error(error,
                   MUX_CLIPBOARD_WIRE_ERROR,
                   (gint)expected_error);
    mux_clipboard_wire_record_clear(&record);
}

static void
add_snapshot_item(MuxClipboardSnapshot *snapshot,
                  const gchar *mime,
                  const guint8 *data,
                  gsize length)
{
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GError) error = NULL;

    bytes = length > 0 ? g_bytes_new(data, length)
                       : g_bytes_new_static("", 0);
    g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                             mime,
                                             bytes,
                                             &error));
    g_assert_no_error(error);
}

static void
assert_snapshot_item(const MuxClipboardSnapshot *snapshot,
                     const gchar *mime,
                     const guint8 *expected,
                     gsize expected_length)
{
    GBytes *bytes;
    const guint8 *actual;
    gsize actual_length;

    bytes = mux_clipboard_snapshot_find(snapshot, mime);
    g_assert_nonnull(bytes);
    actual = g_bytes_get_data(bytes, &actual_length);
    g_assert_cmpuint(actual_length, ==, expected_length);
    if (expected_length > 0)
        g_assert_cmpint(memcmp(actual, expected, expected_length), ==, 0);
}

static void
test_full_mime_binary_round_trip(void)
{
    static const guint8 plain[] = "clipboard text";
    static const guint8 html[] = "<b>clipboard text</b>";
    static const guint8 image[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0xff, 0x80, 0x01, 0x00, 0x7f
    };
    const guint64 transaction_id = G_GUINT64_CONSTANT(0x1020304050607080);
    const guint64 serial = G_GUINT64_CONSTANT(0x8877665544332211);
    const guint64 source_view_id = G_GUINT64_CONSTANT(0x123456789abcdef0);
    const gint64 created_us = G_GINT64_CONSTANT(9876543210);
    const guint32 flags = MUX_CLIPBOARD_WIRE_FLAG_CURRENT |
                          MUX_CLIPBOARD_WIRE_FLAG_HISTORY |
                          MUX_CLIPBOARD_WIRE_FLAG_PRIMARY;
    g_autoptr(MuxClipboardSnapshot) snapshot =
        mux_clipboard_snapshot_new(serial);
    g_autoptr(MuxClipboardWireAssembler) assembler =
        mux_clipboard_wire_assembler_new(0);
    g_autoptr(MuxClipboardWireTransfer) transfer = NULL;
    PacketSink sink;
    g_autoptr(GError) error = NULL;
    guint packet_index;

    add_snapshot_item(snapshot,
                      "text/plain;charset=utf-8",
                      plain,
                      sizeof(plain) - 1U);
    add_snapshot_item(snapshot,
                      "text/html",
                      html,
                      sizeof(html) - 1U);
    add_snapshot_item(snapshot, "image/png", image, sizeof(image));
    add_snapshot_item(snapshot, "application/x-mux-empty", NULL, 0);
    mux_clipboard_snapshot_seal(snapshot);

    packet_sink_init(&sink);
    g_assert_true(mux_clipboard_wire_send_snapshot(
        transaction_id,
        flags,
        "daily",
        "https://clipboard.example",
        source_view_id,
        created_us,
        snapshot,
        collect_packet,
        &sink,
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(sink.packets->len, ==, 9U);

    for (packet_index = 0; packet_index < sink.packets->len; packet_index++) {
        MuxClipboardWireTransfer *completed = NULL;
        MuxClipboardWireFeedResult result;

        result = feed_packet(assembler,
                             g_ptr_array_index(sink.packets, packet_index),
                             G_GINT64_CONSTANT(1000000) + packet_index,
                             &completed,
                             &error);
        g_assert_no_error(error);
        if (packet_index + 1U == sink.packets->len) {
            g_assert_cmpint(result, ==, MUX_CLIPBOARD_WIRE_FEED_COMPLETED);
            g_assert_nonnull(completed);
            transfer = completed;
        } else {
            g_assert_cmpint(result, ==, MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
            g_assert_null(completed);
        }
    }

    g_assert_cmpuint(mux_clipboard_wire_transfer_get_transaction_id(transfer),
                     ==,
                     transaction_id);
    g_assert_cmpuint(mux_clipboard_wire_transfer_get_flags(transfer), ==, flags);
    g_assert_cmpstr(mux_clipboard_wire_transfer_get_profile(transfer),
                    ==,
                    "daily");
    g_assert_cmpstr(mux_clipboard_wire_transfer_get_source_origin(transfer),
                    ==,
                    "https://clipboard.example");
    g_assert_cmpuint(mux_clipboard_wire_transfer_get_source_view_id(transfer),
                     ==,
                     source_view_id);
    g_assert_cmpint(mux_clipboard_wire_transfer_get_created_us(transfer),
                    ==,
                    created_us);

    {
        const MuxClipboardSnapshot *received =
            mux_clipboard_wire_transfer_get_snapshot(transfer);

        g_assert_nonnull(received);
        g_assert_true(mux_clipboard_snapshot_is_sealed(received));
        g_assert_cmpuint(mux_clipboard_snapshot_get_serial(received),
                         ==,
                         serial);
        g_assert_cmpuint(mux_clipboard_snapshot_get_count(received), ==, 4U);
        g_assert_cmpuint(mux_clipboard_snapshot_get_total_bytes(received),
                         ==,
                         (sizeof(plain) - 1U) +
                             (sizeof(html) - 1U) + sizeof(image));
        assert_snapshot_item(received,
                             "text/plain;charset=utf-8",
                             plain,
                             sizeof(plain) - 1U);
        assert_snapshot_item(received,
                             "text/html",
                             html,
                             sizeof(html) - 1U);
        assert_snapshot_item(received, "image/png", image, sizeof(image));
        assert_snapshot_item(received,
                             "application/x-mux-empty",
                             NULL,
                             0);
    }

    packet_sink_clear(&sink);
}

static void
test_chunk_boundaries(void)
{
    static const gsize sizes[] = {
        0,
        1,
        MUX_CLIPBOARD_WIRE_MAX_CHUNK - 1U,
        MUX_CLIPBOARD_WIRE_MAX_CHUNK,
        MUX_CLIPBOARD_WIRE_MAX_CHUNK + 1U,
        (2U * MUX_CLIPBOARD_WIRE_MAX_CHUNK) + 1U
    };
    gsize case_index;

    for (case_index = 0; case_index < G_N_ELEMENTS(sizes); case_index++) {
        const gsize size = sizes[case_index];
        const guint64 transaction_id = 100U + case_index;
        g_autofree guint8 *expected = g_malloc(size > 0 ? size : 1U);
        g_autoptr(MuxClipboardSnapshot) snapshot =
            mux_clipboard_snapshot_new(200U + case_index);
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(size);
        g_autoptr(MuxClipboardWireTransfer) transfer = NULL;
        g_autoptr(GError) error = NULL;
        PacketSink sink;
        gsize expected_chunks;
        gsize offset = 0;
        guint packet_index;

        for (offset = 0; offset < size; offset++)
            expected[offset] = (guint8)((offset * 37U + 11U) & 0xffU);
        offset = 0;
        add_snapshot_item(snapshot,
                          "application/octet-stream",
                          expected,
                          size);
        mux_clipboard_snapshot_seal(snapshot);

        packet_sink_init(&sink);
        g_assert_true(mux_clipboard_wire_send_snapshot(
            transaction_id,
            MUX_CLIPBOARD_WIRE_FLAG_CURRENT,
            NULL,
            NULL,
            0,
            G_GINT64_CONSTANT(5000000),
            snapshot,
            collect_packet,
            &sink,
            &error));
        g_assert_no_error(error);

        expected_chunks = size == 0
                              ? 0
                              : ((size - 1U) /
                                 MUX_CLIPBOARD_WIRE_MAX_CHUNK) + 1U;
        g_assert_cmpuint(sink.packets->len, ==, expected_chunks + 3U);

        for (packet_index = 0; packet_index < sink.packets->len;
             packet_index++) {
            GBytes *packet = g_ptr_array_index(sink.packets, packet_index);
            const guint8 *packet_data;
            gsize packet_length;
            MuxClipboardWireRecord record = { 0 };

            packet_data = g_bytes_get_data(packet, &packet_length);
            g_assert_true(mux_clipboard_wire_record_decode(packet_data,
                                                           packet_length,
                                                           &record,
                                                           &error));
            g_assert_no_error(error);
            g_assert_cmpuint(record.transaction_id, ==, transaction_id);
            if (packet_index == 0) {
                g_assert_cmpint(record.type,
                                ==,
                                MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN);
            } else if (packet_index == 1U) {
                g_assert_cmpint(record.type,
                                ==,
                                MUX_CLIPBOARD_WIRE_ITEM_BEGIN);
            } else if (packet_index + 1U == sink.packets->len) {
                g_assert_cmpint(record.type,
                                ==,
                                MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT);
            } else {
                const guint8 *chunk;
                gsize chunk_length;
                gsize expected_length =
                    MIN((gsize)MUX_CLIPBOARD_WIRE_MAX_CHUNK,
                        size - offset);

                g_assert_cmpint(record.type,
                                ==,
                                MUX_CLIPBOARD_WIRE_ITEM_DATA);
                g_assert_cmpuint(record.item_index, ==, 0U);
                chunk = g_bytes_get_data(record.payload, &chunk_length);
                g_assert_cmpuint(chunk_length, ==, expected_length);
                g_assert_cmpint(memcmp(chunk,
                                       expected + offset,
                                       expected_length),
                                ==,
                                0);
                offset += chunk_length;
            }
            mux_clipboard_wire_record_clear(&record);
        }
        g_assert_cmpuint(offset, ==, size);

        for (packet_index = 0; packet_index < sink.packets->len;
             packet_index++) {
            MuxClipboardWireTransfer *completed = NULL;
            MuxClipboardWireFeedResult result;

            result = feed_packet(assembler,
                                 g_ptr_array_index(sink.packets,
                                                   packet_index),
                                 G_GINT64_CONSTANT(6000000) + packet_index,
                                 &completed,
                                 &error);
            g_assert_no_error(error);
            if (packet_index + 1U == sink.packets->len) {
                g_assert_cmpint(result,
                                ==,
                                MUX_CLIPBOARD_WIRE_FEED_COMPLETED);
                g_assert_nonnull(completed);
                transfer = completed;
            } else {
                g_assert_cmpint(result,
                                ==,
                                MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
                g_assert_null(completed);
            }
        }
        assert_snapshot_item(
            mux_clipboard_wire_transfer_get_snapshot(transfer),
            "application/octet-stream",
            expected,
            size);
        packet_sink_clear(&sink);
    }
}

static void
test_record_framing(void)
{
    static const guint8 payload_data[] = { 0x00, 0x7f, 0x80, 0xff };
    g_autoptr(GBytes) payload =
        g_bytes_new(payload_data, sizeof(payload_data));
    g_autoptr(GBytes) packet =
        encode_packet(MUX_CLIPBOARD_WIRE_ACK,
                      7,
                      MUX_CLIPBOARD_WIRE_FLAG_PASTE,
                      8,
                      9,
                      10,
                      11,
                      12,
                      payload);
    const guint8 *packet_data;
    gsize packet_length;
    g_autofree guint8 *mutated = NULL;

    packet_data = g_bytes_get_data(packet, &packet_length);
    {
        MuxClipboardWireRecord decoded = { 0 };
        g_autoptr(GError) error = NULL;

        g_assert_true(mux_clipboard_wire_record_decode(packet_data,
                                                       packet_length,
                                                       &decoded,
                                                       &error));
        g_assert_no_error(error);
        g_assert_cmpint(decoded.type, ==, MUX_CLIPBOARD_WIRE_ACK);
        g_assert_cmpuint(decoded.flags,
                         ==,
                         MUX_CLIPBOARD_WIRE_FLAG_PASTE);
        g_assert_cmpuint(decoded.transaction_id, ==, 7U);
        g_assert_cmpuint(decoded.serial, ==, 8U);
        g_assert_cmpuint(decoded.source_view_id, ==, 9U);
        g_assert_cmpint(decoded.created_us, ==, 10);
        g_assert_cmpuint(decoded.item_index, ==, 11U);
        g_assert_cmpuint(decoded.item_count, ==, 12U);
        g_assert_cmpuint(g_bytes_get_size(decoded.payload),
                         ==,
                         sizeof(payload_data));
        mux_clipboard_wire_record_clear(&decoded);
    }

    assert_decode_error(packet_data,
                        MUX_CLIPBOARD_WIRE_HEADER_SIZE - 1U,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    assert_decode_error(packet_data,
                        packet_length - 1U,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    mutated = g_malloc(packet_length);

    memcpy(mutated, packet_data, packet_length);
    write_u32(mutated + 56, (guint32)sizeof(payload_data) + 1U);
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    memcpy(mutated, packet_data, packet_length);
    memset(mutated + 16, 0, sizeof(guint64));
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    memcpy(mutated, packet_data, packet_length);
    write_u16(mutated + 6, 0);
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    memcpy(mutated, packet_data, packet_length);
    write_u32(mutated + 8,
              MUX_CLIPBOARD_WIRE_FLAGS_ALL | (1U << 31));
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    memcpy(mutated, packet_data, packet_length);
    write_u32(mutated + 60, 1);
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    memcpy(mutated, packet_data, packet_length);
    write_u64(mutated + 40, ((guint64)G_MAXINT64) + 1U);
    assert_decode_error(mutated,
                        packet_length,
                        MUX_CLIPBOARD_WIRE_ERROR_INVALID);

    {
        g_autofree guint8 *too_large_data =
            g_malloc0((MUX_CLIPBOARD_WIRE_MAX_PACKET -
                       MUX_CLIPBOARD_WIRE_HEADER_SIZE) + 1U);
        g_autoptr(GBytes) too_large =
            g_bytes_new(too_large_data,
                        (MUX_CLIPBOARD_WIRE_MAX_PACKET -
                         MUX_CLIPBOARD_WIRE_HEADER_SIZE) + 1U);
        MuxClipboardWireRecord record = {
            .type = MUX_CLIPBOARD_WIRE_ACK,
            .transaction_id = 1,
            .payload = too_large
        };
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded =
            mux_clipboard_wire_record_encode(&record, &error);

        g_assert_null(encoded);
        g_assert_error(error,
                       MUX_CLIPBOARD_WIRE_ERROR,
                       MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        MuxClipboardWireRecord record = {
            .type = MUX_CLIPBOARD_WIRE_ACK,
            .flags = 1U << 31,
            .transaction_id = 1
        };
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded =
            mux_clipboard_wire_record_encode(&record, &error);

        g_assert_null(encoded);
        g_assert_error(error,
                       MUX_CLIPBOARD_WIRE_ERROR,
                       MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    }
}

static void
test_invalid_ordering_and_ids(void)
{
    static const guint8 byte = 0x42;
    const gint64 now = G_GINT64_CONSTANT(1000000);

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) data = make_data_packet(1, 0, &byte, 1);

        assert_feed_error(assembler,
                          data,
                          now,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(10,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) wrong_id =
            make_item_begin_packet(11, 0, "text/plain", 1);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          wrong_id,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(20,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) item =
            make_item_begin_packet(20, 1, "text/plain", 1);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          item,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(30,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) data = make_data_packet(30, 0, &byte, 1);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          data,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(40,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) commit =
            make_control_packet(MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT, 40);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          commit,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) first = make_begin_packet(50,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    0,
                                                    0,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) second = make_begin_packet(51,
                                                     2,
                                                     0,
                                                     0,
                                                     now,
                                                     0,
                                                     0,
                                                     NULL,
                                                     NULL);
        g_autoptr(GBytes) cancel =
            make_control_packet(MUX_CLIPBOARD_WIRE_CANCEL, 50);

        assert_feed_without_error(assembler,
                                  first,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          second,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_REJECTED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
        assert_feed_without_error(assembler,
                                  cancel,
                                  now + 2,
                                  MUX_CLIPBOARD_WIRE_FEED_CANCELLED);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(60,
                                                    1,
                                                    0,
                                                    0,
                                                    now,
                                                    0,
                                                    0,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) ack =
            make_control_packet(MUX_CLIPBOARD_WIRE_ACK, 60);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          ack,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }
}

static void
test_limits_and_truncation(void)
{
    const gint64 now = G_GINT64_CONSTANT(2000000);

    {
        guint8 truncated[TEST_BEGIN_FIXED_SIZE - 1U] = { 0 };
        g_autoptr(GBytes) payload =
            g_bytes_new(truncated, sizeof(truncated));
        g_autoptr(GBytes) packet =
            encode_packet(MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN,
                          1,
                          0,
                          0,
                          0,
                          now,
                          0,
                          0,
                          payload);
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);

        assert_feed_error(assembler,
                          packet,
                          now,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(8);
        g_autoptr(GBytes) begin = make_begin_packet(2,
                                                    0,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    9,
                                                    NULL,
                                                    NULL);

        assert_feed_error(assembler,
                          begin,
                          now,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(
            3,
            0,
            0,
            0,
            now,
            MUX_CLIPBOARD_MAX_ITEMS + 1U,
            0,
            NULL,
            NULL);

        assert_feed_error(assembler,
                          begin,
                          now,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        g_autoptr(GBytes) metadata = make_begin_metadata(
            MUX_CLIPBOARD_WIRE_MAX_PROFILE + 1U,
            0,
            0,
            NULL,
            0);
        g_autoptr(GBytes) begin =
            encode_packet(MUX_CLIPBOARD_WIRE_SNAPSHOT_BEGIN,
                          4,
                          0,
                          0,
                          0,
                          now,
                          0,
                          0,
                          metadata);
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);

        assert_feed_error(assembler,
                          begin,
                          now,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        guint8 truncated[TEST_ITEM_FIXED_SIZE - 1U] = { 0 };
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(5,
                                                    0,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) payload =
            g_bytes_new(truncated, sizeof(truncated));
        g_autoptr(GBytes) item =
            encode_packet(MUX_CLIPBOARD_WIRE_ITEM_BEGIN,
                          5,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          payload);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          item,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(6,
                                                    0,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) item =
            make_item_begin_packet(6, 0, "text/plain", 2);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          item,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(
            7,
            0,
            0,
            0,
            now,
            1,
            MUX_CLIPBOARD_MAX_ITEM_BYTES + 1U,
            NULL,
            NULL);
        g_autoptr(GBytes) item = make_item_begin_packet(
            7,
            0,
            "application/octet-stream",
            MUX_CLIPBOARD_MAX_ITEM_BYTES + 1U);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          item,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_LIMIT);
    }

    {
        static const guint8 invalid_mime[] = "text plain";
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(8,
                                                    0,
                                                    0,
                                                    0,
                                                    now,
                                                    1,
                                                    0,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) item = make_item_begin_packet_raw(
            8,
            0,
            sizeof(invalid_mime) - 1U,
            0,
            0,
            invalid_mime,
            sizeof(invalid_mime) - 1U);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          item,
                          now + 1,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(9,
                                                    0,
                                                    0,
                                                    0,
                                                    now,
                                                    2,
                                                    0,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) first =
            make_item_begin_packet(9, 0, "text/plain", 0);
        g_autoptr(GBytes) duplicate =
            make_item_begin_packet(9, 1, "text/plain", 0);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_without_error(assembler,
                                  first,
                                  now + 1,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          duplicate,
                          now + 2,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_INVALID);
    }

    {
        g_autofree guint8 *too_large_chunk =
            g_malloc0(MUX_CLIPBOARD_WIRE_MAX_CHUNK + 1U);
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(
            10,
            0,
            0,
            0,
            now,
            1,
            MUX_CLIPBOARD_WIRE_MAX_CHUNK + 1U,
            NULL,
            NULL);
        g_autoptr(GBytes) item = make_item_begin_packet(
            10,
            0,
            "application/octet-stream",
            MUX_CLIPBOARD_WIRE_MAX_CHUNK + 1U);
        g_autoptr(GBytes) data = make_data_packet(
            10,
            0,
            too_large_chunk,
            MUX_CLIPBOARD_WIRE_MAX_CHUNK + 1U);

        assert_feed_without_error(assembler,
                                  begin,
                                  now,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_without_error(assembler,
                                  item,
                                  now + 1,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_error(assembler,
                          data,
                          now + 2,
                          MUX_CLIPBOARD_WIRE_FEED_CANCELLED,
                          MUX_CLIPBOARD_WIRE_ERROR_STATE);
    }
}

static void
test_timeout_semantics(void)
{
    const gint64 start = G_GINT64_CONSTANT(1000000);
    const gint64 timeout_us =
        (gint64)MUX_CLIPBOARD_WIRE_TIMEOUT_MS * 1000;

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(1,
                                                    0,
                                                    0,
                                                    0,
                                                    start,
                                                    0,
                                                    0,
                                                    NULL,
                                                    NULL);

        g_assert_false(mux_clipboard_wire_assembler_tick(assembler, start));
        assert_feed_without_error(assembler,
                                  begin,
                                  start,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        g_assert_false(mux_clipboard_wire_assembler_tick(
            assembler,
            start + timeout_us - 1));
        g_assert_true(mux_clipboard_wire_assembler_tick(
            assembler,
            start + timeout_us));
        g_assert_false(mux_clipboard_wire_assembler_tick(
            assembler,
            start + timeout_us + 1));
    }

    {
        const gint64 refreshed_at = start + G_GINT64_CONSTANT(2000000);
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(2,
                                                    0,
                                                    0,
                                                    0,
                                                    start,
                                                    1,
                                                    1,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) item =
            make_item_begin_packet(2, 0, "text/plain", 1);

        assert_feed_without_error(assembler,
                                  begin,
                                  start,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_without_error(assembler,
                                  item,
                                  refreshed_at,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        g_assert_false(mux_clipboard_wire_assembler_tick(
            assembler,
            start + timeout_us));
        g_assert_false(mux_clipboard_wire_assembler_tick(
            assembler,
            refreshed_at + timeout_us - 1));
        g_assert_true(mux_clipboard_wire_assembler_tick(
            assembler,
            refreshed_at + timeout_us));
    }

    {
        g_autoptr(MuxClipboardWireAssembler) assembler =
            mux_clipboard_wire_assembler_new(0);
        g_autoptr(GBytes) begin = make_begin_packet(3,
                                                    0,
                                                    0,
                                                    0,
                                                    start,
                                                    0,
                                                    0,
                                                    NULL,
                                                    NULL);
        g_autoptr(GBytes) cancel =
            make_control_packet(MUX_CLIPBOARD_WIRE_CANCEL, 3);

        assert_feed_without_error(assembler,
                                  begin,
                                  start,
                                  MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        assert_feed_without_error(assembler,
                                  cancel,
                                  start + 1,
                                  MUX_CLIPBOARD_WIRE_FEED_CANCELLED);
        g_assert_false(mux_clipboard_wire_assembler_tick(
            assembler,
            start + timeout_us + 1));
    }
}

typedef struct {
    GPtrArray *packets;
    GPtrArray *failure_operations;
    guint failures;
    guint would_block_outputs;
    guint failed_outputs;
} KittyClipboardSink;

static gboolean
collect_kitty_packet(MuxKittyClipboard *clipboard,
                     GBytes *packet,
                     gpointer user_data,
                     GError **error)
{
    KittyClipboardSink *sink = user_data;

    (void)clipboard;
    if (sink->would_block_outputs > 0) {
        sink->would_block_outputs--;
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_WOULD_BLOCK,
                            "test terminal queue is full");
        return FALSE;
    }
    if (sink->failed_outputs > 0) {
        sink->failed_outputs--;
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BROKEN_PIPE,
                            "test terminal transport failed");
        return FALSE;
    }
    g_ptr_array_add(sink->packets, g_bytes_ref(packet));
    return TRUE;
}

static void
collect_kitty_failure(MuxKittyClipboard *clipboard,
                      const gchar *operation,
                      const GError *error,
                      gpointer user_data)
{
    KittyClipboardSink *sink = user_data;

    (void)clipboard;
    (void)error;
    sink->failures++;
    g_ptr_array_add(sink->failure_operations,
                    g_strdup(operation != NULL ? operation : ""));
}

static void
kitty_sink_init(KittyClipboardSink *sink)
{
    sink->packets =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    sink->failure_operations =
        g_ptr_array_new_with_free_func(g_free);
}

static void
kitty_sink_clear(KittyClipboardSink *sink)
{
    g_clear_pointer(&sink->packets, g_ptr_array_unref);
    g_clear_pointer(&sink->failure_operations, g_ptr_array_unref);
}

static gboolean
kitty_sink_has_failure(const KittyClipboardSink *sink,
                       const gchar *operation)
{
    guint i;

    for (i = 0; i < sink->failure_operations->len; i++) {
        if (g_str_equal(g_ptr_array_index(sink->failure_operations, i),
                        operation))
            return TRUE;
    }
    return FALSE;
}

static gsize
kitty_sink_bytes_since(const KittyClipboardSink *sink, guint start)
{
    gsize total = 0;
    guint i;

    for (i = start; i < sink->packets->len; i++)
        total += g_bytes_get_size(g_ptr_array_index(sink->packets, i));
    return total;
}

static gboolean
kitty_sink_contains_since(const KittyClipboardSink *sink,
                          guint start,
                          const gchar *needle)
{
    guint i;

    for (i = start; i < sink->packets->len; i++) {
        GBytes *packet = g_ptr_array_index(sink->packets, i);
        gsize length = 0;
        gconstpointer data = g_bytes_get_data(packet, &length);

        if (g_strstr_len(data, length, needle) != NULL)
            return TRUE;
    }
    return FALSE;
}

static gchar *
kitty_sink_first_write_id(const KittyClipboardSink *sink, guint start)
{
    guint i;

    for (i = start; i < sink->packets->len; i++) {
        GBytes *packet = g_ptr_array_index(sink->packets, i);
        gsize length = 0;
        const gchar *data = g_bytes_get_data(packet, &length);
        const gchar *id;
        const gchar *end;

        if (g_strstr_len(data, length, "type=write") == NULL)
            continue;
        id = g_strstr_len(data, length, ":id=");
        if (id == NULL)
            continue;
        id += strlen(":id=");
        end = id;
        while ((gsize)(end - data) < length && *end != ':' && *end != ';')
            end++;
        if (end > id)
            return g_strndup(id, end - id);
    }
    return NULL;
}

static MuxClipboardSnapshot *
kitty_large_snapshot_new(guint64 serial, guint8 fill)
{
    const gsize length =
        (gsize)MUX_OSC5522_MAX_CHUNK *
        (MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK * 3U);
    MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(serial);
    g_autoptr(GError) error = NULL;
    guint8 *data = g_malloc(length);
    GBytes *bytes;

    memset(data, fill, length);
    bytes = g_bytes_new_take(data, length);
    g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                             "application/octet-stream",
                                             bytes,
                                             &error));
    g_assert_no_error(error);
    g_bytes_unref(bytes);
    mux_clipboard_snapshot_seal(snapshot);
    return snapshot;
}

static MuxClipboardSnapshot *
kitty_text_snapshot_new(guint64 serial,
                        const gchar *plain,
                        const gchar *html)
{
    MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(serial);
    g_autoptr(GError) error = NULL;
    GBytes *bytes;

    bytes = g_bytes_new(plain, strlen(plain));
    g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                             "text/plain",
                                             bytes,
                                             &error));
    g_assert_no_error(error);
    g_bytes_unref(bytes);

    bytes = g_bytes_new(html, strlen(html));
    g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                             "text/html",
                                             bytes,
                                             &error));
    g_assert_no_error(error);
    g_bytes_unref(bytes);

    mux_clipboard_snapshot_seal(snapshot);
    return snapshot;
}

static void
kitty_assert_tick_budget(MuxKittyClipboard *clipboard,
                         KittyClipboardSink *sink)
{
    guint start = sink->packets->len;

    g_assert_cmpuint(mux_kitty_clipboard_tick(clipboard,
                                              g_get_monotonic_time()),
                     ==,
                     0);
    g_assert_cmpuint(sink->packets->len - start,
                     <=,
                     MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK);
    g_assert_cmpuint(kitty_sink_bytes_since(sink, start),
                     <=,
                     MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK);
}

static void
kitty_finish_emission(MuxKittyClipboard *clipboard,
                      KittyClipboardSink *sink,
                      guint start)
{
    guint ticks = 0;

    while (!kitty_sink_contains_since(sink, start, "type=wdata;")) {
        g_assert_cmpuint(ticks++, <, 32);
        kitty_assert_tick_budget(clipboard, sink);
    }
}

static void
kitty_ack_write(MuxKittyClipboard *clipboard, const gchar *id)
{
    g_autofree gchar *response = g_strdup_printf(
        "\033]5522;type=write:status=DONE:id=%s;\033\\",
        id);
    g_autoptr(GError) error = NULL;

    g_assert_true(mux_kitty_clipboard_handle_osc(
        clipboard,
        (const guint8 *)response,
        strlen(response),
        &error));
    g_assert_no_error(error);
}

static void
kitty_reject_write(MuxKittyClipboard *clipboard, const gchar *id)
{
    g_autofree gchar *response = g_strdup_printf(
        "\033]5522;type=write:status=EIO:id=%s;\033\\",
        id);
    g_autoptr(GError) error = NULL;

    g_assert_true(mux_kitty_clipboard_handle_osc(
        clipboard,
        (const guint8 *)response,
        strlen(response),
        &error));
    g_assert_no_error(error);
}

static void
test_kitty_write_retries_would_block(void)
{
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *snapshot;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    snapshot = kitty_text_snapshot_new(20, "retry plain", "<b>retry</b>");
    sink.would_block_outputs = 1;

    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, snapshot, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(sink.packets->len, ==, 0);
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));

    kitty_assert_tick_budget(clipboard, &sink);
    g_assert_cmpuint(sink.packets->len, >, 0);
    kitty_finish_emission(clipboard, &sink, 0);
    id = kitty_sink_first_write_id(&sink, 0);
    g_assert_nonnull(id);
    kitty_ack_write(clipboard, id);
    g_assert_false(mux_kitty_clipboard_write_pending(clipboard));
    g_assert_cmpuint(sink.failures, ==, 0);

    mux_clipboard_snapshot_unref(snapshot);
    kitty_sink_clear(&sink);
}

static void
test_kitty_write_rejection_promotes_latest(void)
{
    static const gchar winning_plain[] = "rejection winner";
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *active;
    MuxClipboardSnapshot *winning;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *active_id = NULL;
    g_autofree gchar *winning64 = NULL;
    guint winning_start;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    active = kitty_large_snapshot_new(21, 0x44);
    winning = kitty_text_snapshot_new(22,
                                      winning_plain,
                                      "<b>rejection winner</b>");
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, active, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, winning, &error));
    g_assert_no_error(error);

    active_id = kitty_sink_first_write_id(&sink, 0);
    g_assert_nonnull(active_id);
    kitty_reject_write(clipboard, active_id);
    g_assert_cmpuint(sink.failures, ==, 1);
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));

    winning_start = sink.packets->len;
    kitty_assert_tick_budget(clipboard, &sink);
    winning64 = g_base64_encode((const guchar *)winning_plain,
                                strlen(winning_plain));
    g_assert_true(kitty_sink_contains_since(&sink,
                                            winning_start,
                                            winning64));

    mux_clipboard_snapshot_unref(winning);
    mux_clipboard_snapshot_unref(active);
    kitty_sink_clear(&sink);
}

static void
test_kitty_write_timeout_promotes_latest(void)
{
    static const gchar winning_plain[] = "timeout winner";
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *active;
    MuxClipboardSnapshot *winning;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *winning64 = NULL;
    guint winning_start;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    active = kitty_large_snapshot_new(23, 0x45);
    winning = kitty_text_snapshot_new(24,
                                      winning_plain,
                                      "<b>timeout winner</b>");
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, active, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, winning, &error));
    g_assert_no_error(error);

    winning_start = sink.packets->len;
    g_assert_cmpuint(mux_kitty_clipboard_tick(clipboard, G_MAXINT64), ==, 1);
    winning64 = g_base64_encode((const guchar *)winning_plain,
                                strlen(winning_plain));
    g_assert_true(kitty_sink_contains_since(&sink,
                                            winning_start,
                                            winning64));
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));
    g_assert_cmpuint(sink.failures, ==, 1);

    mux_clipboard_snapshot_unref(winning);
    mux_clipboard_snapshot_unref(active);
    kitty_sink_clear(&sink);
}

static void
test_kitty_terminal_failure_reports_queued_write(void)
{
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *active;
    MuxClipboardSnapshot *queued;
    g_autoptr(GError) error = NULL;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    active = kitty_large_snapshot_new(25, 0x46);
    queued = kitty_text_snapshot_new(26,
                                     "terminal failure winner",
                                     "<b>terminal failure</b>");
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, active, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, queued, &error));
    g_assert_no_error(error);

    sink.failed_outputs = 1;
    g_assert_cmpuint(mux_kitty_clipboard_tick(clipboard,
                                              g_get_monotonic_time()),
                     ==,
                     0);
    g_assert_false(mux_kitty_clipboard_write_pending(clipboard));
    g_assert_cmpuint(sink.failures, ==, 2);
    g_assert_true(kitty_sink_has_failure(&sink,
                                         "queued-clipboard-write"));
    g_assert_true(kitty_sink_has_failure(&sink, "clipboard-write"));

    mux_clipboard_snapshot_unref(queued);
    mux_clipboard_snapshot_unref(active);
    kitty_sink_clear(&sink);
}

static void
test_kitty_write_is_incremental_and_bounded(void)
{
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *snapshot;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    guint initial_packets;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    snapshot = kitty_large_snapshot_new(1, 0x5a);

    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, snapshot, &error));
    g_assert_no_error(error);
    initial_packets = sink.packets->len;
    g_assert_cmpuint(initial_packets, >, 0);
    g_assert_cmpuint(initial_packets,
                     <=,
                     MUX_KITTY_CLIPBOARD_WRITE_PACKETS_PER_TICK);
    g_assert_cmpuint(kitty_sink_bytes_since(&sink, 0),
                     <=,
                     MUX_KITTY_CLIPBOARD_WRITE_BYTES_PER_TICK);
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));
    g_assert_false(kitty_sink_contains_since(&sink, 0, "type=wdata;"));

    kitty_finish_emission(clipboard, &sink, 0);
    id = kitty_sink_first_write_id(&sink, 0);
    g_assert_nonnull(id);
    kitty_ack_write(clipboard, id);
    g_assert_false(mux_kitty_clipboard_write_pending(clipboard));
    g_assert_cmpuint(sink.failures, ==, 0);

    mux_clipboard_snapshot_unref(snapshot);
    kitty_sink_clear(&sink);
}

static void
test_kitty_write_queue_is_bounded_latest_wins(void)
{
    static const gchar winning_plain[] = "winning plain";
    static const gchar winning_html[] = "<b>winning html</b>";
    KittyClipboardSink sink = { 0 };
    g_autoptr(MuxKittyClipboard) clipboard = NULL;
    MuxClipboardSnapshot *active;
    MuxClipboardSnapshot *superseded;
    MuxClipboardSnapshot *winning;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *active_id = NULL;
    g_autofree gchar *plain64 = NULL;
    g_autofree gchar *html64 = NULL;
    g_autofree gchar *superseded64 = NULL;
    guint winning_start;

    kitty_sink_init(&sink);
    clipboard = mux_kitty_clipboard_new(collect_kitty_packet,
                                        NULL,
                                        collect_kitty_failure,
                                        &sink,
                                        NULL);
    active = kitty_large_snapshot_new(10, 0x41);
    superseded = kitty_text_snapshot_new(11,
                                         "superseded plain",
                                         "<i>superseded html</i>");
    winning = kitty_text_snapshot_new(12, winning_plain, winning_html);

    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, active, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, superseded, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_publish(
        clipboard, MUX_OSC5522_LOCATION_CLIPBOARD, winning, &error));
    g_assert_no_error(error);
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));

    kitty_finish_emission(clipboard, &sink, 0);
    active_id = kitty_sink_first_write_id(&sink, 0);
    g_assert_nonnull(active_id);
    kitty_ack_write(clipboard, active_id);
    g_assert_true(mux_kitty_clipboard_write_pending(clipboard));

    winning_start = sink.packets->len;
    kitty_assert_tick_budget(clipboard, &sink);
    g_assert_true(kitty_sink_contains_since(&sink,
                                            winning_start,
                                            "type=wdata;"));

    plain64 = g_base64_encode((const guchar *)winning_plain,
                              strlen(winning_plain));
    html64 = g_base64_encode((const guchar *)winning_html,
                             strlen(winning_html));
    superseded64 = g_base64_encode((const guchar *)"superseded plain",
                                   strlen("superseded plain"));
    g_assert_true(kitty_sink_contains_since(&sink, winning_start, plain64));
    g_assert_true(kitty_sink_contains_since(&sink, winning_start, html64));
    g_assert_false(kitty_sink_contains_since(&sink,
                                             winning_start,
                                             superseded64));
    g_assert_cmpuint(sink.failures, ==, 0);

    mux_clipboard_snapshot_unref(winning);
    mux_clipboard_snapshot_unref(superseded);
    mux_clipboard_snapshot_unref(active);
    kitty_sink_clear(&sink);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/clipboard/wire/full-mime-binary-round-trip",
                    test_full_mime_binary_round_trip);
    g_test_add_func("/clipboard/wire/chunk-boundaries",
                    test_chunk_boundaries);
    g_test_add_func("/clipboard/wire/record-framing",
                    test_record_framing);
    g_test_add_func("/clipboard/wire/invalid-ordering-and-ids",
                    test_invalid_ordering_and_ids);
    g_test_add_func("/clipboard/wire/limits-and-truncation",
                    test_limits_and_truncation);
    g_test_add_func("/clipboard/wire/timeout-semantics",
                    test_timeout_semantics);
    g_test_add_func("/clipboard/kitty-write/incremental-bounds",
                    test_kitty_write_is_incremental_and_bounded);
    g_test_add_func("/clipboard/kitty-write/latest-wins",
                    test_kitty_write_queue_is_bounded_latest_wins);
    g_test_add_func("/clipboard/kitty-write/retry-would-block",
                    test_kitty_write_retries_would_block);
    g_test_add_func("/clipboard/kitty-write/rejection-promotes-latest",
                    test_kitty_write_rejection_promotes_latest);
    g_test_add_func("/clipboard/kitty-write/timeout-promotes-latest",
                    test_kitty_write_timeout_promotes_latest);
    g_test_add_func("/clipboard/kitty-write/terminal-failure",
                    test_kitty_terminal_failure_reports_queued_write);
    return g_test_run();
}
