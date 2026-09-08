#include <glib.h>
#include <string.h>

#include "mux-clipboard-broker-client.h"
#include "mux-clipboard-broker-peer.h"
#include "mux-clipboard-broker.h"
#include "mux-clipboard.h"

typedef struct {
  MuxClipboardBroker *broker;
  MuxClipboardBrokerPeer *peer;
  MuxClipboardBrokerClient *client;
  MuxClipboardBrokerClient *dispatch_client;
  gboolean ready;
  guint list_callbacks;
  guint list_count;
  guint listed_format_count;
  guint listed_mime_count;
  guint64 listed_first_id;
  gboolean listed_text_plain;
  guint failure_count;
  guint observation_count;
  MuxClipboardBrokerObservationResult last_observation_result;
  guint64 last_observation_id;
  guint output_count;
  guint destroy_client_on_output_at;
  gboolean client_destroyed;
  gboolean defer_peer_output;
  GPtrArray *deferred_peer_packets;
  guint select_callbacks;
  guint64 selected_entry_id;
  gboolean selected_paste;
  MuxClipboardSnapshot *selected_snapshot;
  guint64 last_request_id;
  MuxClipboardControlType last_request_type;
  guint wire_ack_count;
  guint64 last_wire_ack_id;
  gchar *last_failure;
} BrokerHarness;

typedef struct {
  BrokerHarness *harness;
} ObservationContext;

static gboolean
client_output(MuxClipboardBrokerClient *client,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
  BrokerHarness *harness = user_data;
  gconstpointer data;
  gsize size = 0;

  harness->output_count++;
  if (harness->destroy_client_on_output_at != 0 &&
      harness->output_count == harness->destroy_client_on_output_at) {
    harness->destroy_client_on_output_at = 0;
    harness->client = NULL;
    mux_clipboard_broker_client_unref(client);
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        "destructive output callback");
    return FALSE;
  }
  data = g_bytes_get_data(packet, &size);
  return mux_clipboard_broker_peer_handle_packet(harness->peer,
                                                  data,
                                                  size,
                                                  error);
}

static void
client_observed(MuxClipboardBrokerClient *client,
                guint64 transaction_id,
                MuxClipboardBrokerObservationResult result,
                const GError *error,
                gpointer user_data)
{
  ObservationContext *context = user_data;
  BrokerHarness *harness = context->harness;

  (void)client;
  (void)error;
  harness->observation_count++;
  harness->last_observation_id = transaction_id;
  harness->last_observation_result = result;
}

static void
client_destroyed(gpointer user_data)
{
  BrokerHarness *harness = user_data;

  harness->client_destroyed = TRUE;
}

static gboolean
peer_output(MuxClipboardBrokerPeer *peer,
            GBytes *packet,
            gpointer user_data,
            GError **error)
{
  BrokerHarness *harness = user_data;
  MuxClipboardBrokerClient *client;
  gconstpointer data;
  gsize size = 0;

  (void) peer;
  if (harness->defer_peer_output) {
    if (harness->deferred_peer_packets == NULL) {
      harness->deferred_peer_packets = g_ptr_array_new_with_free_func(
          (GDestroyNotify)g_bytes_unref);
    }
    g_ptr_array_add(harness->deferred_peer_packets, g_bytes_ref(packet));
    return TRUE;
  }
  data = g_bytes_get_data(packet, &size);
  client = harness->client != NULL
               ? harness->client
               : harness->dispatch_client;
  return mux_clipboard_broker_client_handle_packet(client,
                                                    data,
                                                    size,
                                                    error);
}

static void
client_ready(MuxClipboardBrokerClient *client, gpointer user_data)
{
  BrokerHarness *harness = user_data;

  (void) client;
  harness->ready = TRUE;
}

