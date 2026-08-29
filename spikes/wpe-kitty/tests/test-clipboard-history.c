#include <glib.h>
#include <string.h>

#include "mux-clipboard-history.h"
#include "mux-clipboard.h"

static MuxClipboardSnapshot *
test_snapshot_new(guint64 serial,
                  const gchar *text,
                  const gchar *html,
                  const guint8 *binary,
                  gsize binary_len)
{
  MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(serial);
  GError *error = NULL;
  GBytes *bytes;

  bytes = g_bytes_new(text, strlen(text));
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "text/plain",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  if (html != NULL) {
    bytes = g_bytes_new(html, strlen(html));
    g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                             "text/html",
                                             bytes,
                                             &error));
    g_assert_no_error(error);
    g_bytes_unref(bytes);
  }

  bytes = g_bytes_new(binary, binary_len);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "application/x-mux-test",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);

  mux_clipboard_snapshot_seal(snapshot);
  return snapshot;
}

static void
assert_bytes_equal(GBytes *actual, const guint8 *expected, gsize expected_len)
{
  gconstpointer actual_data;
  gsize actual_len = 0;

  g_assert_nonnull(actual);
  actual_data = g_bytes_get_data(actual, &actual_len);
  g_assert_cmpuint(actual_len, ==, expected_len);
  g_assert_cmpmem(actual_data, actual_len, expected, expected_len);
}

