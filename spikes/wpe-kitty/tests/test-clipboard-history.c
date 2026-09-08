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

static void
test_degraded_retention_is_order_independent(void)
{
  const gsize variant_size = MUX_CLIPBOARD_HISTORY_MAX_BYTES / 2 + 1;
  g_autoptr(MuxClipboardHistory) history = mux_clipboard_history_new(
      "reordered", MUX_CLIPBOARD_HISTORY_MEMORY);
  g_autoptr(GBytes) text = g_bytes_new_static("copy", 4);
  g_autoptr(GBytes) html = g_bytes_new_static("<b>copy</b>", 11);
  g_autoptr(GBytes) variant = g_bytes_new_take(
      g_malloc0(variant_size), variant_size);
  g_autoptr(GBytes) changed_variant = NULL;
  g_autoptr(MuxClipboardSnapshot) first = NULL;
  g_autoptr(MuxClipboardSnapshot) reordered = NULL;
  g_autoptr(MuxClipboardSnapshot) changed = NULL;
  MuxClipboardSnapshotItem items[] = {
    { "image/png", variant },
    { "text/html", html },
    { "image/jpeg", variant },
    { "text/plain", text },
  };
  MuxClipboardSnapshotItem reversed[G_N_ELEMENTS(items)];
  const MuxClipboardSnapshot *stored;
  g_autoptr(GError) error = NULL;
  guint64 first_id = 0;
  guint64 reordered_id = 0;
  guint64 changed_id = 0;
  guint8 *changed_data;
  guint i;

  for (i = 0; i < G_N_ELEMENTS(items); i++)
    reversed[i] = items[G_N_ELEMENTS(items) - i - 1];
  first = mux_clipboard_snapshot_new_sealed_from_items(
      1, items, G_N_ELEMENTS(items), &error);
  g_assert_no_error(error);
  reordered = mux_clipboard_snapshot_new_sealed_from_items(
      2, reversed, G_N_ELEMENTS(reversed), &error);
  g_assert_no_error(error);

  g_assert_cmpint(mux_clipboard_history_add(history, first, 1, NULL, 1,
                                            &first_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_DEGRADED);
  g_assert_no_error(error);
  g_assert_cmpint(mux_clipboard_history_add(history, reordered, 2, NULL, 2,
                                            &reordered_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_DEGRADED);
  g_assert_no_error(error);
  g_assert_cmpuint(reordered_id, ==, first_id);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);
  stored = mux_clipboard_history_entry_get_snapshot(
      mux_clipboard_history_lookup(history, first_id));
  g_assert_cmpuint(mux_clipboard_snapshot_get_count(stored), ==, 3);
  g_assert_nonnull(mux_clipboard_snapshot_find(stored, "image/jpeg"));
  g_assert_null(mux_clipboard_snapshot_find(stored, "image/png"));
  g_assert_true(g_bytes_equal(mux_clipboard_snapshot_find(stored, "text/plain"),
                              text));
  g_assert_true(g_bytes_equal(mux_clipboard_snapshot_find(stored, "text/html"),
                              html));
  g_assert_cmpuint(mux_clipboard_history_get_total_bytes(history),
                   ==, variant_size + g_bytes_get_size(text) +
                           g_bytes_get_size(html));

  /* Different omitted data must not be mistaken for another copy of this. */
  changed_data = g_malloc0(variant_size);
  changed_data[0] = 1;
  changed_variant = g_bytes_new_take(changed_data, variant_size);
  items[0].bytes = changed_variant;
  changed = mux_clipboard_snapshot_new_sealed_from_items(
      3, items, G_N_ELEMENTS(items), &error);
  g_assert_no_error(error);
  g_assert_cmpint(mux_clipboard_history_add(history, changed, 3, NULL, 3,
                                            &changed_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_DEGRADED);
  g_assert_no_error(error);
  g_assert_cmpuint(changed_id, !=, first_id);
  g_assert_nonnull(mux_clipboard_history_lookup(history, changed_id));
  /* Each retained image takes over half the budget, so the first is evicted. */
  g_assert_null(mux_clipboard_history_lookup(history, first_id));
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);
}

static void
test_deduplication_accepts_borrowed_origin(void)
{
  static const guint8 binary[] = { 0x01 };
  g_autoptr(MuxClipboardHistory) history = mux_clipboard_history_new(
      "borrowed", MUX_CLIPBOARD_HISTORY_MEMORY);
  g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot_new(
      1, "original", NULL, binary, sizeof binary);
  const MuxClipboardHistoryEntry *entry;
  const gchar *origin;
  g_autoptr(GError) error = NULL;
  guint64 entry_id = 0;
  guint64 duplicate_id = 0;

  g_assert_cmpint(mux_clipboard_history_add(history, snapshot, 1,
                                            "https://original.test", 1,
                                            &entry_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  entry = mux_clipboard_history_lookup(history, entry_id);
  origin = mux_clipboard_history_entry_get_source_origin(entry);
  g_assert_cmpint(mux_clipboard_history_add(history, snapshot, 2, origin, 2,
                                            &duplicate_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_DEDUPLICATED);
  g_assert_no_error(error);
  g_assert_cmpuint(duplicate_id, ==, entry_id);
  entry = mux_clipboard_history_lookup(history, entry_id);
  g_assert_cmpstr(mux_clipboard_history_entry_get_source_origin(entry),
                  ==, "https://original.test");
  g_assert_cmpuint(mux_clipboard_history_entry_get_source_view_id(entry), ==, 2);
  g_assert_cmpint(mux_clipboard_history_entry_get_created_us(entry), ==, 2);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 1);
}

static void
test_eviction_accepts_borrowed_origin(void)
{
  static const guint8 binary[] = { 0x01 };
  g_autoptr(MuxClipboardHistory) history = mux_clipboard_history_new(
      "borrowed", MUX_CLIPBOARD_HISTORY_MEMORY);
  g_autoptr(MuxClipboardSnapshot) incoming = test_snapshot_new(
      100, "incoming", NULL, binary, sizeof binary);
  const gchar *origin;
  const MuxClipboardHistoryEntry *entry;
  g_autoptr(GError) error = NULL;
  guint64 oldest_id = 0;
  guint64 entry_id = 0;
  guint i;

  for (i = 0; i < MUX_CLIPBOARD_HISTORY_MAX_ENTRIES; i++) {
    g_autofree gchar *text = g_strdup_printf("entry-%u", i);
    g_autoptr(MuxClipboardSnapshot) snapshot = test_snapshot_new(
        i + 1, text, NULL, binary, sizeof binary);

    g_assert_cmpint(mux_clipboard_history_add(history, snapshot, i + 1,
                                              "https://original.test", i + 1,
                                              &entry_id, &error),
                    ==, MUX_CLIPBOARD_HISTORY_ADDED);
    g_assert_no_error(error);
    if (i == 0)
      oldest_id = entry_id;
  }
  entry = mux_clipboard_history_lookup(history, oldest_id);
  origin = mux_clipboard_history_entry_get_source_origin(entry);
  g_assert_cmpint(mux_clipboard_history_add(history, incoming, 100, origin, 100,
                                            &entry_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_null(mux_clipboard_history_lookup(history, oldest_id));
  entry = mux_clipboard_history_lookup(history, entry_id);
  g_assert_cmpstr(mux_clipboard_history_entry_get_source_origin(entry),
                  ==, "https://original.test");
  g_assert_cmpuint(mux_clipboard_history_get_count(history),
                   ==, MUX_CLIPBOARD_HISTORY_MAX_ENTRIES);
}

static void
test_pinned_capacity_failure_is_atomic(void)
{
  g_autoptr(MuxClipboardHistory) history = mux_clipboard_history_new(
      "pinned", MUX_CLIPBOARD_HISTORY_MEMORY);
  g_autoptr(MuxClipboardSnapshot) pinned = test_sized_snapshot_new(
      1, "application/x-pinned", MUX_CLIPBOARD_HISTORY_MAX_BYTES - 1, 0x01);
  g_autoptr(MuxClipboardSnapshot) unpinned = test_sized_snapshot_new(
      2, "application/x-unpinned", 1, 0x02);
  g_autoptr(MuxClipboardSnapshot) incoming = test_sized_snapshot_new(
      3, "application/x-incoming", 2, 0x03);
  g_autoptr(GError) error = NULL;
  guint64 pinned_id = 0;
  guint64 unpinned_id = 0;
  guint64 rejected_id = 99;

  g_assert_cmpint(mux_clipboard_history_add(history, pinned, 1, NULL, 1,
                                            &pinned_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_true(mux_clipboard_history_set_pinned(history, pinned_id,
                                                 TRUE, &error));
  g_assert_no_error(error);
  g_assert_cmpint(mux_clipboard_history_add(history, unpinned, 2, NULL, 2,
                                            &unpinned_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_ADDED);
  g_assert_no_error(error);
  g_assert_cmpint(mux_clipboard_history_add(history, incoming, 3, NULL, 3,
                                            &rejected_id, &error),
                  ==, MUX_CLIPBOARD_HISTORY_IGNORED);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_assert_cmpuint(rejected_id, ==, 0);
  g_assert_cmpuint(mux_clipboard_history_get_count(history), ==, 2);
  g_assert_cmpuint(mux_clipboard_history_get_total_bytes(history),
                   ==, MUX_CLIPBOARD_HISTORY_MAX_BYTES);
  g_assert_cmpuint(mux_clipboard_history_entry_get_id(
                       mux_clipboard_history_get(history, 0)), ==, unpinned_id);
  g_assert_true(mux_clipboard_history_entry_get_pinned(
      mux_clipboard_history_lookup(history, pinned_id)));
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
  g_test_add_func("/clipboard/history/degraded-order-independent",
                  test_degraded_retention_is_order_independent);
  g_test_add_func("/clipboard/history/deduplication-borrowed-origin",
                  test_deduplication_accepts_borrowed_origin);
  g_test_add_func("/clipboard/history/eviction-borrowed-origin",
                  test_eviction_accepts_borrowed_origin);
  g_test_add_func("/clipboard/history/pinned-capacity-atomic",
                  test_pinned_capacity_failure_is_atomic);
  return g_test_run();
}
