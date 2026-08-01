#include "mux-kitty-chooser.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    MuxUiRequest *request;
    gint64 expires_at_us;
    gchar *output_path;
    GSubprocess *process;
    GCancellable *wait_cancellable;
    gboolean suspended;
    gboolean suppress_response;
    gboolean timed_out;
} ChooserJob;

struct _MuxKittyChooser {
    grefcount reference_count;
    GQueue queued;
    ChooserJob *active;
    MuxKittyChooserSendFunc send_func;
    MuxKittyChooserStateFunc suspend_func;
    MuxKittyChooserStateFunc resume_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gboolean shutting_down;
};

static MuxKittyChooser *
mux_kitty_chooser_ref(MuxKittyChooser *chooser)
{
    g_ref_count_inc(&chooser->reference_count);
    return chooser;
}

static void
chooser_job_free(ChooserJob *job)
{
    if (!job)
        return;
    if (job->output_path)
        g_unlink(job->output_path);
    mux_ui_request_free(job->request);
    g_clear_object(&job->process);
    g_clear_object(&job->wait_cancellable);
    g_free(job->output_path);
    g_free(job);
}

static void
mux_kitty_chooser_unref(MuxKittyChooser *chooser)
{
    if (!g_ref_count_dec(&chooser->reference_count))
        return;
    g_queue_clear_full(&chooser->queued,
                       (GDestroyNotify)chooser_job_free);
    chooser_job_free(chooser->active);
    if (chooser->user_data_destroy)
        chooser->user_data_destroy(chooser->user_data);
    g_free(chooser);
}

static gboolean
send_response(MuxKittyChooser *chooser,
              guint64 request_id,
              MuxUiAction action,
              GPtrArray *paths,
              GError **error)
{
    g_autoptr(MuxUiResponse) response =
        mux_ui_response_new(request_id, action);
    g_autoptr(GBytes) payload = NULL;
    guint i;

    if (paths) {
        for (i = 0; i < paths->len; i++)
            g_ptr_array_add(
                response->paths,
                g_strdup(g_ptr_array_index(paths, i)));
    }
    payload = mux_ui_response_encode(response, error);
    if (!payload)
        return FALSE;
    return chooser->send_func(payload, chooser->user_data, error);
}

static gboolean
request_is_queued(const MuxKittyChooser *chooser, guint64 request_id)
{
    GList *link;

    if (chooser->active &&
        chooser->active->request->request_id == request_id)
        return TRUE;
    for (link = chooser->queued.head; link; link = link->next) {
        const ChooserJob *job = link->data;

        if (job->request->request_id == request_id)
            return TRUE;
    }
    return FALSE;
}

static guint
pending_count(const MuxKittyChooser *chooser)
{
    return chooser->queued.length + (chooser->active ? 1U : 0U);
}

static gboolean
valid_mime_type(const gchar *mime)
{
    const guchar *cursor;
    gboolean slash = FALSE;

    if (!mime || !*mime || strlen(mime) > 200)
        return FALSE;
    for (cursor = (const guchar *)mime; *cursor; cursor++) {
        if (*cursor == '/') {
            if (slash)
                return FALSE;
            slash = TRUE;
        } else if (!(g_ascii_isalnum(*cursor) || *cursor == '*' ||
                     *cursor == '+' || *cursor == '-' ||
                     *cursor == '.')) {
            return FALSE;
        }
    }
    return slash;
}

static gchar *
start_directory(const MuxUiRequest *request)
{
    if (request->default_value &&
        g_path_is_absolute(request->default_value)) {
        if (g_file_test(request->default_value,
                        G_FILE_TEST_IS_DIR))
            return g_strdup(request->default_value);
        return g_path_get_dirname(request->default_value);
    }
    return g_strdup(g_get_home_dir());
}

