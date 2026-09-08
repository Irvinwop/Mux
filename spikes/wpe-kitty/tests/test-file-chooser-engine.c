#define MUX_FILE_CHOOSER_ENGINE_TEST

#include "../mux-file-chooser-engine.c"

#include <glib/gstdio.h>

typedef struct {
    GMainLoop *loop;
    GThread *owner_thread;
    guint callbacks;
    gboolean success;
    GError *error;
    StagedSelection *selection;
    const gchar *expected_contents;
    gboolean watchdog_expired;
} StageCapture;

static gboolean
guard_expired(gpointer data)
{
    StageCapture *capture = data;

    capture->watchdog_expired = TRUE;
    g_main_loop_quit(capture->loop);
    return G_SOURCE_REMOVE;
}

static void
run_until_callback(StageCapture *capture)
{
    GSource *guard = g_timeout_source_new_seconds(3);

    capture->watchdog_expired = FALSE;
    g_source_set_callback(guard, guard_expired, capture, NULL);
    g_source_attach(guard, NULL);
    g_main_loop_run(capture->loop);
    g_source_destroy(guard);
    g_source_unref(guard);
    g_assert_false(capture->watchdog_expired);
    g_assert_cmpuint(capture->callbacks, ==, 1);
}

static void
wait_for_atomic(gint *value)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

    while (!g_atomic_int_get(value) &&
           g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        g_usleep(1000);
    }
    g_assert_true(g_atomic_int_get(value));
}

static GPtrArray *
one_path(const gchar *path)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(paths, g_strdup(path));
    return paths;
}

static void
capture_ready(StageOperation *operation, gpointer user_data)
{
    StageCapture *capture = user_data;

    capture->callbacks++;
    g_assert_true(g_thread_self() == capture->owner_thread);
    capture->success = operation->delivery_error == NULL &&
                       operation->worker_result != NULL;
    if (operation->delivery_error)
        capture->error = g_error_copy(operation->delivery_error);
    if (capture->success)
        capture->selection =
            g_steal_pointer(&operation->worker_result);
    if (capture->loop)
        g_main_loop_quit(capture->loop);
}

static StageCapture
capture_new(void)
{
    StageCapture capture = {0};

    capture.loop = g_main_loop_new(NULL, FALSE);
    capture.owner_thread = g_thread_self();
    return capture;
}

static void
capture_clear(StageCapture *capture)
{
    g_clear_error(&capture->error);
    if (capture->selection)
        staged_selection_free(capture->selection);
    g_clear_pointer(&capture->loop, g_main_loop_unref);
}

static void
finish_test_operation(StageOperation *operation,
                      MuxFileChooserBridge *bridge)
{
    wait_for_atomic(&operation->worker_dispatched);
    stage_operation_unref(operation);
    bridge_unref(bridge);
}

static void
test_stalled_worker_times_out_once(void)
{
    g_autoptr(GPtrArray) paths = one_path("/does/not/matter");
    StageCapture capture = capture_new();
    MuxFileChooserBridge *bridge = NULL;
    StageOperation *operation;
    GError *error = NULL;
    int gate[2];
    guint8 release = 1;

    g_assert_cmpint(pipe2(gate, O_CLOEXEC), ==, 0);
    operation = file_chooser_test_stage(&bridge,
                                        paths,
                                        40,
                                        gate[0],
                                        capture_ready,
                                        &capture,
                                        &error);
    g_assert_no_error(error);
    g_assert_nonnull(operation);
    close(gate[0]);
    wait_for_atomic(&operation->stall_entered);
    run_until_callback(&capture);
    g_assert_false(capture.success);
    g_assert_error(capture.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpint(write(gate[1], &release, 1), ==, 1);
    close(gate[1]);
    finish_test_operation(operation, bridge);
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_assert_cmpuint(capture.callbacks, ==, 1);
    capture_clear(&capture);
}

static void
test_stalled_worker_cancels_once(void)
{
    g_autoptr(GPtrArray) paths = one_path("/does/not/matter");
    StageCapture capture = capture_new();
    MuxFileChooserBridge *bridge = NULL;
    StageOperation *operation;
    GError *error = NULL;
    int gate[2];
    guint8 release = 1;

    g_assert_cmpint(pipe2(gate, O_CLOEXEC), ==, 0);
    operation = file_chooser_test_stage(&bridge,
                                        paths,
                                        1000,
                                        gate[0],
                                        capture_ready,
                                        &capture,
                                        &error);
    g_assert_no_error(error);
    g_assert_nonnull(operation);
    close(gate[0]);
    wait_for_atomic(&operation->stall_entered);
    stage_operation_cancel_and_deliver(operation);
    g_assert_cmpuint(capture.callbacks, ==, 1);
    g_assert_false(capture.success);
    g_assert_error(capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpint(write(gate[1], &release, 1), ==, 1);
    close(gate[1]);
    finish_test_operation(operation, bridge);
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_assert_cmpuint(capture.callbacks, ==, 1);
    capture_clear(&capture);
}

static void
test_oversize_sparse_file_is_rejected(void)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GPtrArray) paths = NULL;
    StageCapture capture = capture_new();
    MuxFileChooserBridge *bridge = NULL;
    StageOperation *operation;
    GError *error = NULL;
    int fd;

    path = g_build_filename(g_get_tmp_dir(),
                            "mux-upload-oversize-XXXXXX",
                            NULL);
    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd,
                             (off_t)MUX_UPLOAD_MAX_FILE_BYTES + 1),
                    ==,
                    0);
    close(fd);
    paths = one_path(path);
    operation = file_chooser_test_stage(&bridge,
                                        paths,
                                        1000,
                                        -1,
                                        capture_ready,
                                        &capture,
                                        &error);
    g_assert_no_error(error);
    g_assert_nonnull(operation);
    run_until_callback(&capture);
    g_assert_false(capture.success);
    g_assert_error(capture.error,
                   G_IO_ERROR,
                   G_IO_ERROR_MESSAGE_TOO_LARGE);
    finish_test_operation(operation, bridge);
    g_assert_cmpint(g_remove(path), ==, 0);
    capture_clear(&capture);
}

