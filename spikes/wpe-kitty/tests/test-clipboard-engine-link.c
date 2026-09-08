#include "../mux-clipboard-engine-link.h"

#include <glib.h>

typedef struct {
    GPtrArray *packets;
    GArray *output_targets;
    gboolean switch_on_first_packet;
    guint64 switch_view_id;
    const gchar *switch_origin;
    gboolean switch_ephemeral;
    GMainLoop *publication_loop;
    guint expected_commits;
    guint commits;
    guint failures;
    guint destroy_count;
    gboolean acknowledge_commits;
    gboolean close_on_first_packet;
    MuxClipboardEngineWrite *reentrant_write;
    const MuxClipboardSnapshot *reentrant_snapshot;
    guint paste_attempts;
    guint pastes;
    guint64 paste_target_view_id;
    gboolean reject_paste;
    gpointer replacement_data;
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
    sink->output_targets = g_array_new(FALSE, FALSE, sizeof(guint64));
}

static void
packet_sink_clear(PacketSink *sink)
{
    g_clear_pointer(&sink->packets, g_ptr_array_unref);
    g_clear_pointer(&sink->output_targets, g_array_unref);
}

static void
packet_sink_destroyed(gpointer user_data)
{
    PacketSink *sink = user_data;

    sink->destroy_count++;
}

static void
packet_failure(MuxClipboardEngineLink *link,
               const gchar *operation,
               const GError *error,
               gpointer user_data)
{
    PacketSink *sink = user_data;

    (void)link;
    (void)operation;
    g_assert_nonnull(error);
    sink->failures++;
}

static gboolean
packet_output(MuxClipboardEngineLink *link,
              guint64 target_view_id,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
    PacketSink *sink = user_data;
    MuxClipboardWireRecord record = { 0 };
    const guint8 *data;
    gsize length;

    g_ptr_array_add(sink->packets, g_bytes_ref(packet));
    g_array_append_val(sink->output_targets, target_view_id);
    if (sink->close_on_first_packet && sink->packets->len == 1) {
        sink->close_on_first_packet = FALSE;
        mux_clipboard_engine_link_free(link);
        return TRUE;
    }
    if (sink->reentrant_write != NULL && sink->packets->len == 1) {
        g_autoptr(GError) reentrant_error = NULL;

        g_assert_false(mux_clipboard_engine_link_complete_write(
            link,
            sink->reentrant_write,
            sink->reentrant_snapshot,
            &reentrant_error));
        g_assert_error(reentrant_error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT);
    }
    if (sink->switch_on_first_packet && sink->packets->len == 1) {
        sink->switch_on_first_packet = FALSE;
        if (!mux_clipboard_engine_link_set_active_source(
            link,
            sink->switch_view_id,
            sink->switch_origin,
            sink->switch_ephemeral,
            error))
            return FALSE;
    }
    data = g_bytes_get_data(packet, &length);
    if (!mux_clipboard_wire_record_decode(data, length, &record, error))
        return FALSE;
    if (record.type == MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT) {
        sink->commits++;
        if (sink->acknowledge_commits) {
            MuxClipboardWireRecord ack = {
                .type = MUX_CLIPBOARD_WIRE_ACK,
                .transaction_id = record.transaction_id
            };
            g_autoptr(GBytes) ack_packet =
                mux_clipboard_wire_record_encode(&ack, error);

            mux_clipboard_wire_record_clear(&record);
            if (ack_packet == NULL)
                return FALSE;
            data = g_bytes_get_data(ack_packet, &length);
            if (!mux_clipboard_engine_link_handle_packet(link,
                                                         data,
                                                         length,
                                                         error))
                return FALSE;
        }
        if (sink->publication_loop != NULL &&
            sink->commits >= sink->expected_commits)
            g_main_loop_quit(sink->publication_loop);
    }
    mux_clipboard_wire_record_clear(&record);
    return TRUE;
}

