#include "mux-browser-store.h"

#include <glib/gstdio.h>

static gchar *
temporary_profile(void)
{
    GError *error = NULL;
    gchar *directory = g_dir_make_tmp("mux-browser-store-XXXXXX", &error);

    g_assert_no_error(error);
    g_assert_nonnull(directory);
    return directory;
}

static void
remove_profile(const gchar *directory)
{
    g_autofree gchar *bookmarks = g_build_filename(directory,
                                                    "bookmarks.ini",
                                                    NULL);

    g_remove(bookmarks);
    g_rmdir(directory);
}

static void
test_history_is_bounded_and_private_is_discarded(void)
{
    g_autofree gchar *directory = temporary_profile();
    g_autoptr(MuxBrowserStore) store = NULL;
    g_autoptr(GPtrArray) normal = NULL;
    g_autoptr(GPtrArray) private_entries = NULL;
    guint i;

    store = mux_browser_store_new(directory, NULL);
    for (i = 0; i < MUX_BROWSER_HISTORY_LIMIT + 20; i++) {
        g_autofree gchar *uri = g_strdup_printf("https://example.test/%u", i);

        mux_browser_store_record_navigation(store, 1, FALSE, uri, uri);
    }
    mux_browser_store_record_navigation(store,
                                        2,
                                        TRUE,
                                        "https://private.test/",
                                        "private");

    normal = mux_browser_store_copy_history(store, FALSE, 0);
    private_entries = mux_browser_store_copy_history(store, TRUE, 0);
    g_assert_cmpuint(normal->len, ==, MUX_BROWSER_HISTORY_LIMIT);
    g_assert_cmpstr(((MuxBrowserEntry *)g_ptr_array_index(normal, 0))->uri,
                    ==,
                    "https://example.test/275");
    g_assert_cmpuint(private_entries->len, ==, 0);

    remove_profile(directory);
}

static void
test_private_metadata_is_never_recorded(void)
{
    g_autofree gchar *directory = temporary_profile();
    g_autoptr(MuxBrowserStore) store = mux_browser_store_new(directory, NULL);
    g_autoptr(GPtrArray) history = NULL;
    g_autoptr(GPtrArray) closed = NULL;

    mux_browser_store_record_navigation(store,
                                        99,
                                        TRUE,
                                        "https://private.test/secret",
                                        "private");
    mux_browser_store_close_view(store, 99);

    history = mux_browser_store_copy_history(store, TRUE, 0);
    closed = mux_browser_store_copy_recently_closed(store, TRUE, 0);
    g_assert_cmpuint(history->len, ==, 0);
    g_assert_cmpuint(closed->len, ==, 0);
    g_assert_cmpuint(mux_browser_store_history_count(store, FALSE), ==, 0);
    g_assert_cmpuint(mux_browser_store_recently_closed_count(store, FALSE),
                     ==,
                     0);

    remove_profile(directory);
}

static void
test_recently_closed_is_bounded(void)
{
    g_autofree gchar *directory = temporary_profile();
    g_autoptr(MuxBrowserStore) store = mux_browser_store_new(directory, NULL);
    g_autoptr(GPtrArray) closed = NULL;
    g_autoptr(MuxBrowserEntry) reopened = NULL;
    guint i;

    for (i = 1; i <= MUX_BROWSER_RECENTLY_CLOSED_LIMIT + 5; i++) {
        g_autofree gchar *uri = g_strdup_printf("https://closed.test/%u", i);

        mux_browser_store_record_navigation(store, i, FALSE, uri, uri);
        mux_browser_store_close_view(store, i);
    }
    closed = mux_browser_store_copy_recently_closed(store, FALSE, 0);
    g_assert_cmpuint(closed->len, ==, MUX_BROWSER_RECENTLY_CLOSED_LIMIT);
    reopened = mux_browser_store_take_recently_closed(store, FALSE);
    g_assert_cmpstr(reopened->uri, ==, "https://closed.test/37");
    g_assert_cmpuint(mux_browser_store_recently_closed_count(store, FALSE),
                     ==,
                     MUX_BROWSER_RECENTLY_CLOSED_LIMIT - 1);

    remove_profile(directory);
}

static void
test_bookmarks_persist_but_private_does_not(void)
{
    g_autofree gchar *directory = temporary_profile();
    g_autoptr(MuxBrowserStore) store = mux_browser_store_new(directory, NULL);
    g_autoptr(GPtrArray) bookmarks = NULL;
    GError *error = NULL;

    g_assert_true(mux_browser_store_set_bookmarked(store,
                                                   FALSE,
                                                   "https://example.test/",
                                                   "Example",
                                                   TRUE,
                                                   &error));
    g_assert_no_error(error);
    g_assert_false(mux_browser_store_set_bookmarked(store,
                                                    TRUE,
                                                    "https://private.test/",
                                                    "Private",
                                                    TRUE,
                                                    &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);

    g_clear_pointer(&store, mux_browser_store_free);
    store = mux_browser_store_new(directory, &error);
    g_assert_no_error(error);
    bookmarks = mux_browser_store_copy_bookmarks(store, FALSE, 0);
    g_assert_cmpuint(bookmarks->len, ==, 1);
    g_assert_cmpstr(
        ((MuxBrowserEntry *)g_ptr_array_index(bookmarks, 0))->title,
        ==,
        "Example");
    g_assert_false(mux_browser_store_is_bookmarked(store,
                                                   FALSE,
                                                   "https://private.test/"));

    remove_profile(directory);
}

static void
test_unsafe_uris_are_not_replayed_or_persisted(void)
{
    g_autofree gchar *directory = temporary_profile();
    g_autoptr(MuxBrowserStore) store = mux_browser_store_new(directory, NULL);
    g_autoptr(GPtrArray) history = NULL;
    GError *error = NULL;

    mux_browser_store_record_navigation(store,
                                        1,
                                        FALSE,
                                        "javascript:alert(1)",
                                        "unsafe");
    history = mux_browser_store_copy_history(store, FALSE, 0);
    g_assert_cmpuint(history->len, ==, 0);
    g_assert_false(mux_browser_store_set_bookmarked(store,
                                                    FALSE,
                                                    "file:///etc/passwd",
                                                    "unsafe",
                                                    TRUE,
                                                    &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(mux_browser_store_uri_is_replayable("https:missing-host"));
    g_assert_false(mux_browser_store_uri_is_bookmarkable("http:///path"));

    remove_profile(directory);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/browser-store/history-bounds",
                    test_history_is_bounded_and_private_is_discarded);
    g_test_add_func("/browser-store/private-no-write",
                    test_private_metadata_is_never_recorded);
    g_test_add_func("/browser-store/recently-closed",
                    test_recently_closed_is_bounded);
    g_test_add_func("/browser-store/bookmark-persistence",
                    test_bookmarks_persist_but_private_does_not);
    g_test_add_func("/browser-store/unsafe-uri",
                    test_unsafe_uris_are_not_replayed_or_persisted);
    return g_test_run();
}