static void
test_full_mime_and_deduplication(void)
{
  static const guint8 binary[] = { 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff };
  const gchar *text = "plain clipboard text";
  const gchar *html = "<p><strong>clipboard</strong> text</p>";
  MuxClipboardHistory *history;
  MuxClipboardSnapshot *snapshot;
  MuxClipboardSnapshot *selected;
  const MuxClipboardHistoryEntry *entry;
  const MuxClipboardSnapshot *stored;
  MuxClipboardHistoryAddResult result;
  GError *error = NULL;
  guint64 entry_id = 0;
  guint64 duplicate_id = 0;

  history = mux_clipboard_history_new("default", MUX_CLIPBOARD_HISTORY_MEMORY);
  snapshot = test_snapshot_new(41, text, html, binary, sizeof binary);

  result = mux_clipboard_history_add(history,
                                     snapshot,
                                     123456,
                                     "https://example.test",
                                     77,
                                     &entry_id,
                                     &error);
  g_assert_no_error(error);
  g_assert_cmpint(result, ==, MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_cmpuint(entry_id, >, 0);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);

  entry = mux_clipboard_history_lookup(history, entry_id);
  g_assert_nonnull(entry);
  g_assert_cmpint(mux_clipboard_history_entry_get_created_us(entry), ==, 123456);
  g_assert_cmpstr(mux_clipboard_history_entry_get_profile(entry), ==, "default");
  g_assert_cmpstr(mux_clipboard_history_entry_get_source_origin(entry),
                  ==,
                  "https://example.test");
  g_assert_cmpuint(mux_clipboard_history_entry_get_source_view_id(entry), ==, 77);

  stored = mux_clipboard_history_entry_get_snapshot(entry);
  g_assert_cmpuint(mux_clipboard_snapshot_get_count(stored), ==, 3);
  assert_bytes_equal(mux_clipboard_snapshot_find(stored, "text/plain"),
                     (const guint8 *) text,
                     strlen(text));
  assert_bytes_equal(mux_clipboard_snapshot_find(stored, "text/html"),
                     (const guint8 *) html,
                     strlen(html));
  assert_bytes_equal(mux_clipboard_snapshot_find(stored,
                                                  "application/x-mux-test"),
                     binary,
                     sizeof binary);

  selected = mux_clipboard_history_select(history, entry_id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(selected);
  g_assert_true(mux_clipboard_snapshot_is_sealed(selected));
  assert_bytes_equal(mux_clipboard_snapshot_find(selected,
                                                  "application/x-mux-test"),
                     binary,
                     sizeof binary);
  mux_clipboard_snapshot_unref(selected);

  result = mux_clipboard_history_add(history,
                                     snapshot,
                                     999999,
                                     "https://duplicate.test",
                                     88,
                                     &duplicate_id,
                                     &error);
  g_assert_no_error(error);
  g_assert_cmpint(result, ==, MUX_CLIPBOARD_HISTORY_DEDUPLICATED);
  g_assert_cmpuint(duplicate_id, ==, entry_id);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_history_free(history);
}

static void
test_pin_clear_and_delete(void)
{
  static const guint8 first_binary[] = { 0x10 };
  static const guint8 second_binary[] = { 0x20 };
  MuxClipboardHistory *history;
  MuxClipboardSnapshot *first;
  MuxClipboardSnapshot *second;
  GError *error = NULL;
  guint64 first_id = 0;
  guint64 second_id = 0;

  history = mux_clipboard_history_new("work", MUX_CLIPBOARD_HISTORY_MEMORY);
  first = test_snapshot_new(1, "first", NULL, first_binary, sizeof first_binary);
  second = test_snapshot_new(2, "second", NULL, second_binary, sizeof second_binary);

  g_assert_cmpint(mux_clipboard_history_add(history,
                                            first,
                                            1,
                                            "https://one.test",
                                            1,
                                            &first_id,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_cmpint(mux_clipboard_history_add(history,
                                            second,
                                            2,
                                            "https://two.test",
                                            2,
                                            &second_id,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);

  g_assert_true(mux_clipboard_history_set_pinned(history,
                                                 first_id,
                                                 TRUE,
                                                 &error));
  g_assert_no_error(error);
  g_assert_cmpuint(mux_clipboard_history_clear(history, FALSE), ==, 1);
  g_assert_nonnull(mux_clipboard_history_lookup(history, first_id));
  g_assert_null(mux_clipboard_history_lookup(history, second_id));
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);

  g_assert_true(mux_clipboard_history_delete(history, first_id, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 0);

  mux_clipboard_snapshot_unref(first);
  mux_clipboard_snapshot_unref(second);
  mux_clipboard_history_free(history);
}

static void
test_disabled_history_ignores_snapshots(void)
{
  static const guint8 binary[] = { 0xaa, 0xbb };
  MuxClipboardHistory *history;
  MuxClipboardSnapshot *snapshot;
  GError *error = NULL;
  guint64 entry_id = 0;

  history = mux_clipboard_history_new("private", MUX_CLIPBOARD_HISTORY_DISABLED);
  snapshot = test_snapshot_new(1, "private", NULL, binary, sizeof binary);

  g_assert_cmpint(mux_clipboard_history_add(history,
                                            snapshot,
                                            1,
                                            "https://private.test",
                                            1,
                                            &entry_id,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_IGNORED);
  g_assert_no_error(error);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 0);
  g_assert_cmpuint(mux_clipboard_history_get_total_bytes(history), ==, 0);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_history_free(history);
}

static MuxClipboardSnapshot *
test_sized_snapshot_new(guint64 serial,
                        const gchar *mime,
                        gsize length,
                        guint8 fill)
{
  MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(serial);
  GError *error = NULL;
  guint8 *data = g_malloc(length);
  GBytes *bytes;

  memset(data, fill, length);
  bytes = g_bytes_new_take(data, length);
  g_assert_true(mux_clipboard_snapshot_add(snapshot, mime, bytes, &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);
  mux_clipboard_snapshot_seal(snapshot);
  return snapshot;
}

static void
test_namespace_isolation(void)
{
  static const guint8 binary[] = { 0x42 };
  MuxClipboardHistory *persistent;
  MuxClipboardHistory *private_history;
  MuxClipboardHistory *fresh_private;
  MuxClipboardSnapshot *snapshot;
  const MuxClipboardHistoryEntry *entry;
  GError *error = NULL;
  guint64 entry_id = 0;

  persistent = mux_clipboard_history_new_for_namespace(
      "default",
      "shared",
      MUX_CLIPBOARD_HISTORY_SCOPE_PERSISTENT,
      MUX_CLIPBOARD_HISTORY_MEMORY);
  private_history = mux_clipboard_history_new_for_namespace(
      "default",
      "shared",
      MUX_CLIPBOARD_HISTORY_SCOPE_PRIVATE,
      MUX_CLIPBOARD_HISTORY_EPHEMERAL);
  g_assert_nonnull(persistent);
  g_assert_nonnull(private_history);
  g_assert_cmpstr(mux_clipboard_history_get_namespace(persistent),
                  !=,
                  mux_clipboard_history_get_namespace(private_history));

  snapshot = test_snapshot_new(1, "private", NULL, binary, sizeof binary);
  g_assert_cmpint(mux_clipboard_history_add(private_history,
                                            snapshot,
                                            1,
                                            "https://private.test",
                                            4,
                                            &entry_id,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_cmpuint(mux_clipboard_history_get_count(persistent), ==, 0);
  entry = mux_clipboard_history_lookup(private_history, entry_id);
  g_assert_cmpstr(mux_clipboard_history_entry_get_namespace(entry),
                  ==,
                  mux_clipboard_history_get_namespace(private_history));

  fresh_private = mux_clipboard_history_new_for_namespace(
      "default",
      "shared",
      MUX_CLIPBOARD_HISTORY_SCOPE_PRIVATE,
      MUX_CLIPBOARD_HISTORY_EPHEMERAL);
  g_assert_cmpuint(mux_clipboard_history_get_count(fresh_private), ==, 0);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_history_free(fresh_private);
  mux_clipboard_history_free(private_history);
  mux_clipboard_history_free(persistent);
}

static void
test_entry_and_total_bounds(void)
{
  MuxClipboardHistory *history;
  MuxClipboardSnapshot *snapshot;
  GError *error = NULL;
  guint64 first_id = 0;
  guint64 last_id = 0;
  guint i;

  history = mux_clipboard_history_new("bounded",
                                      MUX_CLIPBOARD_HISTORY_MEMORY);
  for (i = 0; i <= MUX_CLIPBOARD_HISTORY_MAX_ENTRIES; i++) {
    g_autofree gchar *text = g_strdup_printf("entry-%u", i);
    static const guint8 binary[] = { 0x01 };

    snapshot = test_snapshot_new(i + 1,
                                 text,
                                 NULL,
                                 binary,
                                 sizeof binary);
    g_assert_cmpint(mux_clipboard_history_add(history,
                                              snapshot,
                                              i + 1,
                                              NULL,
                                              0,
                                              &last_id,
                                              &error),
                    ==,
                    MUX_CLIPBOARD_HISTORY_ADDED);
    g_assert_no_error(error);
    if (i == 0)
      first_id = last_id;
    mux_clipboard_snapshot_unref(snapshot);
  }
  g_assert_cmpuint(mux_clipboard_history_get_count(history),
                   ==,
                   MUX_CLIPBOARD_HISTORY_MAX_ENTRIES);
  g_assert_null(mux_clipboard_history_lookup(history, first_id));
  g_assert_nonnull(mux_clipboard_history_lookup(history, last_id));
  mux_clipboard_history_free(history);

  history = mux_clipboard_history_new("bytes",
                                      MUX_CLIPBOARD_HISTORY_MEMORY);
  snapshot = test_sized_snapshot_new(
      100,
      "application/x-oversized",
      MUX_CLIPBOARD_HISTORY_MAX_BYTES,
      0xaa);
  g_assert_cmpint(mux_clipboard_history_add(history,
                                            snapshot,
                                            1,
                                            NULL,
                                            0,
                                            NULL,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  mux_clipboard_snapshot_unref(snapshot);
  g_assert_cmpuint(mux_clipboard_history_clear(history, TRUE), ==, 1);

  for (i = 0; i < 2; i++) {
    snapshot = test_sized_snapshot_new(
        200 + i,
        i == 0 ? "application/x-first" : "application/x-second",
        MUX_CLIPBOARD_HISTORY_MAX_BYTES / 2,
        (guint8)(i + 1));
    g_assert_cmpint(mux_clipboard_history_add(history,
                                              snapshot,
                                              i + 1,
                                              NULL,
                                              0,
                                              NULL,
                                              &error),
                    ==,
                    MUX_CLIPBOARD_HISTORY_ADDED);
    g_assert_no_error(error);
    mux_clipboard_snapshot_unref(snapshot);
  }
  g_assert_cmpuint(mux_clipboard_history_get_total_bytes(history),
                   ==,
                   MUX_CLIPBOARD_HISTORY_MAX_BYTES);

  snapshot = test_sized_snapshot_new(
      300, "application/x-third", 1, 0x03);
  g_assert_cmpint(mux_clipboard_history_add(history,
                                            snapshot,
                                            3,
                                            NULL,
                                            0,
                                            NULL,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_cmpuint(mux_clipboard_history_get_total_bytes(history),
                   <=,
                   MUX_CLIPBOARD_HISTORY_MAX_BYTES);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 2);

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_history_free(history);
}

static void
test_large_variant_keeps_text_fallback(void)
{
  MuxClipboardHistory *history;
  MuxClipboardSnapshot *snapshot = mux_clipboard_snapshot_new(400);
  const MuxClipboardHistoryEntry *entry;
  const MuxClipboardSnapshot *stored;
  GBytes *bytes;
  GError *error = NULL;
  guint64 entry_id = 0;

  bytes = g_bytes_new_take(g_malloc0(MUX_CLIPBOARD_HISTORY_MAX_BYTES),
                           MUX_CLIPBOARD_HISTORY_MAX_BYTES);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "image/png",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);
  bytes = g_bytes_new_static("fallback", 8);
  g_assert_true(mux_clipboard_snapshot_add(snapshot,
                                           "text/plain",
                                           bytes,
                                           &error));
  g_assert_no_error(error);
  g_bytes_unref(bytes);
  mux_clipboard_snapshot_seal(snapshot);

  history = mux_clipboard_history_new("fallback",
                                      MUX_CLIPBOARD_HISTORY_MEMORY);
  g_assert_cmpint(mux_clipboard_history_add(history,
                                            snapshot,
                                            10,
                                            "https://fallback.test",
                                            1,
                                            &entry_id,
                                            &error),
                  ==,
                  MUX_CLIPBOARD_HISTORY_DEGRADED);
  g_assert_no_error(error);
  entry = mux_clipboard_history_lookup(history, entry_id);
  g_assert_nonnull(entry);
  stored = mux_clipboard_history_entry_get_snapshot(entry);
  g_assert_nonnull(mux_clipboard_snapshot_find(stored, "text/plain"));
  g_assert_null(mux_clipboard_snapshot_find(stored, "image/png"));

  mux_clipboard_snapshot_unref(snapshot);
  mux_clipboard_history_free(history);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/clipboard/history/full-mime-and-deduplication",
                  test_full_mime_and_deduplication);
  g_test_add_func("/clipboard/history/pin-clear-delete",
                  test_pin_clear_and_delete);
  g_test_add_func("/clipboard/history/disabled",
                  test_disabled_history_ignores_snapshots);
  g_test_add_func("/clipboard/history/namespace-isolation",
                  test_namespace_isolation);
  g_test_add_func("/clipboard/history/bounds",
                  test_entry_and_total_bounds);
  g_test_add_func("/clipboard/history/large-variant-text-fallback",
                  test_large_variant_keeps_text_fallback);
  return g_test_run();
}
