#include "../mux-clipboard-engine-link.h"

#include <glib.h>

typedef struct {
    GPtrArray *packets;
    gboolean switch_on_first_packet;
    guint64 switch_view_id;
    const gchar *switch_origin;
    gboolean switch_ephemeral;
} PacketSink;

static WPEDisplay *
test_display(void)
{
    WPEDisplay *display;

    g_setenv("WPE_DISPLAY", "wpe-display-headless", FALSE);
    display = wpe_display_get_default();
    g_assert_nonnull(display);
    return display;
}

static void
packet_sink_init(PacketSink *sink)
{
    sink->packets = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_bytes_unref);
}

static void
packet_sink_clear(PacketSink *sink)
{
    g_clear_pointer(&sink->packets, g_ptr_array_unref);
}

static gboolean
packet_output(MuxClipboardEngineLink *link,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
    PacketSink *sink = user_data;

    g_ptr_array_add(sink->packets, g_bytes_ref(packet));
    if (sink->switch_on_first_packet && sink->packets->len == 1) {
        sink->switch_on_first_packet = FALSE;
        return mux_clipboard_engine_link_set_active_source(
            link,
            sink->switch_view_id,
            sink->switch_origin,
            sink->switch_ephemeral,
            error);
    }
    return TRUE;
}

static MuxClipboardSnapshot *
test_snapshot(guint64 serial)
{
    g_autoptr(GBytes) bytes = g_bytes_new_static("payload", 7);
    MuxClipboardSnapshotItem item = {
        .mime = "text/plain;charset=utf-8",
        .bytes = bytes,
    };

    return mux_clipboard_snapshot_new_sealed_from_items(serial,
                                                        &item,
                                                        1,
                                                        NULL);
}

static MuxClipboardWireTransfer *
assemble_packets(const PacketSink *sink)
{
    g_autoptr(MuxClipboardWireAssembler) assembler =
        mux_clipboard_wire_assembler_new(0);
    MuxClipboardWireTransfer *transfer = NULL;
    guint i;

    for (i = 0; i < sink->packets->len; i++) {
        GBytes *packet = g_ptr_array_index(sink->packets, i);
        const guint8 *data;
        gsize length;
        g_autoptr(GError) error = NULL;
        MuxClipboardWireTransfer *completed = NULL;
        MuxClipboardWireFeedResult result;

        data = g_bytes_get_data(packet, &length);
        result = mux_clipboard_wire_assembler_feed(
            assembler,
            data,
            length,
            G_GINT64_CONSTANT(1000000) + i,
            &completed,
            &error);
        g_assert_no_error(error);
        if (result == MUX_CLIPBOARD_WIRE_FEED_COMPLETED) {
            g_assert_null(transfer);
            transfer = completed;
        } else {
            g_assert_null(completed);
            g_assert_cmpint(result, ==, MUX_CLIPBOARD_WIRE_FEED_ACCEPTED);
        }
    }
    g_assert_nonnull(transfer);
    return transfer;
}

static void
assert_attribution(const PacketSink *sink,
                   const gchar *profile,
                   const gchar *origin,
                   guint64 view_id,
                   gboolean ephemeral)
{
    g_autoptr(MuxClipboardWireTransfer) transfer =
        assemble_packets(sink);
    guint32 flags = mux_clipboard_wire_transfer_get_flags(transfer);

    g_assert_cmpstr(mux_clipboard_wire_transfer_get_profile(transfer),
                    ==,
                    profile);
    g_assert_cmpstr(mux_clipboard_wire_transfer_get_source_origin(transfer),
                    ==,
                    origin);
    g_assert_cmpuint(
        mux_clipboard_wire_transfer_get_source_view_id(transfer),
        ==,
        view_id);
    g_assert_cmpuint((flags & MUX_CLIPBOARD_WIRE_FLAG_EPHEMERAL) != 0,
                     ==,
                     ephemeral);
}