static gboolean
publication_wait_timeout(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static void
wait_for_publications(PacketSink *sink,
                      GMainContext *context,
                      guint expected_commits)
{
    GSource *timeout;

    if (sink->commits >= expected_commits)
        return;
    sink->expected_commits = expected_commits;
    sink->publication_loop = g_main_loop_new(context, FALSE);
    timeout = g_timeout_source_new_seconds(3);
    g_source_set_callback(timeout,
                          publication_wait_timeout,
                          sink->publication_loop,
                          NULL);
    g_source_attach(timeout, context);
    g_main_loop_run(sink->publication_loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    g_clear_pointer(&sink->publication_loop, g_main_loop_unref);
    g_assert_cmpuint(sink->commits, ==, expected_commits);
}

static gboolean
packet_paste(MuxClipboardEngineLink *link,
             guint64 target_view_id,
             const MuxClipboardSnapshot *snapshot,
             gpointer user_data,
             GError **error)
{
    PacketSink *sink = user_data;
    g_autoptr(GBytes) actual = NULL;
    GBytes *expected;

    sink->paste_attempts++;
    sink->paste_target_view_id = target_view_id;
    if (sink->reject_paste) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "the routed target view is no longer owned");
        return FALSE;
    }
    expected = mux_clipboard_snapshot_find(snapshot,
                                            "text/plain;charset=utf-8");
    actual = wpe_clipboard_read_bytes(
        mux_clipboard_engine_link_get_clipboard(link),
        "text/plain;charset=utf-8");
    g_assert_nonnull(expected);
    g_assert_nonnull(actual);
    g_assert_true(g_bytes_equal(expected, actual));
    sink->pastes++;
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
assert_output_targets(const PacketSink *sink, guint64 view_id)
{
    guint i;

    g_assert_cmpuint(sink->output_targets->len, ==, sink->packets->len);
    for (i = 0; i < sink->output_targets->len; i++)
        g_assert_cmpuint(g_array_index(sink->output_targets, guint64, i),
                         ==,
                         view_id);
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

    assert_output_targets(sink, view_id);
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
    g_array_set_size(sink.output_targets, 0);
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
    g_autoptr(GMainContext) context = g_main_context_new();
    g_autoptr(MuxClipboardEngineLink) link = NULL;
    WPEClipboardContent *content = wpe_clipboard_content_new();
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GError) error = NULL;

    g_main_context_push_thread_default(context);
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

    wpe_clipboard_content_set_text(content, "payload");
    wpe_clipboard_set_content(mux_clipboard_engine_link_get_clipboard(link),
                              content);
    wpe_clipboard_content_unref(content);

    /* Reads are synchronous; publication belongs to the captured context. */
    g_assert_cmpuint(sink.packets->len, ==, 0);
    bytes = wpe_clipboard_read_bytes(
        mux_clipboard_engine_link_get_clipboard(link),
        "text/plain;charset=utf-8");
    g_assert_nonnull(bytes);
    g_assert_cmpmem(g_bytes_get_data(bytes, NULL),
                    g_bytes_get_size(bytes),
                    "payload",
                    7);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 73, "https://before-dispatch.test", TRUE, &error));
    g_assert_no_error(error);
    wait_for_publications(&sink, context, 1);
    assert_attribution(&sink,
                       "profile-b",
                       "https://normal-before.test",
                       71,
                       FALSE);
    g_assert_false(sink.switch_on_first_packet);
    g_main_context_pop_thread_default(context);
    packet_sink_clear(&sink);
}

static void
test_wpe_queued_publication_is_cancelled_on_close(void)
{
    PacketSink sink = { 0 };
    g_autoptr(GMainContext) context = g_main_context_new();
    MuxClipboardEngineLink *link;
    g_autoptr(WPEClipboard) clipboard = NULL;
    WPEClipboardContent *content = wpe_clipboard_content_new();

    g_main_context_push_thread_default(context);
    packet_sink_init(&sink);
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-cancel-queued",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         packet_failure,
                                         &sink,
                                         packet_sink_destroyed);
    g_assert_nonnull(link);
    clipboard = g_object_ref(mux_clipboard_engine_link_get_clipboard(link));
    wpe_clipboard_content_set_text(content, "queued copy");
    wpe_clipboard_set_content(clipboard, content);
    g_assert_cmpuint(sink.packets->len, ==, 0);

    /* Cancellation must release the link even if its context never runs. */
    mux_clipboard_engine_link_free(link);
    g_assert_cmpuint(sink.destroy_count, ==, 1);
    g_assert_cmpuint(sink.failures, ==, 0);
    g_assert_false(g_main_context_pending(context));

    /* A retained clipboard cannot call its former publication recipient. */
    wpe_clipboard_set_content(clipboard, content);
    wpe_clipboard_content_unref(content);
    g_assert_false(g_main_context_pending(context));
    g_assert_cmpuint(sink.packets->len, ==, 0);
    g_main_context_pop_thread_default(context);
    packet_sink_clear(&sink);
}