static void
client_listed(MuxClipboardBrokerClient *client,
              GPtrArray *summaries,
              gpointer user_data)
{
  BrokerHarness *harness = user_data;
  guint index;

  (void) client;
  harness->list_callbacks++;
  harness->list_count = summaries->len;
  if (summaries->len == 0)
    return;

  {
    MuxClipboardControlSummary *summary =
        g_ptr_array_index(summaries, 0);

    harness->listed_first_id = summary->entry_id;
    harness->listed_format_count = summary->format_count;
    harness->listed_mime_count = summary->mime_type_count;
    for (index = 0; summary->mime_types != NULL &&
                    summary->mime_types[index] != NULL; index++) {
      if (g_str_equal(summary->mime_types[index], "text/plain"))
        harness->listed_text_plain = TRUE;
    }
  }
}

static gboolean
deliver_deferred_peer_packet(BrokerHarness *harness,
                             guint index,
                             GError **error)
{
  GBytes *packet;
  gconstpointer data;
  gsize size = 0;
  gboolean result;

  g_assert_nonnull(harness->deferred_peer_packets);
  g_assert_cmpuint(index, <, harness->deferred_peer_packets->len);
  packet = g_bytes_ref(g_ptr_array_index(harness->deferred_peer_packets,
                                         index));
  g_ptr_array_remove_index(harness->deferred_peer_packets, index);
  data = g_bytes_get_data(packet, &size);
  result = mux_clipboard_broker_client_handle_packet(harness->client,
                                                      data,
                                                      size,
                                                      error);
  g_bytes_unref(packet);
  return result;
}

static void
client_selected(MuxClipboardBrokerClient *client,
                guint64 entry_id,
                const MuxClipboardSnapshot *snapshot,
                gboolean paste,
                gpointer user_data)
{
  BrokerHarness *harness = user_data;

  (void)client;
  harness->select_callbacks++;
  harness->selected_entry_id = entry_id;
  harness->selected_paste = paste;
  g_clear_pointer(&harness->selected_snapshot, mux_clipboard_snapshot_unref);
  harness->selected_snapshot = mux_clipboard_snapshot_dup_sealed(snapshot);
}

static gboolean
capture_selection_output(MuxClipboardBrokerClient *client,
                         GBytes *packet,
                         gpointer user_data,
                         GError **error)
{
  BrokerHarness *harness = user_data;
  MuxExtensionRecord extension = { 0 };
  const guint8 *data;
  gsize size;
  gboolean result;

  (void)client;
  data = g_bytes_get_data(packet, &size);
  if (!mux_extension_record_decode(data, size, &extension, error))
    return FALSE;
  data = g_bytes_get_data(extension.payload, &size);
  if (extension.channel == MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER) {
    MuxClipboardControlRecord record = { 0 };

    result = mux_clipboard_control_record_decode(data, size, &record, error);
    if (result) {
      harness->last_request_id = record.request_id;
      harness->last_request_type = record.type;
    }
    mux_clipboard_control_record_clear(&record);
  } else {
    MuxClipboardWireRecord record = { 0 };

    g_assert_cmpuint(extension.channel, ==, MUX_EXTENSION_CHANNEL_CLIPBOARD);
    result = mux_clipboard_wire_record_decode(data, size, &record, error);
    if (result) {
      g_assert_cmpint(record.type, ==, MUX_CLIPBOARD_WIRE_ACK);
      harness->wire_ack_count++;
      harness->last_wire_ack_id = record.transaction_id;
    }
    mux_clipboard_wire_record_clear(&record);
  }
  mux_extension_record_clear(&extension);
  return result;
}

static gboolean
queue_selection_wire(GBytes *payload, gpointer user_data, GError **error)
{
  MuxExtensionRecord extension = {
    .channel = MUX_EXTENSION_CHANNEL_CLIPBOARD,
    .payload = payload,
  };
  g_autoptr(GBytes) packet = mux_extension_record_encode(&extension, error);

  if (packet == NULL)
    return FALSE;
  return peer_output(NULL, packet, user_data, error);
}