static void
test_delayed_completion_keeps_original_attribution(void)
{
    PacketSink sink = { 0 };
    g_autoptr(MuxClipboardEngineLink) link = NULL;
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot(1);
    g_autoptr(MuxClipboardEngineWrite) write = NULL;
    g_autoptr(GError) error = NULL;

    packet_sink_init(&sink);
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-a",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         NULL,
                                         &sink,
                                         NULL);
    g_assert_nonnull(link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 11, "https://normal.test", FALSE, &error));
    g_assert_no_error(error);
    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);

    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 22, "https://private.test", TRUE, &error));
    g_assert_no_error(error);
    g_assert_true(mux_clipboard_engine_link_complete_write(link,
                                                          write,
                                                          snapshot,
                                                          &error));
    g_assert_no_error(error);
    assert_attribution(&sink,
                       "profile-a",
                       "https://normal.test",
                       11,
                       FALSE);

    g_ptr_array_set_size(sink.packets, 0);
    g_clear_pointer(&write, mux_clipboard_engine_write_free);
    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 33, "https://normal-again.test", FALSE, &error));
    g_assert_no_error(error);
    g_assert_true(mux_clipboard_engine_link_complete_write(link,
                                                          write,
                                                          snapshot,
                                                          &error));
    g_assert_no_error(error);
    assert_attribution(&sink,
                       "profile-a",
                       "https://private.test",
                       22,
                       TRUE);

    packet_sink_clear(&sink);
}

static void
test_wpe_publication_survives_reentrant_focus_switch(void)
{
    PacketSink sink = { 0 };
    g_autoptr(MuxClipboardEngineLink) link = NULL;
    WPEClipboardContent *content = wpe_clipboard_content_new();
    g_autoptr(GBytes) bytes = g_bytes_new_static("payload", 7);
    g_autoptr(GError) error = NULL;

    packet_sink_init(&sink);
    sink.switch_on_first_packet = TRUE;
    sink.switch_view_id = 72;
    sink.switch_origin = "https://private-after.test";
    sink.switch_ephemeral = TRUE;
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-b",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         NULL,
                                         &sink,
                                         NULL);
    g_assert_nonnull(link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 71, "https://normal-before.test", FALSE, &error));
    g_assert_no_error(error);

    wpe_clipboard_content_set_bytes(content,
                                    "text/plain;charset=utf-8",
                                    bytes);
    wpe_clipboard_set_content(mux_clipboard_engine_link_get_clipboard(link),
                              content);
    wpe_clipboard_content_unref(content);

    assert_attribution(&sink,
                       "profile-b",
                       "https://normal-before.test",
                       71,
                       FALSE);
    packet_sink_clear(&sink);
}

static void
test_cancelled_write_has_independent_lifetime(void)
{
    PacketSink sink = { 0 };
    MuxClipboardEngineLink *link;
    MuxClipboardEngineWrite *write;
    g_autoptr(GError) error = NULL;

    packet_sink_init(&sink);
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-c",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         NULL,
                                         &sink,
                                         NULL);
    g_assert_nonnull(link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 81, "https://cancelled.test", TRUE, &error));
    g_assert_no_error(error);
    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 82, "https://replacement.test", FALSE, &error));
    g_assert_no_error(error);
    mux_clipboard_engine_write_free(write);
    g_assert_cmpuint(sink.packets->len, ==, 0);

    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);
    mux_clipboard_engine_link_free(link);
    mux_clipboard_engine_write_free(write);
    g_assert_cmpuint(sink.packets->len, ==, 0);
    packet_sink_clear(&sink);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/clipboard/engine-link/delayed-focus-switch",
                    test_delayed_completion_keeps_original_attribution);
    g_test_add_func("/clipboard/engine-link/wpe-reentrant-focus-switch",
                    test_wpe_publication_survives_reentrant_focus_switch);
    g_test_add_func("/clipboard/engine-link/cancel-lifetime",
                    test_cancelled_write_has_independent_lifetime);
    return g_test_run();
}