static void
test_success_is_atomically_published(void)
{
    static const gchar contents[] = "immutable upload snapshot\n";
    g_autofree gchar *directory = NULL;
    g_autofree gchar *source = NULL;
    g_autoptr(GPtrArray) paths = NULL;
    StageCapture capture = capture_new();
    MuxFileChooserBridge *bridge = NULL;
    StageOperation *operation;
    GError *error = NULL;
    const gchar *staged_path;
    g_autofree gchar *staged_contents = NULL;
    g_autofree gchar *item_directory = NULL;
    g_autoptr(GDir) item = NULL;
    const gchar *entry;
    struct stat status;
    guint entries = 0;

    directory = g_dir_make_tmp("mux-upload-source-XXXXXX", &error);
    g_assert_no_error(error);
    source = g_build_filename(directory, "sample.txt", NULL);
    g_assert_true(g_file_set_contents(source,
                                      contents,
                                      sizeof(contents) - 1,
                                      &error));
    g_assert_no_error(error);
    paths = one_path(source);
    operation = file_chooser_test_stage(&bridge,
                                        paths,
                                        1000,
                                        -1,
                                        capture_ready,
                                        &capture,
                                        &error);
    g_assert_no_error(error);
    g_assert_nonnull(operation);
    run_until_callback(&capture);
    g_assert_true(capture.success);
    g_assert_nonnull(capture.selection);
    g_assert_cmpuint(capture.selection->file_paths->len, ==, 1);
    staged_path = g_ptr_array_index(capture.selection->file_paths, 0);
    g_assert_true(g_file_get_contents(staged_path,
                                      &staged_contents,
                                      NULL,
                                      &error));
    g_assert_no_error(error);
    g_assert_cmpstr(staged_contents, ==, contents);
    g_assert_cmpint(lstat(staged_path, &status), ==, 0);
    g_assert_true(S_ISREG(status.st_mode));
    g_assert_cmpuint(status.st_mode & 0777, ==, S_IRUSR);
    item_directory = g_path_get_dirname(staged_path);
    item = g_dir_open(item_directory, 0, &error);
    g_assert_no_error(error);
    g_assert_nonnull(item);
    while ((entry = g_dir_read_name(item))) {
        g_assert_cmpstr(entry, !=, MUX_UPLOAD_TEMP_NAME);
        entries++;
    }
    g_assert_cmpuint(entries, ==, 1);
    finish_test_operation(operation, bridge);
    capture_clear(&capture);
    g_assert_cmpint(g_remove(source), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/file-chooser/stalled-worker-timeout",
                    test_stalled_worker_times_out_once);
    g_test_add_func("/file-chooser/stalled-worker-cancel",
                    test_stalled_worker_cancels_once);
    g_test_add_func("/file-chooser/oversize",
                    test_oversize_sparse_file_is_rejected);
    g_test_add_func("/file-chooser/atomic-success",
                    test_success_is_atomically_published);
    return g_test_run();
}
