#include "mux-session-state.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>

static void
test_round_trip(void)
{
    g_autoptr(MuxSessionState) source = mux_session_state_new();
    g_autoptr(MuxSessionState) restored = NULL;
    g_autofree gchar *serialized = NULL;
    g_autoptr(GError) error = NULL;
    const MuxSessionView *view;
    gsize length = 0;

    g_assert_true(mux_session_state_set_active_layer(source, "research"));
    g_assert_true(mux_session_state_set_next_view_id(source, 65));
    g_assert_true(mux_session_state_upsert_view(
        source,
        42,
        "research",
        "https://example.test/a?x=1&y=2",
        "Title\nwith punctuation = intact"));

    serialized = mux_session_state_serialize(source, &length, &error);
    g_assert_no_error(error);
    g_assert_nonnull(serialized);
    restored = mux_session_state_deserialize(serialized, length, &error);
    g_assert_no_error(error);
    g_assert_nonnull(restored);
    g_assert_cmpuint(mux_session_state_get_next_view_id(restored), ==, 65);
    g_assert_cmpstr(mux_session_state_get_active_layer(restored),
                    ==,
                    "research");
    g_assert_cmpuint(mux_session_state_get_layer_count(restored), ==, 2);
    g_assert_cmpuint(mux_session_state_get_view_count(restored), ==, 1);
    view = mux_session_state_get_view(restored, 0);
    g_assert_cmpuint(view->id, ==, 42);
    g_assert_cmpstr(view->layer, ==, "research");
    g_assert_cmpstr(view->uri, ==, "https://example.test/a?x=1&y=2");
    g_assert_cmpstr(view->title, ==, "Title\nwith punctuation = intact");
}

static void
test_corrupt_input(void)
{
    const gchar truncated[] =
        "[session]\nversion=1\nnext-view-id=8\nlayer-count=1\n";
    g_autoptr(GError) error = NULL;
    MuxSessionState *state = mux_session_state_deserialize(
        truncated,
        sizeof(truncated) - 1,
        &error);

    g_assert_null(state);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_many_views_and_duplicate_ids(void)
{
    const guint view_count = 4096;
    g_autoptr(MuxSessionState) state = mux_session_state_new();
    g_autofree gchar *serialized = NULL;
    g_autoptr(GError) error = NULL;
    MuxSessionView *last_view;
    gsize length = 0;

    g_assert_true(mux_session_state_set_next_view_id(state,
                                                     view_count + 1));
    for (guint id = 1; id <= view_count; id++) {
        g_assert_true(mux_session_state_upsert_view(state,
                                                   id,
                                                   "main",
                                                   "about:blank",
                                                   ""));
    }

    serialized = mux_session_state_serialize(state, &length, &error);
    g_assert_no_error(error);
    g_assert_nonnull(serialized);
    g_assert_cmpuint(length, >, 0);

    last_view = (MuxSessionView *)mux_session_state_get_view(state,
                                                            view_count - 1);
    g_assert_nonnull(last_view);
    last_view->id = 1;
    g_clear_pointer(&serialized, g_free);

    serialized = mux_session_state_serialize(state, &length, &error);
    g_assert_null(serialized);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpstr(error->message,
                    ==,
                    "session contains duplicate view IDs");
}

static void
test_atomic_file_permissions(void)
{
    g_autoptr(MuxSessionState) source = mux_session_state_new();
    g_autoptr(MuxSessionState) restored = NULL;
    g_autofree gchar *temporary_root = NULL;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    struct stat status;

    temporary_root = g_dir_make_tmp("mux-session-test-XXXXXX", &error);
    g_assert_no_error(error);
    directory = g_build_filename(temporary_root, "state", NULL);
    path = g_build_filename(directory, "workspace-v1.ini", NULL);
    g_assert_true(mux_session_state_save_atomic(source, path, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(directory, &status), ==, 0);
    g_assert_cmpuint(status.st_mode & 0777, ==, 0700);
    g_assert_cmpint(g_stat(path, &status), ==, 0);
    g_assert_cmpuint(status.st_mode & 0777, ==, 0600);

    restored = mux_session_state_load(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(restored);
    g_assert_cmpstr(mux_session_state_get_active_layer(restored), ==, "main");

    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(temporary_root), ==, 0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/session/round-trip", test_round_trip);
    g_test_add_func("/session/corrupt-input", test_corrupt_input);
    g_test_add_func("/session/many-views-and-duplicate-ids",
                    test_many_views_and_duplicate_ids);
    g_test_add_func("/session/atomic-file-permissions",
                    test_atomic_file_permissions);
    return g_test_run();
}