static gboolean
deliver_control_ok(BrokerHarness *harness,
                   guint64 request_id,
                   guint64 entry_id,
                   GError **error)
{
  MuxClipboardControlRecord record = {
    .type = MUX_CLIPBOARD_CONTROL_OK,
    .request_id = request_id,
    .entry_id = entry_id,
  };
  MuxExtensionRecord extension = {
    .channel = MUX_EXTENSION_CHANNEL_CLIPBOARD_BROKER,
  };
  g_autoptr(GBytes) payload = mux_clipboard_control_record_encode(&record,
                                                                error);
  g_autoptr(GBytes) packet = NULL;
  const guint8 *data;
  gsize size;

  if (payload == NULL)
    return FALSE;
  extension.payload = payload;
  packet = mux_extension_record_encode(&extension, error);
  if (packet == NULL)
    return FALSE;
  data = g_bytes_get_data(packet, &size);
  return mux_clipboard_broker_client_handle_packet(harness->client,
                                                   data, size, error);
}

static void
client_failed(MuxClipboardBrokerClient *client,
              const gchar *operation,
              const GError *error,
              gpointer user_data)
{
  BrokerHarness *harness = user_data;

  (void) client;
  harness->failure_count++;
  g_free(harness->last_failure);
  harness->last_failure = g_strdup_printf("%s: %s",
                                          operation,
                                          error->message);
}

static MuxClipboardSnapshot *
broker_snapshot_new(void)
{
  static const guint8 binary[] = { 0x00, 0x42, 0x80, 0xff };
  const gchar *text = "broker round trip";
  MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(55);
  GError *error = NULL;
  GBytes *bytes;

  bytes = g_bytes_new(text, strlen(text));
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "text/plain",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  bytes = g_bytes_new_static(binary, sizeof binary);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "application/x-mux-test",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  mux_clipboard_snapshot_seal(snapshot);
  return snapshot;
}

static MuxClipboardSnapshot *
oversized_history_snapshot_new(void)
{
  MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(56);
  GError *error = NULL;
  GBytes *bytes;

  bytes = g_bytes_new_take(g_malloc0(MUX_CLIPBOARD_HISTORY_MAX_BYTES),
                           MUX_CLIPBOARD_HISTORY_MAX_BYTES);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "application/x-mux-large",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  bytes = g_bytes_new_static("x", 1);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "text/plain",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  mux_clipboard_snapshot_seal(snapshot);
  return snapshot;
}