static void
test_write_accepts_synchronous_acknowledgement(void)
{
    PacketSink sink = { 0 };
    g_autoptr(MuxClipboardEngineLink) link = NULL;
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot(10);
    g_autoptr(MuxClipboardEngineWrite) write = NULL;
    g_autoptr(GError) error = NULL;

    packet_sink_init(&sink);
    sink.acknowledge_commits = TRUE;
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-ack",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         packet_failure,
                                         &sink,
                                         NULL);
    g_assert_nonnull(link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 91, "https://ack-target.test", FALSE, &error));
    g_assert_no_error(error);
    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);
    sink.reentrant_write = write;
    sink.reentrant_snapshot = snapshot;
    g_assert_true(mux_clipboard_engine_link_complete_write(link,
                                                          write,
                                                          snapshot,
                                                          &error));
    g_assert_no_error(error);
    g_assert_cmpuint(sink.commits, ==, 1);
    assert_output_targets(&sink, 91);
    g_assert_false(mux_clipboard_engine_link_tick(link, G_MAXINT64));
    g_assert_cmpuint(sink.failures, ==, 0);
    packet_sink_clear(&sink);
}

typedef struct {
    PacketSink old_sink;
    PacketSink new_sink;
    MuxClipboardEngineLink *replacement;
} RecipientReplacement;

static void
replace_recipient_on_destroy(gpointer user_data)
{
    PacketSink *sink = user_data;
    RecipientReplacement *fixture = sink->replacement_data;
    WPEClipboardContent *content = wpe_clipboard_content_new();
    g_autoptr(GError) error = NULL;

    packet_sink_destroyed(sink);
    fixture->replacement = mux_clipboard_engine_link_new(
        test_display(),
        "profile-replacement",
        FALSE,
        packet_output,
        NULL,
        packet_failure,
        &fixture->new_sink,
        NULL);
    g_assert_nonnull(fixture->replacement);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        fixture->replacement,
        302,
        "https://replacement.test",
        FALSE,
        &error));
    g_assert_no_error(error);
    wpe_clipboard_content_set_text(content, "replacement copy");
    wpe_clipboard_set_content(
        mux_clipboard_engine_link_get_clipboard(fixture->replacement),
        content);
    wpe_clipboard_content_unref(content);
}

static void
test_close_preserves_reentrant_replacement_recipient(void)
{
    RecipientReplacement fixture = { 0 };
    g_autoptr(GMainContext) context = g_main_context_new();
    MuxClipboardEngineLink *old_link;
    g_autoptr(WPEClipboard) old_clipboard = NULL;
    WPEClipboardContent *content = wpe_clipboard_content_new();

    g_main_context_push_thread_default(context);
    packet_sink_init(&fixture.old_sink);
    packet_sink_init(&fixture.new_sink);
    fixture.old_sink.replacement_data = &fixture;
    old_link = mux_clipboard_engine_link_new(test_display(),
                                             "profile-old-recipient",
                                             FALSE,
                                             packet_output,
                                             NULL,
                                             packet_failure,
                                             &fixture.old_sink,
                                             replace_recipient_on_destroy);
    g_assert_nonnull(old_link);
    old_clipboard = g_object_ref(
        mux_clipboard_engine_link_get_clipboard(old_link));
    wpe_clipboard_content_set_text(content, "cancelled old copy");
    wpe_clipboard_set_content(old_clipboard, content);
    wpe_clipboard_content_unref(content);

    mux_clipboard_engine_link_free(old_link);
    g_assert_cmpuint(fixture.old_sink.destroy_count, ==, 1);
    g_assert_nonnull(fixture.replacement);
    g_assert_true(old_clipboard !=
        mux_clipboard_engine_link_get_clipboard(fixture.replacement));
    mux_wpe_clipboard_stop_publishing(MUX_WPE_CLIPBOARD(old_clipboard));
    wait_for_publications(&fixture.new_sink, context, 1);
    assert_attribution(&fixture.new_sink,
                       "profile-replacement",
                       "https://replacement.test",
                       302,
                       FALSE);
    g_assert_cmpuint(fixture.old_sink.packets->len, ==, 0);
    g_assert_cmpuint(fixture.old_sink.failures, ==, 0);
    g_assert_cmpuint(fixture.new_sink.failures, ==, 0);

    g_clear_pointer(&fixture.replacement, mux_clipboard_engine_link_free);
    g_main_context_pop_thread_default(context);
    packet_sink_clear(&fixture.old_sink);
    packet_sink_clear(&fixture.new_sink);
}