static GPtrArray *
chooser_argv(const ChooserJob *job)
{
    const MuxUiRequest *request = job->request;
    GPtrArray *arguments =
        g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *mode = NULL;
    g_autofree gchar *output = NULL;
    g_autofree gchar *title = NULL;
    g_autofree gchar *directory = NULL;
    guint i;

    g_ptr_array_add(arguments, g_strdup("kitten"));
    g_ptr_array_add(arguments, g_strdup("choose-files"));
    mode = g_strdup_printf(
        "--mode=%s",
        request->flags & MUX_UI_REQUEST_FLAG_MULTIPLE
            ? "files"
            : "file");
    g_ptr_array_add(arguments, g_steal_pointer(&mode));
    output = g_strdup_printf(
        "--write-output-to=%s", job->output_path);
    g_ptr_array_add(arguments, g_steal_pointer(&output));
    g_ptr_array_add(arguments, g_strdup("--output-format=text"));
    title = g_strdup_printf(
        "--title=Mux upload: %s",
        request->origin && *request->origin
            ? request->origin
            : "browser");
    g_ptr_array_add(arguments, g_steal_pointer(&title));

    if (request->choices) {
        for (i = 0; i < request->choices->len; i++) {
            const MuxUiChoice *choice =
                g_ptr_array_index(request->choices, i);

            if (choice && valid_mime_type(choice->label)) {
                gchar *filter = g_strdup_printf(
                    "--file-filter=mime:%s:%s",
                    choice->label,
                    choice->label);

                g_ptr_array_add(arguments, filter);
            }
        }
    }
    directory = start_directory(request);
    g_ptr_array_add(arguments, g_steal_pointer(&directory));
    g_ptr_array_add(arguments, NULL);
    return arguments;
}

static void start_next(MuxKittyChooser *chooser);

static void
warn_async_error(const gchar *operation, GError *error)
{
    if (error)
        g_warning("%s: %s", operation, error->message);
}

static gboolean
resume_pane(MuxKittyChooser *chooser, ChooserJob *job)
{
    g_autoptr(GError) error = NULL;
    gboolean resumed = TRUE;

    if (!job->suspended)
        return TRUE;
    job->suspended = FALSE;
    if (chooser->resume_func)
        resumed = chooser->resume_func(
            chooser->user_data, &error);
    if (!resumed)
        warn_async_error("could not resume browser pane", error);
    return resumed;
}

static GPtrArray *
read_selected_paths(const gchar *output_path)
{
    g_autofree gchar *contents = NULL;
    g_auto(GStrv) lines = NULL;
    gsize length;
    GPtrArray *paths =
        g_ptr_array_new_with_free_func(g_free);
    guint i;

    if (!g_file_get_contents(
            output_path, &contents, &length, NULL) ||
        !length)
        return paths;
    lines = g_strsplit(contents, "\n", -1);
    for (i = 0; lines[i]; i++) {
        gsize line_length = strlen(lines[i]);

        if (line_length && lines[i][line_length - 1] == '\r')
            lines[i][--line_length] = '\0';
        if (!line_length || !g_path_is_absolute(lines[i]))
            continue;
        g_ptr_array_add(paths, g_strdup(lines[i]));
    }
    return paths;
}

static void
complete_active(MuxKittyChooser *chooser,
                gboolean child_succeeded)
{
    ChooserJob *job = chooser->active;
    g_autoptr(GPtrArray) paths = NULL;
    g_autoptr(GError) error = NULL;
    MuxUiAction action;

    if (!job)
        return;
    resume_pane(chooser, job);
    if (!job->suppress_response && !chooser->shutting_down) {
        paths = child_succeeded && !job->timed_out
                    ? read_selected_paths(job->output_path)
                    : g_ptr_array_new_with_free_func(g_free);
        action = paths->len ? MUX_UI_ACTION_SUBMIT
                            : MUX_UI_ACTION_CANCEL;
        if (!send_response(chooser,
                           job->request->request_id,
                           action,
                           paths,
                           &error))
            warn_async_error(
                "could not send file chooser response", error);
    }
    chooser->active = NULL;
    chooser_job_free(job);
    if (!chooser->shutting_down)
        start_next(chooser);
}

