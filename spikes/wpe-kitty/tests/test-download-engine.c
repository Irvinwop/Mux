#define MUX_DOWNLOAD_ENGINE_TEST

#include "../mux-download-engine.c"

#include <glib/gstdio.h>

static void
pending_init(PendingDownload *pending)
{
    memset(pending, 0, sizeof(*pending));
    pending->directory_fd = -1;
    pending->reservation_fd = -1;
    pending->partial_fd = -1;
    pending->download_id = G_GUINT64_CONSTANT(0x123456789abcdef0);
}

static void
pending_clear(PendingDownload *pending)
{
    cleanup_files(pending);
    g_clear_pointer(&pending->suggested_filename, g_free);
    g_clear_pointer(&pending->final_path, g_free);
    g_clear_pointer(&pending->partial_path, g_free);
    g_clear_pointer(&pending->final_name, g_free);
    g_clear_pointer(&pending->partial_name, g_free);
    g_clear_pointer(&pending->failure_message, g_free);
}

static void
assert_file_contents(const gchar *path, const gchar *expected)
{
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(contents, ==, expected);
}

static void
test_suggested_filename_sanitization(void)
{
    g_autofree gchar *safe = sanitize_filename("../../.evil\n");
    g_autofree gchar *empty = sanitize_filename("..");
    g_autofree gchar *long_name = g_strnfill(300, 'a');
    g_autofree gchar *bounded = sanitize_filename(long_name);

    g_assert_cmpstr(safe, ==, "download-.evil_");
    g_assert_cmpstr(empty, ==, "download");
    g_assert_cmpuint(strlen(bounded), ==, 200);
}

static void
test_selected_path_validation(void)
{
    g_autofree gchar *directory = NULL;
    g_autofree gchar *valid_path = NULL;
    g_autofree gchar *hidden_path = NULL;
    g_autofree gchar *control_path = NULL;
    g_autofree gchar *validated = NULL;
    g_autoptr(GError) error = NULL;

    directory = g_dir_make_tmp("mux-download-path-XXXXXX", &error);
    g_assert_no_error(error);
    valid_path = g_build_filename(directory, "archive.tar", NULL);
    validated = validated_destination_path(valid_path, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(validated, ==, valid_path);

    g_clear_pointer(&validated, g_free);
    hidden_path = g_build_filename(directory, ".background", NULL);
    validated = validated_destination_path(hidden_path, &error);
    g_assert_null(validated);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    control_path = g_build_filename(directory, "bad\nname", NULL);
    validated = validated_destination_path(control_path, &error);
    g_assert_null(validated);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    validated = validated_destination_path("relative.txt", &error);
    g_assert_null(validated);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    validated = validated_destination_path(directory, &error);
    g_assert_null(validated);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_collision_safe_atomic_completion(void)
{
    static const gchar original_contents[] = "original\n";
    static const gchar download_contents[] = "downloaded\n";
    PendingDownload pending;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *requested = NULL;
    g_autofree gchar *expected_final = NULL;
    g_autoptr(GError) error = NULL;
    gint descriptor;

    pending_init(&pending);
    directory = g_dir_make_tmp("mux-download-finish-XXXXXX", &error);
    g_assert_no_error(error);
    requested = g_build_filename(directory, "report.txt", NULL);
    expected_final =
        g_build_filename(directory, "report (1).txt", NULL);
    g_assert_true(g_file_set_contents(requested,
                                      original_contents,
                                      -1,
                                      &error));
    g_assert_no_error(error);

    g_assert_true(reserve_final_path(&pending, requested, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(pending.final_path, ==, expected_final);
    assert_file_contents(requested, original_contents);
    g_assert_true(choose_partial_path(&pending, &error));
    g_assert_no_error(error);

    descriptor = openat(pending.directory_fd,
                        pending.partial_name,
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                            O_NOFOLLOW,
                        0600);
    g_assert_cmpint(descriptor, >=, 0);
    g_assert_cmpint(write(descriptor,
                          download_contents,
                          sizeof(download_contents) - 1),
                    ==,
                    sizeof(download_contents) - 1);
    pending.partial_fd = descriptor;

    if (!finalize_download(&pending, &error) &&
        g_error_matches(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED)) {
        g_test_skip("kernel does not provide renameat2 exchange");
        g_clear_error(&error);
        pending_clear(&pending);
        g_assert_cmpint(g_remove(requested), ==, 0);
        g_assert_cmpint(g_rmdir(directory), ==, 0);
        return;
    }
    g_assert_no_error(error);
    assert_file_contents(requested, original_contents);
    assert_file_contents(expected_final, download_contents);
    g_assert_false(g_file_test(pending.partial_path,
                               G_FILE_TEST_EXISTS));

    g_assert_cmpint(g_remove(expected_final), ==, 0);
    g_assert_cmpint(g_remove(requested), ==, 0);
    pending_clear(&pending);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void
test_terminal_event_classification(void)
{
    PendingDownload pending;
    g_autoptr(GError) cancelled = NULL;
    g_autoptr(GError) network_failure = NULL;

    pending_init(&pending);
    cancelled = g_error_new_literal(
        WEBKIT_DOWNLOAD_ERROR,
        WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER,
        "cancelled");
    network_failure = g_error_new_literal(G_IO_ERROR,
                                          G_IO_ERROR_FAILED,
                                          "network failed");
    g_assert_cmpint(failure_event_type(&pending, cancelled),
                    ==,
                    MUX_DOWNLOAD_EVENT_CANCELLED);
    g_assert_cmpint(failure_event_type(&pending, network_failure),
                    ==,
                    MUX_DOWNLOAD_EVENT_FAILED);
    pending.failure_message = g_strdup("unsafe destination");
    g_assert_cmpint(failure_event_type(&pending, cancelled),
                    ==,
                    MUX_DOWNLOAD_EVENT_FAILED);
    pending_clear(&pending);
}

static void
test_clipboard_directory_is_private_and_retained(void)
{
    MuxDownloadManager manager = { 0 };
    g_autofree gchar *directory = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    struct stat status;

    manager.clipboard_paths = g_ptr_array_new_with_free_func(g_free);
    g_assert_true(ensure_clipboard_directory(&manager, &error));
    g_assert_no_error(error);
    directory = g_strdup(manager.clipboard_directory);
    g_assert_cmpint(g_stat(directory, &status), ==, 0);
    g_assert_true(S_ISDIR(status.st_mode));
    g_assert_cmpuint(status.st_uid, ==, geteuid());
    g_assert_cmpuint(status.st_mode & 0777, ==, 0700);

    path = g_build_filename(directory, "retained.bin", NULL);
    g_assert_true(g_file_set_contents(path, "file", 4, &error));
    g_assert_no_error(error);
    g_ptr_array_add(manager.clipboard_paths, g_strdup(path));
    cleanup_clipboard_directory(&manager);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));
    g_assert_null(manager.clipboard_directory);
    g_ptr_array_unref(manager.clipboard_paths);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/download/sanitize-suggested-name",
                    test_suggested_filename_sanitization);
    g_test_add_func("/download/validate-selected-path",
                    test_selected_path_validation);
    g_test_add_func("/download/collision-safe-completion",
                    test_collision_safe_atomic_completion);
    g_test_add_func("/download/terminal-events",
                    test_terminal_event_classification);
    g_test_add_func("/download/clipboard-private-retention",
                    test_clipboard_directory_is_private_and_retained);
    return g_test_run();
}
