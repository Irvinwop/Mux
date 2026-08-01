#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

#include "mux-permission-store.h"

static void
test_private_namespace_never_persists(void)
{
  g_autofree gchar *directory = NULL;
  g_autofree gchar *path = NULL;
  MuxPermissionStore *private_store;
  MuxPermissionStore *fresh_private;
  MuxPermissionStore *persistent_store;
  MuxPermissionStore *reopened;
  MuxPermissionStore *wrong_namespace;
  GError *error = NULL;

  directory = g_dir_make_tmp("mux-permission-store-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(directory);
  path = g_build_filename(directory, "permissions.ini", NULL);

  private_store = mux_permission_store_new_for_namespace(
      directory,
      "shared",
      MUX_PERMISSION_STORE_SCOPE_PRIVATE,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(private_store);
  g_assert_false(mux_permission_store_is_persistent(private_store));
  g_assert_true(mux_permission_store_set(private_store,
                                         "https://private.test",
                                         "camera",
                                         MUX_PERMISSION_DECISION_ALLOW,
                                         &error));
  g_assert_no_error(error);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_assert_cmpint(mux_permission_store_lookup(private_store,
                                              "https://private.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);

  persistent_store = mux_permission_store_new_for_namespace(
      directory,
      "shared",
      MUX_PERMISSION_STORE_SCOPE_PERSISTENT,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(persistent_store);
  g_assert_cmpint(mux_permission_store_lookup(persistent_store,
                                              "https://private.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  g_assert_true(mux_permission_store_set(persistent_store,
                                         "https://persistent.test",
                                         "microphone",
                                         MUX_PERMISSION_DECISION_DENY,
                                         &error));
  g_assert_no_error(error);
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpint(mux_permission_store_lookup(private_store,
                                              "https://persistent.test",
                                              "microphone"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  mux_permission_store_free(persistent_store);

  reopened = mux_permission_store_new_for_namespace(
      directory,
      "shared",
      MUX_PERMISSION_STORE_SCOPE_PERSISTENT,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(reopened);
  g_assert_cmpint(mux_permission_store_lookup(reopened,
                                              "https://persistent.test",
                                              "microphone"),
                  ==,
                  MUX_PERMISSION_DECISION_DENY);

  wrong_namespace = mux_permission_store_new_for_namespace(
      directory,
      "other",
      MUX_PERMISSION_STORE_SCOPE_PERSISTENT,
      &error);
  g_assert_null(wrong_namespace);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  fresh_private = mux_permission_store_new_for_namespace(
      directory,
      "shared",
      MUX_PERMISSION_STORE_SCOPE_PRIVATE,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(fresh_private);
  g_assert_cmpint(mux_permission_store_lookup(fresh_private,
                                              "https://private.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);

  mux_permission_store_free(fresh_private);
  mux_permission_store_free(reopened);
  mux_permission_store_free(private_store);
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_entry_bound_evicts_oldest(void)
{
  MuxPermissionStore *store;
  GError *error = NULL;
  guint i;

  store = mux_permission_store_new_for_namespace(
      NULL,
      "bounded",
      MUX_PERMISSION_STORE_SCOPE_EPHEMERAL,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);

  for (i = 0; i <= MUX_PERMISSION_STORE_MAX_ENTRIES; i++) {
    g_autofree gchar *origin =
        g_strdup_printf("https://%04u.test", i);

    g_assert_true(mux_permission_store_set(store,
                                           origin,
                                           "notifications",
                                           MUX_PERMISSION_DECISION_DENY,
                                           &error));
    g_assert_no_error(error);
  }

  g_assert_cmpuint(mux_permission_store_size(store),
                   ==,
                   MUX_PERMISSION_STORE_MAX_ENTRIES);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://0000.test",
                                              "notifications"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://0512.test",
                                              "notifications"),
                  ==,
                  MUX_PERMISSION_DECISION_DENY);
  mux_permission_store_free(store);
}

static void
test_decision_expiry(void)
{
  MuxPermissionStore *store;
  GError *error = NULL;

  store = mux_permission_store_new_for_namespace(
      NULL,
      "expiry",
      MUX_PERMISSION_STORE_SCOPE_EPHEMERAL,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);

  g_assert_true(mux_permission_store_set_for_duration(
      store,
      "https://expiry.test",
      "camera",
      MUX_PERMISSION_DECISION_DENY,
      1000,
      &error));
  g_assert_no_error(error);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://expiry.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_DENY);
  g_usleep(5000);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://expiry.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  g_assert_cmpuint(mux_permission_store_size(store), ==, 0);

  g_assert_false(mux_permission_store_set_for_duration(
      store,
      "https://expiry.test",
      "camera",
      MUX_PERMISSION_DECISION_DENY,
      MUX_PERMISSION_STORE_MAX_TTL_US + 1,
      &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  mux_permission_store_free(store);
}

static void
test_allow_decisions_are_discarded(void)
{
  g_autofree gchar *directory = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *contents = NULL;
  MuxPermissionStore *store;
  GError *error = NULL;
  gint64 now_us = g_get_real_time();

  directory = g_dir_make_tmp("mux-permission-allow-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(directory);
  path = g_build_filename(directory, "permissions.ini", NULL);
  contents = g_strdup_printf(
      "[mux]\nversion=2\nnamespace=allow-test\n\n"
      "[permission legacy]\n"
      "origin=https://legacy-allow.test\n"
      "category=camera\n"
      "decision=allow\n"
      "updated-us=%" G_GINT64_FORMAT "\n"
      "expires-us=%" G_GINT64_FORMAT "\n",
      now_us,
      now_us + G_TIME_SPAN_DAY);
  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_no_error(error);
  g_assert_cmpint(g_chmod(path, 0600), ==, 0);

  store = mux_permission_store_new_for_namespace(
      directory,
      "allow-test",
      MUX_PERMISSION_STORE_SCOPE_PERSISTENT,
      &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://legacy-allow.test",
                                              "camera"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  g_assert_cmpuint(mux_permission_store_size(store), ==, 0);

  g_assert_true(mux_permission_store_set(store,
                                         "https://new-allow.test",
                                         "microphone",
                                         MUX_PERMISSION_DECISION_ALLOW,
                                         &error));
  g_assert_no_error(error);
  g_assert_cmpint(mux_permission_store_lookup(store,
                                              "https://new-allow.test",
                                              "microphone"),
                  ==,
                  MUX_PERMISSION_DECISION_ASK);
  g_clear_pointer(&contents, g_free);
  g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
  g_assert_no_error(error);
  g_assert_null(strstr(contents, "decision=allow"));

  mux_permission_store_free(store);
  g_assert_cmpint(g_remove(path), ==, 0);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/permission-store/private-namespace",
                  test_private_namespace_never_persists);
  g_test_add_func("/permission-store/entry-bound",
                  test_entry_bound_evicts_oldest);
  g_test_add_func("/permission-store/expiry",
                  test_decision_expiry);
  g_test_add_func("/permission-store/allow-discarded",
                  test_allow_decisions_are_discarded);
  return g_test_run();
}