static void
on_wait_complete(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    MuxKittyChooser *chooser = user_data;
    ChooserJob *job = chooser->active;
    g_autoptr(GError) error = NULL;
    gboolean waited;
    gboolean succeeded = FALSE;

    if (job && G_SUBPROCESS(source) == job->process) {
        waited = g_subprocess_wait_finish(
            G_SUBPROCESS(source), result, &error);
        if (waited)
            succeeded =
                g_subprocess_get_successful(G_SUBPROCESS(source));
        else if (!g_error_matches(
                     error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            warn_async_error("file chooser process failed", error);
        complete_active(chooser, succeeded);
    }
    mux_kitty_chooser_unref(chooser);
}

static void
fail_start(MuxKittyChooser *chooser,
           ChooserJob *job,
           GError *cause)
{
    g_autoptr(GError) send_error = NULL;

    warn_async_error("could not start kitten choose-files", cause);
    resume_pane(chooser, job);
    if (!job->suppress_response && !chooser->shutting_down &&
        !send_response(chooser,
                       job->request->request_id,
                       MUX_UI_ACTION_CANCEL,
                       NULL,
                       &send_error))
        warn_async_error(
            "could not send file chooser cancellation", send_error);
    chooser->active = NULL;
    chooser_job_free(job);
    if (!chooser->shutting_down)
        start_next(chooser);
}

static void
start_next(MuxKittyChooser *chooser)
{
    ChooserJob *job;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) arguments = NULL;
    gint output_descriptor;

    if (chooser->active || chooser->shutting_down)
        return;
    job = g_queue_pop_head(&chooser->queued);
    if (!job)
        return;
    chooser->active = job;

    if (job->expires_at_us <= g_get_monotonic_time()) {
        fail_start(chooser, job, NULL);
        return;
    }
    if (chooser->suspend_func &&
        !chooser->suspend_func(chooser->user_data, &error)) {
        fail_start(chooser, job, error);
        return;
    }
    job->suspended = TRUE;

    output_descriptor = g_file_open_tmp(
        "mux-choose-files-XXXXXX", &job->output_path, &error);
    if (output_descriptor < 0) {
        fail_start(chooser, job, error);
        return;
    }
    if (close(output_descriptor) < 0) {
        gint saved_errno = errno;

        g_set_error(&error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot close chooser output: %s",
                    g_strerror(saved_errno));
        fail_start(chooser, job, error);
        return;
    }

    arguments = chooser_argv(job);
    job->process = g_subprocess_newv(
        (const gchar *const *)arguments->pdata,
        G_SUBPROCESS_FLAGS_STDIN_INHERIT,
        &error);
    if (!job->process) {
        fail_start(chooser, job, error);
        return;
    }
    job->wait_cancellable = g_cancellable_new();
    g_subprocess_wait_async(
        job->process,
        job->wait_cancellable,
        on_wait_complete,
        mux_kitty_chooser_ref(chooser));
}

MuxKittyChooser *
mux_kitty_chooser_new(MuxKittyChooserSendFunc send_func,
                      MuxKittyChooserStateFunc suspend_func,
                      MuxKittyChooserStateFunc resume_func,
                      gpointer user_data,
                      GDestroyNotify user_data_destroy)
{
    MuxKittyChooser *chooser;

    g_return_val_if_fail(send_func, NULL);
    g_return_val_if_fail(suspend_func, NULL);
    g_return_val_if_fail(resume_func, NULL);
    chooser = g_new0(MuxKittyChooser, 1);
    g_ref_count_init(&chooser->reference_count);
    g_queue_init(&chooser->queued);
    chooser->send_func = send_func;
    chooser->suspend_func = suspend_func;
    chooser->resume_func = resume_func;
    chooser->user_data = user_data;
    chooser->user_data_destroy = user_data_destroy;
    return chooser;
}

void
mux_kitty_chooser_free(MuxKittyChooser *chooser)
{
    if (!chooser)
        return;
    chooser->shutting_down = TRUE;
    g_queue_clear_full(&chooser->queued,
                       (GDestroyNotify)chooser_job_free);
    if (chooser->active && chooser->active->process) {
        chooser->active->suppress_response = TRUE;
        g_subprocess_force_exit(chooser->active->process);
        g_cancellable_cancel(chooser->active->wait_cancellable);
    } else if (chooser->active) {
        resume_pane(chooser, chooser->active);
        chooser_job_free(chooser->active);
        chooser->active = NULL;
    }
    mux_kitty_chooser_unref(chooser);
}

gboolean
mux_kitty_chooser_handle_request(MuxKittyChooser *chooser,
                                 const MuxUiRequest *request,
                                 GError **error)
{
    ChooserJob *job;
    guint32 deadline_ms;

    g_return_val_if_fail(chooser, FALSE);
    g_return_val_if_fail(request, FALSE);
    if (request->kind != MUX_UI_REQUEST_FILE_CHOOSER) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "Kitty chooser received a non-chooser request");
        return FALSE;
    }
    if (!request->request_id) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "file chooser request ID must be nonzero");
        return FALSE;
    }
    if (request_is_queued(chooser, request->request_id))
        return TRUE;
    if (pending_count(chooser) >= MUX_KITTY_CHOOSER_MAX_PENDING)
        return send_response(chooser,
                             request->request_id,
                             MUX_UI_ACTION_UNSUPPORTED,
                             NULL,
                             error);

    deadline_ms = request->deadline_ms
                      ? request->deadline_ms
                      : 300000;
    job = g_new0(ChooserJob, 1);
    job->request = mux_ui_request_copy(request);
    job->expires_at_us =
        g_get_monotonic_time() + ((gint64)deadline_ms * 1000);
    g_queue_push_tail(&chooser->queued, job);
    start_next(chooser);
    return TRUE;
}