static void
test_client_peer_round_trip(void)
{
  BrokerHarness harness = { 0 };
  ObservationContext observation_context = { .harness = &harness };
  MuxClipboardSnapshot *snapshot;
  MuxClipboardSnapshot *oversized;
  MuxClipboardSnapshot *current;
  GBytes *binary;
  GError *error = NULL;
  gconstpointer binary_data;
  gsize binary_size = 0;

  harness.broker = mux_clipboard_broker_new();
  harness.peer = mux_clipboard_broker_peer_new(harness.broker,
                                                peer_output,
                                                &harness,
                                                NULL);
  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      client_listed,
      NULL,
      NULL,
      client_failed,
      &harness,
      NULL);
  mux_clipboard_broker_client_set_observation_func(harness.client,
                                                   client_observed,
                                                   &observation_context);

  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);
  g_assert_true(harness.ready);
  g_assert_cmpint(mux_clipboard_broker_client_get_state(harness.client),
                  ==,
                  MUX_CLIPBOARD_BROKER_CLIENT_READY);
  g_assert_cmpstr(mux_clipboard_broker_peer_get_profile(harness.peer),
                  ==,
                  "default");
  g_assert_cmpint(mux_clipboard_broker_peer_get_mode(harness.peer),
                  ==,
                  MUX_CLIPBOARD_HISTORY_MEMORY);
  g_assert_cmpuint(harness.failure_count, ==, 0);

  /* FALSE means an idle peer is healthy and has no expired transfer. */
  g_assert_false(mux_clipboard_broker_peer_tick(harness.peer,
                                                g_get_monotonic_time()));

  snapshot = broker_snapshot_new();
  g_assert_true(mux_clipboard_broker_client_observe(harness.client,
                                                    0,
                                                    "https://broker.test",
                                                    901,
                                                    snapshot,
                                                    &error));
  g_assert_no_error(error);
  g_assert_false(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_cmpuint(harness.observation_count, ==, 1);
  g_assert_cmpint(harness.last_observation_result,
                  ==,
                  MUX_CLIPBOARD_BROKER_OBSERVATION_ACCEPTED);
  g_assert_cmpuint(harness.failure_count, ==, 0);

  current = mux_clipboard_broker_get_current(harness.broker,
                                              "default",
                                              &error);
  g_assert_no_error(error);
  g_assert_nonnull(current);
  g_assert_cmpuint(mux_clipboard_snapshot_get_count(current), ==, 2);
  binary = mux_clipboard_snapshot_find(current, "application/x-mux-test");
  g_assert_nonnull(binary);
  binary_data = g_bytes_get_data(binary, &binary_size);
  g_assert_cmpuint(binary_size, ==, 4);
  g_assert_cmpmem(binary_data,
                  binary_size,
                  ((const guint8[]) { 0x00, 0x42, 0x80, 0xff }),
                  4);
  mux_clipboard_snapshot_unref(current);

  g_assert_true(mux_clipboard_broker_client_list(harness.client, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.list_callbacks, ==, 1);
  g_assert_cmpuint(harness.list_count, ==, 1);
  g_assert_cmpuint(harness.listed_format_count, ==, 2);
  g_assert_cmpuint(harness.listed_mime_count, ==, 2);
  g_assert_true(harness.listed_text_plain);
  g_assert_cmpuint(harness.failure_count, ==, 0);
  g_assert_cmpint(mux_clipboard_broker_client_get_state(harness.client),
                  ==,
                  MUX_CLIPBOARD_BROKER_CLIENT_READY);

  g_assert_false(mux_clipboard_broker_peer_tick(harness.peer,
                                                g_get_monotonic_time()));

  oversized = oversized_history_snapshot_new();
  g_assert_true(mux_clipboard_broker_client_observe(harness.client,
                                                    0,
                                                    "https://large.test",
                                                    902,
                                                    oversized,
                                                    &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.observation_count, ==, 2);
  g_assert_cmpint(harness.last_observation_result,
                  ==,
                  MUX_CLIPBOARD_BROKER_OBSERVATION_DEGRADED);
  g_assert_cmpint(mux_clipboard_broker_client_get_state(harness.client),
                  ==,
                  MUX_CLIPBOARD_BROKER_CLIENT_READY);
  g_assert_false(mux_clipboard_broker_client_request_pending(harness.client));
  mux_clipboard_snapshot_unref(oversized);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_broker_client_unref(harness.client);
  mux_clipboard_broker_peer_unref(harness.peer);
  mux_clipboard_broker_free(harness.broker);
  g_free(harness.last_failure);
}

static void
test_destructive_start_output_callback(void)
{
  BrokerHarness harness = { 0 };
  GError *error = NULL;

  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      NULL,
      NULL,
      NULL,
      client_failed,
      &harness,
      client_destroyed);
  harness.destroy_client_on_output_at = 1;
  g_assert_false(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
  g_assert_true(harness.client_destroyed);
  g_free(harness.last_failure);
}

static void
test_destructive_observe_output_callback(void)
{
  BrokerHarness harness = { 0 };
  MuxClipboardSnapshot *snapshot;
  GError *error = NULL;

  harness.broker = mux_clipboard_broker_new();
  harness.peer = mux_clipboard_broker_peer_new(harness.broker,
                                                peer_output,
                                                &harness,
                                                NULL);
  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      NULL,
      NULL,
      NULL,
      client_failed,
      &harness,
      client_destroyed);
  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);

  snapshot = broker_snapshot_new();
  harness.destroy_client_on_output_at = harness.output_count + 1;
  g_assert_false(mux_clipboard_broker_client_observe(harness.client,
                                                     0,
                                                     "https://destroy.test",
                                                     1,
                                                     snapshot,
                                                     &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
  g_assert_true(harness.client_destroyed);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_broker_peer_unref(harness.peer);
  mux_clipboard_broker_free(harness.broker);
  g_free(harness.last_failure);
}

static void
test_destructive_control_output_callback(void)
{
  BrokerHarness harness = { 0 };
  GError *error = NULL;

  harness.broker = mux_clipboard_broker_new();
  harness.peer = mux_clipboard_broker_peer_new(harness.broker,
                                                peer_output,
                                                &harness,
                                                NULL);
  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      NULL,
      NULL,
      NULL,
      client_failed,
      &harness,
      client_destroyed);
  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);

  harness.destroy_client_on_output_at = harness.output_count + 1;
  g_assert_false(mux_clipboard_broker_client_list(harness.client, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
  g_assert_true(harness.client_destroyed);

  mux_clipboard_broker_peer_unref(harness.peer);
  mux_clipboard_broker_free(harness.broker);
  g_free(harness.last_failure);
}

static void
test_destructive_selection_ack_output_callback(void)
{
  BrokerHarness harness = { 0 };
  MuxClipboardSnapshot *snapshot;
  GError *error = NULL;

  harness.broker = mux_clipboard_broker_new();
  harness.peer = mux_clipboard_broker_peer_new(harness.broker,
                                                peer_output,
                                                &harness,
                                                NULL);
  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      client_listed,
      NULL,
      NULL,
      client_failed,
      &harness,
      client_destroyed);
  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);
  snapshot = broker_snapshot_new();
  g_assert_true(mux_clipboard_broker_client_observe(harness.client,
                                                    0,
                                                    "https://destroy.test",
                                                    2,
                                                    snapshot,
                                                    &error));
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_broker_client_list(harness.client, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.listed_first_id, !=, 0);

  harness.dispatch_client = harness.client;
  harness.destroy_client_on_output_at = harness.output_count + 2;
  g_assert_false(mux_clipboard_broker_client_select(
      harness.client,
      harness.listed_first_id,
      TRUE,
      &error));
  harness.dispatch_client = NULL;
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);
  g_assert_true(harness.client_destroyed);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_broker_peer_unref(harness.peer);
  mux_clipboard_broker_free(harness.broker);
  g_free(harness.last_failure);
}