typedef enum {
    PASTE_CACHE_UNCHANGED,
    PASTE_FOCUS_CHANGED,
    PASTE_TARGET_REMOVED,
    PASTE_CACHE_REPLACED,
    PASTE_LINK_CLOSED
} PasteCacheChange;

typedef struct {
    PacketSink sink;
    MuxClipboardEngineLink *link;
    PasteCacheChange change;
    gulong notify_handler;
    guint notifications;
} IncomingPaste;

static void
change_during_cache_notification(GObject *clipboard,
                                  GParamSpec *property,
                                  gpointer user_data)
{
    IncomingPaste *fixture = user_data;
    g_autoptr(GError) error = NULL;

    (void)property;
    fixture->notifications++;
    g_signal_handler_disconnect(clipboard, fixture->notify_handler);
    fixture->notify_handler = 0;
    if (fixture->change == PASTE_LINK_CLOSED) {
        g_clear_pointer(&fixture->link, mux_clipboard_engine_link_free);
        return;
    }
    if (fixture->change == PASTE_CACHE_REPLACED) {
        g_autoptr(MuxClipboardSnapshot) replacement = test_snapshot(501);

        mux_wpe_clipboard_set_external(MUX_WPE_CLIPBOARD(clipboard),
                                       replacement);
        return;
    }
    if (fixture->change == PASTE_FOCUS_CHANGED ||
        fixture->change == PASTE_TARGET_REMOVED) {
        g_assert_true(mux_clipboard_engine_link_set_active_source(
            fixture->link,
            402,
            "https://other-view.test",
            TRUE,
            &error));
        g_assert_no_error(error);
        fixture->sink.reject_paste =
            fixture->change == PASTE_TARGET_REMOVED;
    }
}

static gboolean
collect_incoming_paste_packet(GBytes *packet,
                              gpointer user_data,
                              GError **error)
{
    GPtrArray *packets = user_data;

    (void)error;
    g_ptr_array_add(packets, g_bytes_ref(packet));
    return TRUE;
}

