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
  gboolean ready;
  guint list_callbacks;
  guint list_count;
  guint failure_count;
  gchar *last_failure;
} BrokerHarness;

static gboolean
client_output(MuxClipboardBrokerClient *client,
              GBytes *packet,
              gpointer user_data,
              GError **error)
{
  BrokerHarness *harness = user_data;
  gconstpointer data;
  gsize size = 0;

  (void) client;
  data = g_bytes_get_data(packet, &size);
  return mux_clipboard_broker_peer_handle_packet(harness->peer,
                                                  data,
                                                  size,
                                                  error);
}

static gboolean
peer_output(MuxClipboardBrokerPeer *peer,
            GBytes *packet,
            gpointer user_data,
            GError **error)
{
  BrokerHarness *harness = user_data;
  gconstpointer data;
  gsize size = 0;

  (void) peer;
  data = g_bytes_get_data(packet, &size);
  return mux_clipboard_broker_client_handle_packet(harness->client,
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

  (void) client;
  harness->list_callbacks++;
  harness->list_count = summaries->len;
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
  g_assert_cmpuint(harness.failure_count, ==, 0);
  g_assert_cmpint(mux_clipboard_broker_client_get_state(harness.client),
                  ==,
                  MUX_CLIPBOARD_BROKER_CLIENT_READY);

  g_assert_false(mux_clipboard_broker_peer_tick(harness.peer,
                                                g_get_monotonic_time()));

  oversized = oversized_history_snapshot_new();
  g_assert_false(mux_clipboard_broker_client_observe(harness.client,
                                                     0,
                                                     "https://large.test",
                                                     902,
                                                     oversized,
                                                     &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
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

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/clipboard/broker/client-peer-round-trip",
                  test_client_peer_round_trip);
  return g_test_run();
}
