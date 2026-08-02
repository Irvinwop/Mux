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
  return g_test_run();
}