static void
test_observation_order_and_errors(void)
{
  BrokerHarness harness = { 0 };
  ObservationContext observation_context = { .harness = &harness };
  MuxClipboardSnapshot *snapshot;
  guint64 first_id = 0;
  guint64 second_id = 0;
  guint64 timeout_id = 0;
  GError *error = NULL;

  harness.broker = mux_clipboard_broker_new();
  harness.peer = mux_clipboard_broker_peer_new(harness.broker,
                                                peer_output,
                                                &harness,
                                                NULL);
  harness.client = mux_clipboard_broker_client_new(
      "default",
      MUX_CLIPBOARD_HISTORY_MEMORY,
      client_output,
      client_ready,
      client_listed,
      NULL,
      NULL,
      client_failed,
      &harness,
      NULL);
  mux_clipboard_broker_client_set_observation_func(harness.client,
                                                   client_observed,
                                                   &observation_context);
  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);
  snapshot = broker_snapshot_new();
  g_assert_true(mux_clipboard_broker_client_observe(harness.client,
                                                    0,
                                                    "https://order.test",
                                                    3,
                                                    snapshot,
                                                    &error));
  g_assert_no_error(error);
  harness.observation_count = 0;

  harness.defer_peer_output = TRUE;
  g_assert_true(mux_clipboard_broker_client_list(harness.client, &error));
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_true(mux_clipboard_broker_client_observe_full(
      harness.client,
      0,
      "https://order.test/first",
      4,
      snapshot,
      &first_id,
      &error));
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_broker_client_observe_full(
      harness.client,
      0,
      "https://order.test/second",
      5,
      snapshot,
      &second_id,
      &error));
  g_assert_no_error(error);
  g_assert_cmpuint(first_id, !=, second_id);
  g_assert_cmpuint(harness.deferred_peer_packets->len, ==, 4);

  g_assert_true(deliver_deferred_peer_packet(
      &harness,
      harness.deferred_peer_packets->len - 1,
      &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.observation_count, ==, 1);
  g_assert_cmpuint(harness.last_observation_id, ==, second_id);
  g_assert_true(deliver_deferred_peer_packet(
      &harness,
      harness.deferred_peer_packets->len - 1,
      &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.observation_count, ==, 2);
  g_assert_cmpuint(harness.last_observation_id, ==, first_id);
  g_assert_true(mux_clipboard_broker_client_request_pending(harness.client));

  while (harness.deferred_peer_packets->len > 0) {
    g_assert_true(deliver_deferred_peer_packet(&harness, 0, &error));
    g_assert_no_error(error);
  }
  g_assert_false(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_cmpuint(harness.list_callbacks, ==, 1);
  g_assert_cmpuint(harness.listed_format_count, ==, 2);
  g_assert_cmpuint(harness.listed_mime_count, ==, 2);
  g_assert_true(harness.listed_text_plain);

  harness.observation_count = 0;
  g_assert_true(mux_clipboard_broker_client_observe_full(
      harness.client,
      0,
      "https://order.test/timeout",
      6,
      snapshot,
      &timeout_id,
      &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.deferred_peer_packets->len, ==, 1);
  mux_clipboard_broker_client_tick(harness.client, G_MAXINT64);
  g_assert_cmpuint(harness.observation_count, ==, 1);
  g_assert_cmpuint(harness.last_observation_id, ==, timeout_id);
  g_assert_cmpint(harness.last_observation_result,
                  ==,
                  MUX_CLIPBOARD_BROKER_OBSERVATION_REJECTED);
  g_ptr_array_set_size(harness.deferred_peer_packets, 0);

  mux_clipboard_snapshot_unref(snapshot);
  g_ptr_array_unref(harness.deferred_peer_packets);
  mux_clipboard_broker_client_unref(harness.client);
  mux_clipboard_broker_peer_unref(harness.peer);
  mux_clipboard_broker_free(harness.broker);
  g_free(harness.last_failure);
}

static void
test_expired_selection_cannot_replace_current(gconstpointer user_data)
{
  const gboolean begin_expired_transfer = GPOINTER_TO_INT(user_data);
  BrokerHarness harness = { 0 };
  g_autoptr(MuxClipboardSnapshot) obsolete = broker_snapshot_new();
  g_autoptr(GBytes) replacement_text = g_bytes_new_static("replacement", 11);
  MuxClipboardSnapshotItem item = { "text/plain", replacement_text };
  g_autoptr(MuxClipboardSnapshot) replacement = NULL;
  g_autoptr(GError) error = NULL;
  guint64 expired_request;
  guint64 current_request;
  guint stale_packets;

  replacement = mux_clipboard_snapshot_new_sealed_from_items(99, &item, 1,
                                                             &error);
  g_assert_no_error(error);
  harness.client = mux_clipboard_broker_client_new(
      "default", MUX_CLIPBOARD_HISTORY_MEMORY, capture_selection_output,
      client_ready, NULL, client_selected, NULL, client_failed, &harness, NULL);
  g_assert_true(mux_clipboard_broker_client_start(harness.client, &error));
  g_assert_no_error(error);
  g_assert_cmpint(harness.last_request_type, ==, MUX_CLIPBOARD_CONTROL_HELLO);
  g_assert_true(deliver_control_ok(&harness, harness.last_request_id, 0, &error));
  g_assert_no_error(error);
  g_assert_true(harness.ready);

  harness.defer_peer_output = TRUE;
  g_assert_true(mux_clipboard_broker_client_select(harness.client, 41, TRUE,
                                                   &error));
  g_assert_no_error(error);
  g_assert_cmpint(harness.last_request_type, ==, MUX_CLIPBOARD_CONTROL_SELECT);
  expired_request = harness.last_request_id;
  g_assert_true(mux_clipboard_wire_send_snapshot(
      expired_request,
      MUX_CLIPBOARD_WIRE_FLAG_CURRENT | MUX_CLIPBOARD_WIRE_FLAG_PASTE,
      "default", "https://obsolete.test", 1, 1, obsolete,
      queue_selection_wire, &harness, &error));
  g_assert_no_error(error);
  if (begin_expired_transfer) {
    g_assert_true(deliver_deferred_peer_packet(&harness, 0, &error));
    g_assert_no_error(error);
  }
  g_assert_cmpuint(mux_clipboard_broker_client_tick(harness.client, G_MAXINT64),
                   ==, 1);
  g_assert_cmpuint(harness.failure_count, ==, 1);
  g_assert_cmpuint(harness.select_callbacks, ==, 0);
  g_assert_false(mux_clipboard_broker_client_request_pending(harness.client));

  g_assert_true(mux_clipboard_broker_client_select(harness.client, 99, TRUE,
                                                   &error));
  g_assert_no_error(error);
  current_request = harness.last_request_id;
  g_assert_cmpuint(current_request, !=, expired_request);
  stale_packets = harness.deferred_peer_packets->len;
  g_assert_true(mux_clipboard_wire_send_snapshot(
      current_request,
      MUX_CLIPBOARD_WIRE_FLAG_CURRENT | MUX_CLIPBOARD_WIRE_FLAG_PASTE,
      "default", "https://replacement.test", 2, 2, replacement,
      queue_selection_wire, &harness, &error));
  g_assert_no_error(error);

  /* Start the new transfer, then deliver all obsolete fragments into it. */
  g_assert_true(deliver_deferred_peer_packet(&harness, stale_packets, &error));
  g_assert_no_error(error);
  while (stale_packets-- > 0) {
    g_assert_true(deliver_deferred_peer_packet(&harness, 0, &error));
    g_assert_no_error(error);
  }
  g_assert_true(deliver_control_ok(&harness, expired_request, 41, &error));
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_cmpuint(harness.select_callbacks, ==, 0);
  g_assert_cmpuint(harness.wire_ack_count, ==, 0);

  while (harness.deferred_peer_packets->len > 0) {
    g_assert_true(deliver_deferred_peer_packet(&harness, 0, &error));
    g_assert_no_error(error);
  }
  g_assert_cmpuint(harness.wire_ack_count, ==, 1);
  g_assert_cmpuint(harness.last_wire_ack_id, ==, current_request);
  g_assert_cmpuint(harness.select_callbacks, ==, 0);
  g_assert_true(deliver_control_ok(&harness, expired_request, 41, &error));
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_true(deliver_control_ok(&harness, current_request, 99, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.select_callbacks, ==, 1);
  g_assert_cmpuint(harness.selected_entry_id, ==, 99);
  g_assert_true(harness.selected_paste);
  g_assert_true(g_bytes_equal(
      mux_clipboard_snapshot_find(harness.selected_snapshot, "text/plain"),
      replacement_text));
  g_assert_cmpuint(mux_clipboard_snapshot_get_count(harness.selected_snapshot),
                   ==, 1);
  g_assert_false(mux_clipboard_broker_client_request_pending(harness.client));
  g_assert_cmpuint(harness.failure_count, ==, 1);

  /* Repeated completion packets must not paste the selection twice. */
  g_assert_true(deliver_control_ok(&harness, current_request, 99, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(harness.select_callbacks, ==, 1);

  mux_clipboard_snapshot_unref(harness.selected_snapshot);
  g_ptr_array_unref(harness.deferred_peer_packets);
  mux_clipboard_broker_client_unref(harness.client);
  g_free(harness.last_failure);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/clipboard/broker/client-peer-round-trip",
                  test_client_peer_round_trip);
  g_test_add_func("/clipboard/broker/destructive-start-output",
                  test_destructive_start_output_callback);
  g_test_add_func("/clipboard/broker/destructive-observe-output",
                  test_destructive_observe_output_callback);
  g_test_add_func("/clipboard/broker/destructive-control-output",
                  test_destructive_control_output_callback);
  g_test_add_func("/clipboard/broker/destructive-selection-ack-output",
                  test_destructive_selection_ack_output_callback);
  g_test_add_func("/clipboard/broker/observation-order-and-errors",
                  test_observation_order_and_errors);
  g_test_add_data_func("/clipboard/broker/selection-expired-before-begin",
                       GINT_TO_POINTER(FALSE),
                       test_expired_selection_cannot_replace_current);
  g_test_add_data_func("/clipboard/broker/selection-expired-mid-transfer",
                       GINT_TO_POINTER(TRUE),
                       test_expired_selection_cannot_replace_current);
  return g_test_run();
}