gboolean
mux_kitty_chooser_cancel(MuxKittyChooser *chooser,
                         guint64 request_id)
{
    GList *link;

    g_return_val_if_fail(chooser, FALSE);
    if (chooser->active &&
        chooser->active->request->request_id == request_id) {
        chooser->active->suppress_response = TRUE;
        if (chooser->active->process) {
            g_subprocess_force_exit(chooser->active->process);
        } else {
            ChooserJob *job = chooser->active;

            chooser->active = NULL;
            resume_pane(chooser, job);
            chooser_job_free(job);
            start_next(chooser);
        }
        return TRUE;
    }
    for (link = chooser->queued.head; link; link = link->next) {
        ChooserJob *job = link->data;

        if (job->request->request_id == request_id) {
            g_queue_delete_link(&chooser->queued, link);
            chooser_job_free(job);
            return TRUE;
        }
    }
    return FALSE;
}

void
mux_kitty_chooser_cancel_all(MuxKittyChooser *chooser)
{
    g_return_if_fail(chooser);
    g_queue_clear_full(&chooser->queued,
                       (GDestroyNotify)chooser_job_free);
    if (chooser->active) {
        chooser->active->suppress_response = TRUE;
        if (chooser->active->process)
            g_subprocess_force_exit(chooser->active->process);
        else {
            ChooserJob *job = chooser->active;

            chooser->active = NULL;
            resume_pane(chooser, job);
            chooser_job_free(job);
        }
    }
}

guint
mux_kitty_chooser_tick(MuxKittyChooser *chooser,
                       gint64 monotonic_us)
{
    GList *link;
    GList *next;
    guint expired = 0;

    g_return_val_if_fail(chooser, 0);
    for (link = chooser->queued.head; link; link = next) {
        ChooserJob *job = link->data;
        g_autoptr(GError) error = NULL;

        next = link->next;
        if (job->expires_at_us > monotonic_us)
            continue;
        g_queue_delete_link(&chooser->queued, link);
        if (!send_response(chooser,
                           job->request->request_id,
                           MUX_UI_ACTION_CANCEL,
                           NULL,
                           &error))
            warn_async_error(
                "could not send expired chooser response", error);
        chooser_job_free(job);
        expired++;
    }
    if (chooser->active &&
        chooser->active->expires_at_us <= monotonic_us &&
        !chooser->active->timed_out) {
        chooser->active->timed_out = TRUE;
        if (chooser->active->process)
            g_subprocess_force_exit(chooser->active->process);
        expired++;
    }
    return expired;
}

gboolean
mux_kitty_chooser_is_busy(const MuxKittyChooser *chooser)
{
    g_return_val_if_fail(chooser, FALSE);
    return chooser->active != NULL;
}

guint
mux_kitty_chooser_pending_count(const MuxKittyChooser *chooser)
{
    g_return_val_if_fail(chooser, 0);
    return pending_count(chooser);
}