static void
test_incoming_paste_owns_its_destination(gconstpointer user_data)
{
    IncomingPaste fixture = { 0 };
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot(500);
    g_autoptr(GPtrArray) incoming_packets = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_bytes_unref);
    g_autoptr(WPEClipboard) clipboard = NULL;
    g_autoptr(GError) error = NULL;
    gboolean accepted = FALSE;
    guint i;

    fixture.change = GPOINTER_TO_INT(user_data);
    packet_sink_init(&fixture.sink);
    fixture.link = mux_clipboard_engine_link_new(test_display(),
                                                 "profile-paste",
                                                 FALSE,
                                                 packet_output,
                                                 packet_paste,
                                                 packet_failure,
                                                 &fixture.sink,
                                                 packet_sink_destroyed);
    g_assert_nonnull(fixture.link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        fixture.link, 401, "https://target.test", FALSE, &error));
    g_assert_no_error(error);
    clipboard = g_object_ref(
        mux_clipboard_engine_link_get_clipboard(fixture.link));
    fixture.notify_handler = g_signal_connect(
        clipboard,
        "notify::change-count",
        G_CALLBACK(change_during_cache_notification),
        &fixture);

    /* Historical source metadata must never become a paste destination. */
    g_assert_true(mux_clipboard_wire_send_snapshot(
        700,
        MUX_CLIPBOARD_WIRE_FLAG_CURRENT | MUX_CLIPBOARD_WIRE_FLAG_PASTE,
        "profile-paste",
        "https://historical-source.test",
        499,
        g_get_monotonic_time(),
        snapshot,
        collect_incoming_paste_packet,
        incoming_packets,
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(incoming_packets->len, >, 0);

    /*
     * Receiving a record happens after transport accepts it. An application
     * rejection of COMMIT must not become a send failure that makes the sender
     * manufacture a second CANCEL against an already-finished receiver.
     */
    for (i = 0; i < incoming_packets->len; i++) {
        GBytes *packet = g_ptr_array_index(incoming_packets, i);
        const guint8 *data;
        gsize length;

        data = g_bytes_get_data(packet, &length);
        if (i + 1 == incoming_packets->len) {
            MuxClipboardWireRecord commit = { 0 };

            g_assert_true(mux_clipboard_wire_record_decode(data,
                                                          length,
                                                          &commit,
                                                          &error));
            g_assert_no_error(error);
            g_assert_cmpint(commit.type,
                             ==,
                             MUX_CLIPBOARD_WIRE_SNAPSHOT_COMMIT);
            g_assert_cmpuint(commit.transaction_id, ==, 700);
            mux_clipboard_wire_record_clear(&commit);
        }
        g_assert_nonnull(fixture.link);
        accepted = mux_clipboard_engine_link_handle_packet(fixture.link,
                                                            data,
                                                            length,
                                                            &error);
        if (i + 1 < incoming_packets->len) {
            g_assert_true(accepted);
            g_assert_no_error(error);
        }
    }
    g_assert_cmpuint(fixture.notifications, ==, 1);
    if (fixture.change == PASTE_CACHE_UNCHANGED ||
        fixture.change == PASTE_FOCUS_CHANGED) {
        MuxClipboardWireRecord ack = { 0 };
        GBytes *packet;
        const guint8 *data;
        gsize length;

        g_assert_true(accepted);
        g_assert_no_error(error);
        g_assert_cmpuint(fixture.sink.pastes, ==, 1);
        g_assert_cmpuint(fixture.sink.paste_target_view_id, ==, 401);
        g_assert_cmpuint(fixture.sink.packets->len, ==, 1);
        assert_output_targets(&fixture.sink, 401);
        packet = g_ptr_array_index(fixture.sink.packets, 0);
        data = g_bytes_get_data(packet, &length);
        g_assert_true(mux_clipboard_wire_record_decode(data,
                                                      length,
                                                      &ack,
                                                      &error));
        g_assert_no_error(error);
        g_assert_cmpint(ack.type, ==, MUX_CLIPBOARD_WIRE_ACK);
        g_assert_cmpuint(ack.transaction_id, ==, 700);
        mux_clipboard_wire_record_clear(&ack);
    } else {
        g_assert_false(accepted);
        if (fixture.change == PASTE_TARGET_REMOVED) {
            g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
            g_assert_cmpuint(fixture.sink.paste_attempts, ==, 1);
            g_assert_cmpuint(fixture.sink.paste_target_view_id, ==, 401);
        } else {
            g_assert_cmpuint(fixture.sink.paste_attempts, ==, 0);
            if (fixture.change == PASTE_CACHE_REPLACED)
                g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            else
                g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
        }
        g_assert_cmpuint(fixture.sink.pastes, ==, 0);
        g_assert_cmpuint(fixture.sink.packets->len, ==, 0);
    }

    g_clear_pointer(&fixture.link, mux_clipboard_engine_link_free);
    g_assert_cmpuint(fixture.sink.destroy_count, ==, 1);
    g_assert_cmpuint(fixture.sink.failures, ==, 0);
    packet_sink_clear(&fixture.sink);
}

static void
test_write_can_close_link_during_output(void)
{
    PacketSink sink = { 0 };
    MuxClipboardEngineLink *link;
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot(11);
    MuxClipboardEngineWrite *write;
    g_autoptr(GError) error = NULL;

    packet_sink_init(&sink);
    sink.close_on_first_packet = TRUE;
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-close-output",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         packet_failure,
                                         &sink,
                                         packet_sink_destroyed);
    g_assert_nonnull(link);
    g_assert_true(mux_clipboard_engine_link_set_active_source(
        link, 92, "https://closing-target.test", FALSE, &error));
    g_assert_no_error(error);
    write = mux_clipboard_engine_link_begin_write(link);
    g_assert_nonnull(write);
    g_assert_false(mux_clipboard_engine_link_complete_write(link,
                                                           write,
                                                           snapshot,
                                                           &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED);
    g_assert_cmpuint(sink.packets->len, ==, 1);
    assert_output_targets(&sink, 92);
    g_assert_cmpuint(sink.destroy_count, ==, 0);
    mux_clipboard_engine_write_free(write);
    g_assert_cmpuint(sink.destroy_count, ==, 1);
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

static void
test_external_content_is_readable_through_wpe(void)
{
    PacketSink sink = { 0 };
    g_autoptr(MuxClipboardEngineLink) link = NULL;
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot(9);
    g_autoptr(GBytes) bytes = NULL;
    const gchar *data;
    gsize length;

    packet_sink_init(&sink);
    link = mux_clipboard_engine_link_new(test_display(),
                                         "profile-d",
                                         FALSE,
                                         packet_output,
                                         NULL,
                                         NULL,
                                         &sink,
                                         NULL);
    g_assert_nonnull(link);

    mux_wpe_clipboard_set_external(
        MUX_WPE_CLIPBOARD(mux_clipboard_engine_link_get_clipboard(link)),
        snapshot);
    bytes = wpe_clipboard_read_bytes(
        mux_clipboard_engine_link_get_clipboard(link),
        "text/plain;charset=utf-8");
    g_assert_nonnull(bytes);
    data = g_bytes_get_data(bytes, &length);
    g_assert_cmpuint(length, ==, 7);
    g_assert_cmpmem(data, length, "payload", 7);

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
    g_test_add_func("/clipboard/engine-link/wpe-cancel-queued-publication",
                    test_wpe_queued_publication_is_cancelled_on_close);
    g_test_add_func("/clipboard/engine-link/synchronous-acknowledgement",
                    test_write_accepts_synchronous_acknowledgement);
    g_test_add_func("/clipboard/engine-link/close-during-output",
                    test_write_can_close_link_during_output);
    g_test_add_func("/clipboard/engine-link/close-preserves-replacement",
                    test_close_preserves_reentrant_replacement_recipient);
    g_test_add_data_func("/clipboard/engine-link/paste/routed-target",
                         GINT_TO_POINTER(PASTE_CACHE_UNCHANGED),
                         test_incoming_paste_owns_its_destination);
    g_test_add_data_func("/clipboard/engine-link/paste/focus-changed",
                         GINT_TO_POINTER(PASTE_FOCUS_CHANGED),
                         test_incoming_paste_owns_its_destination);
    g_test_add_data_func("/clipboard/engine-link/paste/target-removed",
                         GINT_TO_POINTER(PASTE_TARGET_REMOVED),
                         test_incoming_paste_owns_its_destination);
    g_test_add_data_func("/clipboard/engine-link/paste/cache-replaced",
                         GINT_TO_POINTER(PASTE_CACHE_REPLACED),
                         test_incoming_paste_owns_its_destination);
    g_test_add_data_func("/clipboard/engine-link/paste/link-closed",
                         GINT_TO_POINTER(PASTE_LINK_CLOSED),
                         test_incoming_paste_owns_its_destination);
    g_test_add_func("/clipboard/engine-link/cancel-lifetime",
                    test_cancelled_write_has_independent_lifetime);
    g_test_add_func("/clipboard/engine-link/wpe-external-public-read",
                    test_external_content_is_readable_through_wpe);
    return g_test_run();
}
