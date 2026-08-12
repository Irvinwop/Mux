#define _GNU_SOURCE

#include "mux-file-chooser-engine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MUX_UPLOAD_MAX_FILES_PER_REQUEST 32u
#define MUX_UPLOAD_MAX_STAGED_FILES 512u
#define MUX_UPLOAD_MAX_STAGED_SELECTIONS 128u
#define MUX_UPLOAD_MAX_FILE_BYTES ((guint64)256 * 1024 * 1024)
#define MUX_UPLOAD_MAX_STAGED_BYTES ((guint64)2 * 1024 * 1024 * 1024)
#define MUX_UPLOAD_COPY_BUFFER_BYTES (128u * 1024u)
#define MUX_UPLOAD_COPY_DEADLINE_MS 15000u
#define MUX_UPLOAD_WORKER_THREADS 2
#define MUX_UPLOAD_MAX_SCHEDULED_JOBS 8u
#define MUX_UPLOAD_TEMP_NAME ".mux-upload.tmp"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

typedef struct _StageOperation StageOperation;
typedef void (*StageReadyFunc)(StageOperation *operation,
                               gpointer user_data);

typedef struct {
    guint64 request_id;
    WebKitFileChooserRequest *request;
    gboolean select_multiple;
    StageOperation *operation;
} PendingChooser;

typedef struct {
    gchar *directory;
    GPtrArray *file_paths;
    GPtrArray *item_directories;
    guint64 bytes;
} StagedSelection;

typedef struct {
    dev_t device;
    ino_t inode;
} SourceIdentity;

struct _MuxFileChooserBridge {
    gatomicrefcount ref_count;
    GMainContext *context;
    WebKitWebView *web_view;
    GHashTable *pending;
    GPtrArray *staged_selections;
    GMutex quota_lock;
    guint staged_selection_count;
    guint staged_file_count;
    guint64 staged_bytes;
    guint reserved_selection_count;
    guint reserved_file_count;
    guint64 reserved_bytes;
    guint staging_jobs;
    MuxFileChooserSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong run_file_chooser_handler;
    gboolean disposing;
    gboolean disposed;
};

struct _StageOperation {
    gatomicrefcount ref_count;
    MuxFileChooserBridge *bridge;
    guint64 request_id;
    gboolean select_multiple;
    GPtrArray *paths;
    GCancellable *cancellable;
    gint64 deadline_us;
    GSource *deadline_source;
    gint outcome_claimed;
    gboolean job_counted;
    guint reserved_selections;
    guint reserved_files;
    guint64 reserved_bytes;
    StagedSelection *worker_result;
    GError *worker_error;
    GError *delivery_error;
    StageReadyFunc ready_func;
    gpointer ready_data;
    GDestroyNotify ready_data_destroy;
    gint worker_done;
    gint worker_dispatched;
#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
    gint stall_fd;
    gint stall_entered;
#endif
};

static GThreadPool *stage_pool;
static GThreadPool *cleanup_pool;
static gsize worker_pools_initialized;
static GMutex scheduler_lock;
static guint scheduled_jobs;
static gchar *stage_pool_error;
static gchar *cleanup_pool_error;

static MuxFileChooserBridge *bridge_ref(MuxFileChooserBridge *bridge);
static void bridge_unref(MuxFileChooserBridge *bridge);
static StageOperation *stage_operation_ref(StageOperation *operation);
static void stage_operation_unref(StageOperation *operation);
static void stage_worker(gpointer data, gpointer user_data);
static void cleanup_worker(gpointer data, gpointer user_data);
static gboolean stage_worker_dispatch(gpointer data);

static void
staged_selection_free(StagedSelection *staged)
{
    guint i;

    if (!staged)
        return;
    if (staged->file_paths) {
        for (i = 0; i < staged->file_paths->len; i++) {
            const gchar *path =
                g_ptr_array_index(staged->file_paths, i);

            if (path)
                unlink(path);
        }
    }
    if (staged->item_directories) {
        for (i = staged->item_directories->len; i > 0; i--) {
            const gchar *path =
                g_ptr_array_index(staged->item_directories, i - 1);

            if (path)
                rmdir(path);
        }
    }
    if (staged->directory)
        rmdir(staged->directory);
    g_clear_pointer(&staged->file_paths, g_ptr_array_unref);
    g_clear_pointer(&staged->item_directories, g_ptr_array_unref);
    g_free(staged->directory);
    g_free(staged);
}

static void
cleanup_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    staged_selection_free(data);
}

static void
initialize_worker_pools(void)
{
    GError *error = NULL;

    cleanup_pool = g_thread_pool_new(cleanup_worker,
                                     NULL,
                                     1,
                                     FALSE,
                                     &error);
    if (!cleanup_pool) {
        cleanup_pool_error = g_strdup(error->message);
        g_clear_error(&error);
    }
    stage_pool = g_thread_pool_new(stage_worker,
                                   NULL,
                                   MUX_UPLOAD_WORKER_THREADS,
                                   FALSE,
                                   &error);
    if (!stage_pool) {
        stage_pool_error = g_strdup(error->message);
        g_clear_error(&error);
    }
}

static void
ensure_worker_pools(void)
{
    if (g_once_init_enter(&worker_pools_initialized)) {
        initialize_worker_pools();
        g_once_init_leave(&worker_pools_initialized, 1);
    }
}

static void
queue_staged_selection_cleanup(StagedSelection *staged)
{
    GError *error = NULL;

    if (!staged)
        return;
    ensure_worker_pools();
    if (cleanup_pool &&
        g_thread_pool_push(cleanup_pool, staged, &error))
        return;
    g_warning("Cannot schedule upload cleanup: %s",
              error ? error->message
                    : (cleanup_pool_error ? cleanup_pool_error
                                          : "worker pool unavailable"));
    g_clear_error(&error);
    /* Never move potentially blocking cleanup back onto the engine thread. */
}

static void
clear_staged_selections(MuxFileChooserBridge *bridge)
{
    guint i;

    if (!bridge->staged_selections)
        return;
    for (i = 0; i < bridge->staged_selections->len; i++)
        queue_staged_selection_cleanup(
            g_ptr_array_index(bridge->staged_selections, i));
    g_ptr_array_set_size(bridge->staged_selections, 0);
    g_mutex_lock(&bridge->quota_lock);
    bridge->staged_selection_count = 0;
    bridge->staged_file_count = 0;
    bridge->staged_bytes = 0;
    g_mutex_unlock(&bridge->quota_lock);
}

static void
pending_chooser_free(PendingChooser *pending)
{
    if (!pending)
        return;
    g_clear_pointer(&pending->operation, stage_operation_unref);
    g_clear_object(&pending->request);
    g_free(pending);
}

static MuxFileChooserBridge *
bridge_alloc(void)
{
    MuxFileChooserBridge *bridge = g_new0(MuxFileChooserBridge, 1);

    g_atomic_ref_count_init(&bridge->ref_count);
    bridge->context = g_main_context_ref_thread_default();
    bridge->pending = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)pending_chooser_free);
    bridge->staged_selections = g_ptr_array_new();
    g_mutex_init(&bridge->quota_lock);
    return bridge;
}

static MuxFileChooserBridge *
bridge_ref(MuxFileChooserBridge *bridge)
{
    g_atomic_ref_count_inc(&bridge->ref_count);
    return bridge;
}

static void
bridge_destroy(MuxFileChooserBridge *bridge)
{
    clear_staged_selections(bridge);
    g_clear_pointer(&bridge->pending, g_hash_table_unref);
    g_clear_pointer(&bridge->staged_selections, g_ptr_array_unref);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_clear_object(&bridge->web_view);
    g_clear_pointer(&bridge->context, g_main_context_unref);
    g_mutex_clear(&bridge->quota_lock);
    g_free(bridge);
}

static void
bridge_unref(MuxFileChooserBridge *bridge)
{
    if (g_atomic_ref_count_dec(&bridge->ref_count))
        bridge_destroy(bridge);
}

static guint64 *
request_key_new(guint64 request_id)
{
    guint64 *key = g_new(guint64, 1);

    *key = request_id;
    return key;
}

static guint64
next_request_id(MuxFileChooserBridge *bridge)
{
    guint64 request_id;

    do {
        request_id = ((guint64)g_random_int() << 32) | g_random_int();
    } while (!request_id ||
             g_hash_table_contains(bridge->pending, &request_id));
    return request_id;
}

static gchar *
bounded_utf8(const gchar *value, gsize maximum)
{
    g_autofree gchar *valid = g_utf8_make_valid(value ? value : "", -1);
    gsize length = strlen(valid);

    if (length <= maximum)
        return g_steal_pointer(&valid);
    length = maximum;
    while (length && !g_utf8_validate(valid, length, NULL))
        length--;
    return g_strndup(valid, length);
}

static gchar *
origin_for_view(WebKitWebView *web_view)
{
    const gchar *uri_string = webkit_web_view_get_uri(web_view);
    g_autoptr(GError) error = NULL;
    g_autoptr(GUri) uri = NULL;
    const gchar *scheme;
    const gchar *host;
    gint port;
    g_autofree gchar *authority = NULL;

    if (!uri_string || !*uri_string)
        return g_strdup("browser");
    uri = g_uri_parse(uri_string,
                      G_URI_FLAGS_PARSE_RELAXED | G_URI_FLAGS_ENCODED,
                      &error);
    if (!uri)
        return bounded_utf8(uri_string, 2048);
    scheme = g_uri_get_scheme(uri);
    host = g_uri_get_host(uri);
    port = g_uri_get_port(uri);
    if (!scheme || !*scheme)
        return g_strdup("browser");
    if (!host || !*host)
        return g_strdup_printf("%s:", scheme);
    authority = strchr(host, ':') ? g_strdup_printf("[%s]", host)
                                  : g_strdup(host);
    if (port >= 0 &&
        !((g_str_equal(scheme, "http") && port == 80) ||
          (g_str_equal(scheme, "https") && port == 443)))
        return g_strdup_printf("%s://%s:%d", scheme, authority, port);
    return g_strdup_printf("%s://%s", scheme, authority);
}

static PendingChooser *
take_pending(MuxFileChooserBridge *bridge, guint64 request_id)
{
    gpointer stored_key = NULL;
    gpointer stored_value = NULL;

    if (!g_hash_table_lookup_extended(bridge->pending,
                                      &request_id,
                                      &stored_key,
                                      &stored_value))
        return NULL;
    g_hash_table_steal(bridge->pending, &request_id);
    g_free(stored_key);
    return stored_value;
}

static void
send_cancel(MuxFileChooserBridge *bridge,
            guint64 request_id,
            MuxUiCancelReason reason)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) payload = NULL;

    if (!bridge->send_func || bridge->disposed)
        return;
    payload = mux_ui_cancel_encode(request_id, reason, &error);
    if (payload)
        bridge->send_func(payload, bridge->user_data, &error);
}

static gboolean
on_run_file_chooser(WebKitWebView *web_view,
                    WebKitFileChooserRequest *chooser,
                    MuxFileChooserBridge *bridge)
{
    const gchar *const *mime_types =
        webkit_file_chooser_request_get_mime_types(chooser);
    const gchar *const *selected_files =
        webkit_file_chooser_request_get_selected_files(chooser);
    gboolean multiple =
        webkit_file_chooser_request_get_select_multiple(chooser);
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_FILE_CHOOSER);
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GError) error = NULL;
    PendingChooser *pending;
    MuxFileChooserBridge *keep_alive;
    guint i;

    if (bridge->disposing || bridge->disposed) {
        webkit_file_chooser_request_cancel(chooser);
        return TRUE;
    }
    request->request_id = next_request_id(bridge);
    request->deadline_ms = 300000;
    request->origin = origin_for_view(web_view);
    request->heading = g_strdup("Choose files");
    request->message = g_strdup(
        multiple ? "Select one or more local files to share with this site."
                 : "Select one local file to share with this site.");
    if (multiple)
        request->flags |= MUX_UI_REQUEST_FLAG_MULTIPLE;
    if (selected_files && selected_files[0])
        request->default_value =
            bounded_utf8(selected_files[0], MUX_UI_MAX_PATH);
    if (mime_types) {
        for (i = 0; mime_types[i] && i < 64; i++) {
            g_autofree gchar *mime = bounded_utf8(mime_types[i], 1024);

            g_ptr_array_add(request->choices,
                            mux_ui_choice_new(i, 0, mime));
        }
    }

    payload = mux_ui_request_encode(request, &error);
    if (!payload) {
        webkit_file_chooser_request_cancel(chooser);
        return TRUE;
    }
    pending = g_new0(PendingChooser, 1);
    pending->request_id = request->request_id;
    pending->request = g_object_ref(chooser);
    pending->select_multiple = multiple;
    g_hash_table_insert(bridge->pending,
                        request_key_new(pending->request_id),
                        pending);
    keep_alive = bridge_ref(bridge);
    if (!bridge->send_func(payload, bridge->user_data, &error)) {
        PendingChooser *failed =
            take_pending(bridge, request->request_id);

        if (failed) {
            webkit_file_chooser_request_cancel(failed->request);
            pending_chooser_free(failed);
        }
    }
    bridge_unref(keep_alive);
    return TRUE;
}

MuxFileChooserBridge *
mux_file_chooser_bridge_new(WebKitWebView *web_view,
                            MuxFileChooserSendFunc send_func,
                            gpointer user_data,
                            GDestroyNotify user_data_destroy)
{
    MuxFileChooserBridge *bridge;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);
    g_return_val_if_fail(send_func, NULL);
    bridge = bridge_alloc();
    bridge->web_view = g_object_ref(web_view);
    bridge->send_func = send_func;
    bridge->user_data = user_data;
    bridge->user_data_destroy = user_data_destroy;
    bridge->run_file_chooser_handler =
        g_signal_connect(web_view,
                         "run-file-chooser",
                         G_CALLBACK(on_run_file_chooser),
                         bridge);
    return bridge;
}

static gboolean
set_errno_error(GError **error, const gchar *operation)
{
    int saved_errno = errno;

    g_set_error(error,
                G_FILE_ERROR,
                g_file_error_from_errno(saved_errno),
                "%s: %s",
                operation,
                g_strerror(saved_errno));
    return FALSE;
}

static gboolean
stage_checkpoint(StageOperation *operation, GError **error)
{
    if (g_cancellable_is_cancelled(operation->cancellable)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "file upload staging was cancelled");
        return FALSE;
    }
    if (g_get_monotonic_time() >= operation->deadline_us) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_TIMED_OUT,
                            "staging selected files exceeded its time budget");
        return FALSE;
    }
    return TRUE;
}

static gboolean
source_identity_seen(const GArray *identities,
                     const struct stat *status)
{
    guint i;

    for (i = 0; i < identities->len; i++) {
        const SourceIdentity *identity =
            &g_array_index(identities, SourceIdentity, i);

        if (identity->device == status->st_dev &&
            identity->inode == status->st_ino)
            return TRUE;
    }
    return FALSE;
}

static gboolean
source_snapshot_unchanged(const struct stat *before,
                          const struct stat *after)
{
    return S_ISREG(after->st_mode) &&
           before->st_dev == after->st_dev &&
           before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static gboolean
reserve_selection(StageOperation *operation, GError **error)
{
    MuxFileChooserBridge *bridge = operation->bridge;
    gboolean success = FALSE;

    g_mutex_lock(&bridge->quota_lock);
    if (bridge->disposing || bridge->disposed ||
        g_cancellable_is_cancelled(operation->cancellable)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "file upload staging was abandoned");
    } else if (bridge->staged_selection_count +
                   bridge->reserved_selection_count >=
               MUX_UPLOAD_MAX_STAGED_SELECTIONS) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "the chooser manager already retains 128 upload snapshots");
    } else {
        bridge->reserved_selection_count++;
        operation->reserved_selections++;
        success = TRUE;
    }
    g_mutex_unlock(&bridge->quota_lock);
    return success;
}

static gboolean
reserve_source(StageOperation *operation,
               guint64 source_bytes,
               GError **error)
{
    MuxFileChooserBridge *bridge = operation->bridge;
    guint64 used_bytes;
    gboolean success = FALSE;

    g_mutex_lock(&bridge->quota_lock);
    used_bytes = bridge->staged_bytes + bridge->reserved_bytes;
    if (bridge->disposing || bridge->disposed ||
        g_cancellable_is_cancelled(operation->cancellable)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "file upload staging was abandoned");
    } else if (bridge->staged_file_count +
                   bridge->reserved_file_count >=
               MUX_UPLOAD_MAX_STAGED_FILES) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "too many upload files are retained by this chooser manager");
    } else if (used_bytes > MUX_UPLOAD_MAX_STAGED_BYTES ||
               source_bytes > MUX_UPLOAD_MAX_STAGED_BYTES - used_bytes) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "selected files exceed the 2 GiB chooser manager staging limit");
    } else {
        bridge->reserved_file_count++;
        bridge->reserved_bytes += source_bytes;
        operation->reserved_files++;
        operation->reserved_bytes += source_bytes;
        success = TRUE;
    }
    g_mutex_unlock(&bridge->quota_lock);
    return success;
}

static void
release_reservations(StageOperation *operation)
{
    MuxFileChooserBridge *bridge = operation->bridge;

    if (!operation->reserved_selections &&
        !operation->reserved_files &&
        !operation->reserved_bytes)
        return;
    g_mutex_lock(&bridge->quota_lock);
    g_assert_cmpuint(bridge->reserved_selection_count,
                     >=,
                     operation->reserved_selections);
    g_assert_cmpuint(bridge->reserved_file_count,
                     >=,
                     operation->reserved_files);
    g_assert_cmpuint(bridge->reserved_bytes,
                     >=,
                     operation->reserved_bytes);
    bridge->reserved_selection_count -= operation->reserved_selections;
    bridge->reserved_file_count -= operation->reserved_files;
    bridge->reserved_bytes -= operation->reserved_bytes;
    g_mutex_unlock(&bridge->quota_lock);
    operation->reserved_selections = 0;
    operation->reserved_files = 0;
    operation->reserved_bytes = 0;
}

static void
commit_reservations(StageOperation *operation)
{
    MuxFileChooserBridge *bridge = operation->bridge;

    g_mutex_lock(&bridge->quota_lock);
    g_assert_cmpuint(bridge->reserved_selection_count,
                     >=,
                     operation->reserved_selections);
    g_assert_cmpuint(bridge->reserved_file_count,
                     >=,
                     operation->reserved_files);
    g_assert_cmpuint(bridge->reserved_bytes,
                     >=,
                     operation->reserved_bytes);
    bridge->reserved_selection_count -= operation->reserved_selections;
    bridge->reserved_file_count -= operation->reserved_files;
    bridge->reserved_bytes -= operation->reserved_bytes;
    bridge->staged_selection_count += operation->reserved_selections;
    bridge->staged_file_count += operation->reserved_files;
    bridge->staged_bytes += operation->reserved_bytes;
    g_mutex_unlock(&bridge->quota_lock);
    operation->reserved_selections = 0;
    operation->reserved_files = 0;
    operation->reserved_bytes = 0;
}

static gboolean
write_all(StageOperation *operation,
          int fd,
          const guint8 *data,
          gsize length,
          GError **error)
{
    gsize offset = 0;

    while (offset < length) {
        ssize_t written;

        if (!stage_checkpoint(operation, error))
            return FALSE;
        written = write(fd, data + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return set_errno_error(error, "cannot write staged upload");
        }
        if (!written) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "staged upload write made no progress");
            return FALSE;
        }
        offset += (gsize)written;
    }
    return TRUE;
}

static gboolean
copy_stable_snapshot(StageOperation *operation,
                     int source_fd,
                     int destination_fd,
                     const struct stat *before,
                     GError **error)
{
    g_autofree guint8 *buffer = g_malloc(MUX_UPLOAD_COPY_BUFFER_BYTES);
    guint64 remaining = (guint64)before->st_size;
    struct stat after;
    struct stat destination;
    guint8 extra_byte;
    ssize_t extra;

    while (remaining) {
        gsize requested = (gsize)MIN(
            remaining, (guint64)MUX_UPLOAD_COPY_BUFFER_BYTES);
        ssize_t received;

        if (!stage_checkpoint(operation, error))
            return FALSE;
        received = read(source_fd, buffer, requested);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return set_errno_error(error, "cannot read selected file");
        }
        if (!received) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_BUSY,
                                "selected file changed while it was being staged");
            return FALSE;
        }
        if (!write_all(operation,
                       destination_fd,
                       buffer,
                       (gsize)received,
                       error))
            return FALSE;
        remaining -= (guint64)received;
    }

    if (!stage_checkpoint(operation, error))
        return FALSE;
    do {
        extra = read(source_fd, &extra_byte, 1);
    } while (extra < 0 && errno == EINTR);
    if (extra < 0)
        return set_errno_error(error, "cannot finish reading selected file");
    if (extra > 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "selected file grew while it was being staged");
        return FALSE;
    }
    if (fstat(source_fd, &after) < 0)
        return set_errno_error(error, "cannot revalidate selected file");
    if (!source_snapshot_unchanged(before, &after)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "selected file changed while it was being staged");
        return FALSE;
    }
    if (fstat(destination_fd, &destination) < 0)
        return set_errno_error(error, "cannot validate staged upload");
    if (!S_ISREG(destination.st_mode) ||
        destination.st_size != before->st_size) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "staged upload has an unexpected size or type");
        return FALSE;
    }
    if (fchmod(destination_fd, S_IRUSR) < 0)
        return set_errno_error(error, "cannot make staged upload read-only");
    return TRUE;
}

static gboolean
rename_noreplace_at(int directory_fd,
                    const gchar *temporary_name,
                    const gchar *final_name,
                    GError **error)
{
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2,
                directory_fd,
                temporary_name,
                directory_fd,
                final_name,
                RENAME_NOREPLACE) == 0)
        return TRUE;
    if (errno != ENOSYS && errno != EINVAL)
        return set_errno_error(error, "cannot publish staged upload");
#endif
    if (linkat(directory_fd,
               temporary_name,
               directory_fd,
               final_name,
               0) < 0)
        return set_errno_error(error, "cannot publish staged upload");
    if (unlinkat(directory_fd, temporary_name, 0) < 0) {
        int saved_errno = errno;

        unlinkat(directory_fd, final_name, 0);
        errno = saved_errno;
        return set_errno_error(error, "cannot retire upload staging inode");
    }
    return TRUE;
}

static gboolean
stage_source_path(StageOperation *operation,
                  StagedSelection *staged,
                  GArray *identities,
                  int staging_fd,
                  const gchar *path,
                  GError **error)
{
    g_autofree gchar *basename = NULL;
    g_autofree gchar *item_directory = NULL;
    g_autofree gchar *staged_path = NULL;
    gchar item_name[32];
    struct stat status;
    struct stat published;
    struct stat destination;
    SourceIdentity identity;
    guint64 source_bytes;
    int source_fd = -1;
    int item_fd = -1;
    int destination_fd = -1;
    gboolean temporary_exists = FALSE;
    gboolean final_exists = FALSE;
    gboolean success = FALSE;

    if (!stage_checkpoint(operation, error))
        return FALSE;
    if (!path || !g_utf8_validate(path, -1, NULL) ||
        !g_path_is_absolute(path) || strlen(path) > MUX_UI_MAX_PATH) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "selected file path must be a bounded absolute UTF-8 path");
        return FALSE;
    }
    source_fd = open(path,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source_fd < 0) {
        if (errno == ELOOP) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "symbolic links are not accepted for file uploads");
            return FALSE;
        }
        return set_errno_error(error, "cannot open selected file");
    }
    if (fstat(source_fd, &status) < 0) {
        set_errno_error(error, "cannot inspect selected file");
        goto out;
    }
    if (S_ISDIR(status.st_mode)) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_SUPPORTED,
            "directory uploads are rejected because an immutable bounded directory snapshot cannot be guaranteed");
        goto out;
    }
    if (!S_ISREG(status.st_mode)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "selected path is not a regular file");
        goto out;
    }
    if (status.st_size < 0 ||
        (guint64)status.st_size > MUX_UPLOAD_MAX_FILE_BYTES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "selected file exceeds the 256 MiB per-file staging limit");
        goto out;
    }
    if (source_identity_seen(identities, &status)) {
        success = TRUE;
        goto out;
    }
    source_bytes = (guint64)status.st_size;
    if (!reserve_source(operation, source_bytes, error))
        goto out;

    basename = g_path_get_basename(path);
    if (!basename || !*basename ||
        g_str_equal(basename, ".") || g_str_equal(basename, "..") ||
        !g_utf8_validate(basename, -1, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "selected file has an unsafe basename");
        goto out;
    }
    g_snprintf(item_name,
               sizeof(item_name),
               "item-%u",
               staged->file_paths->len);
    if (mkdirat(staging_fd, item_name, S_IRWXU) < 0) {
        set_errno_error(error, "cannot create private upload item directory");
        goto out;
    }
    item_directory = g_build_filename(staged->directory, item_name, NULL);
    g_ptr_array_add(staged->item_directories,
                    g_steal_pointer(&item_directory));
    item_fd = openat(staging_fd,
                     item_name,
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (item_fd < 0) {
        set_errno_error(error, "cannot open private upload item directory");
        goto out;
    }
    destination_fd = openat(item_fd,
                            MUX_UPLOAD_TEMP_NAME,
                            O_WRONLY | O_CREAT | O_EXCL |
                                O_CLOEXEC | O_NOFOLLOW,
                            S_IRUSR | S_IWUSR);
    if (destination_fd < 0) {
        set_errno_error(error, "cannot create staged upload");
        goto out;
    }
    temporary_exists = TRUE;
    if (!copy_stable_snapshot(operation,
                              source_fd,
                              destination_fd,
                              &status,
                              error))
        goto out;
    if (!stage_checkpoint(operation, error))
        goto out;
    if (fsync(destination_fd) < 0) {
        set_errno_error(error, "cannot sync staged upload");
        goto out;
    }
    if (fstat(destination_fd, &destination) < 0) {
        set_errno_error(error, "cannot revalidate staged upload");
        goto out;
    }
    if (close(destination_fd) < 0) {
        destination_fd = -1;
        set_errno_error(error, "cannot close staged upload");
        goto out;
    }
    destination_fd = -1;
    if (!stage_checkpoint(operation, error))
        goto out;
    if (!rename_noreplace_at(item_fd,
                             MUX_UPLOAD_TEMP_NAME,
                             basename,
                             error))
        goto out;
    temporary_exists = FALSE;
    final_exists = TRUE;
    if (fstatat(item_fd, basename, &published, AT_SYMLINK_NOFOLLOW) < 0) {
        set_errno_error(error, "cannot inspect published staged upload");
        goto out;
    }
    if (!S_ISREG(published.st_mode) ||
        published.st_dev != destination.st_dev ||
        published.st_ino != destination.st_ino ||
        published.st_size != status.st_size ||
        (published.st_mode & (S_IWUSR | S_IRWXG | S_IRWXO))) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "published staged upload failed validation");
        goto out;
    }
    staged_path = g_build_filename(
        staged->directory, item_name, basename, NULL);
    g_ptr_array_add(staged->file_paths,
                    g_steal_pointer(&staged_path));
    if (fsync(item_fd) < 0) {
        set_errno_error(error, "cannot sync staged upload directory");
        goto out;
    }
    identity.device = status.st_dev;
    identity.inode = status.st_ino;
    g_array_append_val(identities, identity);
    staged->bytes += source_bytes;
    success = TRUE;

out:
    if (destination_fd >= 0)
        close(destination_fd);
    if (!success && final_exists)
        unlinkat(item_fd, basename, 0);
    if (temporary_exists)
        unlinkat(item_fd, MUX_UPLOAD_TEMP_NAME, 0);
    if (item_fd >= 0)
        close(item_fd);
    if (source_fd >= 0)
        close(source_fd);
    return success;
}

static StagedSelection *
stage_paths(StageOperation *operation, GError **error)
{
    StagedSelection *staged = NULL;
    GArray *identities = NULL;
    struct stat directory_status;
    int staging_fd = -1;
    guint i;

    if (!operation->paths || !operation->paths->len ||
        (!operation->select_multiple && operation->paths->len != 1) ||
        operation->paths->len > MUX_UI_MAX_PATHS ||
        operation->paths->len > MUX_UPLOAD_MAX_FILES_PER_REQUEST) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid number of selected files");
        return NULL;
    }
    if (!stage_checkpoint(operation, error) ||
        !reserve_selection(operation, error))
        return NULL;

    staged = g_new0(StagedSelection, 1);
    staged->file_paths = g_ptr_array_new_with_free_func(g_free);
    staged->item_directories = g_ptr_array_new_with_free_func(g_free);
    staged->directory = g_dir_make_tmp("mux-upload-XXXXXX", error);
    if (!staged->directory)
        goto fail;
    if (!g_utf8_validate(staged->directory, -1, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_FILENAME,
                            "private upload directory is not valid UTF-8");
        goto fail;
    }
    staging_fd = open(staged->directory,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging_fd < 0) {
        set_errno_error(error, "cannot open private upload directory");
        goto fail;
    }
    if (fstat(staging_fd, &directory_status) < 0) {
        set_errno_error(error, "cannot inspect private upload directory");
        goto fail;
    }
    if (!S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != geteuid() ||
        (directory_status.st_mode & (S_IRWXG | S_IRWXO))) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "private upload directory is not owner-only");
        goto fail;
    }

    identities = g_array_new(FALSE, FALSE, sizeof(SourceIdentity));
    for (i = 0; i < operation->paths->len; i++) {
        const gchar *path =
            g_ptr_array_index(operation->paths, i);

        if (!stage_source_path(operation,
                               staged,
                               identities,
                               staging_fd,
                               path,
                               error))
            goto fail;
    }
    if (!staged->file_paths->len) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "no distinct files were selected");
        goto fail;
    }
    if (!stage_checkpoint(operation, error))
        goto fail;
    if (fsync(staging_fd) < 0) {
        set_errno_error(error, "cannot sync private upload directory");
        goto fail;
    }
    close(staging_fd);
    g_array_unref(identities);
    return staged;

fail:
    if (staging_fd >= 0)
        close(staging_fd);
    if (identities)
        g_array_unref(identities);
    staged_selection_free(staged);
    return NULL;
}

static StageOperation *
stage_operation_ref(StageOperation *operation)
{
    g_atomic_ref_count_inc(&operation->ref_count);
    return operation;
}

static void
stage_operation_destroy(StageOperation *operation)
{
    if (operation->deadline_source) {
        g_source_destroy(operation->deadline_source);
        g_source_unref(operation->deadline_source);
    }
    if (operation->worker_result)
        queue_staged_selection_cleanup(operation->worker_result);
    g_clear_error(&operation->worker_error);
    g_clear_error(&operation->delivery_error);
    g_clear_pointer(&operation->paths, g_ptr_array_unref);
    g_clear_object(&operation->cancellable);
#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
    if (operation->stall_fd >= 0)
        close(operation->stall_fd);
#endif
    if (operation->ready_data_destroy)
        operation->ready_data_destroy(operation->ready_data);
    bridge_unref(operation->bridge);
    g_free(operation);
}

static void
stage_operation_unref(StageOperation *operation)
{
    if (g_atomic_ref_count_dec(&operation->ref_count))
        stage_operation_destroy(operation);
}

static StageOperation *
stage_operation_new(MuxFileChooserBridge *bridge,
                    guint64 request_id,
                    gboolean select_multiple,
                    const GPtrArray *paths,
                    guint deadline_ms,
                    StageReadyFunc ready_func,
                    gpointer ready_data,
                    GDestroyNotify ready_data_destroy,
                    GError **error)
{
    StageOperation *operation;
    guint i;

    if (!paths || !paths->len ||
        (!select_multiple && paths->len != 1) ||
        paths->len > MUX_UI_MAX_PATHS ||
        paths->len > MUX_UPLOAD_MAX_FILES_PER_REQUEST) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid number of selected files");
        return NULL;
    }
    operation = g_new0(StageOperation, 1);
    g_atomic_ref_count_init(&operation->ref_count);
    operation->bridge = bridge_ref(bridge);
    operation->request_id = request_id;
    operation->select_multiple = select_multiple;
    operation->paths = g_ptr_array_new_with_free_func(g_free);
    operation->cancellable = g_cancellable_new();
    operation->deadline_us = g_get_monotonic_time() +
        MAX(deadline_ms, 1u) * (gint64)G_TIME_SPAN_MILLISECOND;
    operation->ready_func = ready_func;
    operation->ready_data = ready_data;
    operation->ready_data_destroy = ready_data_destroy;
#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
    operation->stall_fd = -1;
#endif
    for (i = 0; i < paths->len; i++) {
        const gchar *path = g_ptr_array_index((GPtrArray *)paths, i);

        if (!path || !g_utf8_validate(path, -1, NULL) ||
            strlen(path) > MUX_UI_MAX_PATH) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "selected file path is not bounded UTF-8");
            stage_operation_unref(operation);
            return NULL;
        }
        g_ptr_array_add(operation->paths, g_strdup(path));
    }
    return operation;
}

static void
stage_operation_disarm_deadline(StageOperation *operation)
{
    GSource *source = operation->deadline_source;

    if (!source)
        return;
    operation->deadline_source = NULL;
    g_source_destroy(source);
    g_source_unref(source);
}

static gboolean
stage_deadline_expired(gpointer data)
{
    StageOperation *operation = data;
    GSource *source = operation->deadline_source;

    operation->deadline_source = NULL;
    if (source)
        g_source_unref(source);
    g_cancellable_cancel(operation->cancellable);
    if (g_atomic_int_compare_and_exchange(&operation->outcome_claimed,
                                          0,
                                          1)) {
        operation->delivery_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_TIMED_OUT,
            "staging selected files exceeded its time budget");
        operation->ready_func(operation, operation->ready_data);
    }
    return G_SOURCE_REMOVE;
}

static gboolean
stage_scheduler_submit(StageOperation *operation, GError **error)
{
    gboolean pushed;

    ensure_worker_pools();
    if (!stage_pool) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "file staging worker pool is unavailable: %s",
                    stage_pool_error ? stage_pool_error : "unknown error");
        return FALSE;
    }
    g_mutex_lock(&scheduler_lock);
    if (scheduled_jobs >= MUX_UPLOAD_MAX_SCHEDULED_JOBS) {
        g_mutex_unlock(&scheduler_lock);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "file staging worker queue is full");
        return FALSE;
    }
    scheduled_jobs++;
    g_mutex_unlock(&scheduler_lock);

    pushed = g_thread_pool_push(stage_pool,
                                stage_operation_ref(operation),
                                error);
    if (!pushed) {
        stage_operation_unref(operation);
        g_mutex_lock(&scheduler_lock);
        scheduled_jobs--;
        g_mutex_unlock(&scheduler_lock);
        return FALSE;
    }
    operation->bridge->staging_jobs++;
    operation->job_counted = TRUE;
    return TRUE;
}

static gboolean
stage_operation_start(StageOperation *operation, GError **error)
{
    GSource *source;
    guint deadline_ms;

    if (operation->bridge->disposing || operation->bridge->disposed) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            "file chooser was destroyed before staging began");
        return FALSE;
    }
    deadline_ms = (guint)MAX(
        1,
        (operation->deadline_us - g_get_monotonic_time() + 999) / 1000);
    source = g_timeout_source_new(deadline_ms);
    operation->deadline_source = source;
    g_source_set_callback(source,
                          stage_deadline_expired,
                          stage_operation_ref(operation),
                          (GDestroyNotify)stage_operation_unref);
    g_source_attach(source, operation->bridge->context);
    if (!stage_scheduler_submit(operation, error)) {
        stage_operation_disarm_deadline(operation);
        return FALSE;
    }
    return TRUE;
}

static void
stage_operation_abandon(StageOperation *operation)
{
    StageOperation *keep_alive = stage_operation_ref(operation);

    g_cancellable_cancel(operation->cancellable);
    stage_operation_disarm_deadline(operation);
    g_atomic_int_compare_and_exchange(&operation->outcome_claimed, 0, 1);
    stage_operation_unref(keep_alive);
}

#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
static void
stage_operation_cancel_and_deliver(StageOperation *operation)
{
    StageOperation *keep_alive = stage_operation_ref(operation);

    g_cancellable_cancel(operation->cancellable);
    stage_operation_disarm_deadline(operation);
    if (g_atomic_int_compare_and_exchange(&operation->outcome_claimed,
                                          0,
                                          1)) {
        operation->delivery_error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED,
            "file upload staging was cancelled");
        operation->ready_func(operation, operation->ready_data);
    }
    stage_operation_unref(keep_alive);
}
#endif

static StagedSelection *
stage_operation_accept_result(StageOperation *operation)
{
    StagedSelection *staged;

    if (operation->delivery_error || !operation->worker_result)
        return NULL;
    commit_reservations(operation);
    staged = operation->worker_result;
    operation->worker_result = NULL;
    return staged;
}

static void
stage_worker(gpointer data, gpointer user_data)
{
    StageOperation *operation = data;

    (void)user_data;
#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
    if (operation->stall_fd >= 0) {
        guint8 byte;
        ssize_t received;

        g_atomic_int_set(&operation->stall_entered, 1);
        do {
            received = read(operation->stall_fd, &byte, 1);
        } while (received < 0 && errno == EINTR);
        if (received <= 0)
            g_set_error_literal(&operation->worker_error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "test staging gate failed");
    }
#endif
    if (!operation->worker_error &&
        stage_checkpoint(operation, &operation->worker_error))
        operation->worker_result =
            stage_paths(operation, &operation->worker_error);
    if (!operation->worker_result && !operation->worker_error)
        g_set_error_literal(&operation->worker_error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "file upload staging failed without an error");
    g_atomic_int_set(&operation->worker_done, 1);
    g_mutex_lock(&scheduler_lock);
    g_assert_cmpuint(scheduled_jobs, >, 0);
    scheduled_jobs--;
    g_mutex_unlock(&scheduler_lock);
    {
        GSource *source = g_idle_source_new();

        g_source_set_priority(source, G_PRIORITY_DEFAULT);
        g_source_set_callback(source,
                              stage_worker_dispatch,
                              operation,
                              (GDestroyNotify)stage_operation_unref);
        g_source_attach(source, operation->bridge->context);
        g_source_unref(source);
    }
}

static gboolean
stage_worker_dispatch(gpointer data)
{
    StageOperation *operation = data;
    gboolean expired =
        g_get_monotonic_time() >= operation->deadline_us;

    if (operation->job_counted) {
        g_assert_cmpuint(operation->bridge->staging_jobs, >, 0);
        operation->bridge->staging_jobs--;
        operation->job_counted = FALSE;
    }
    stage_operation_disarm_deadline(operation);
    if (g_atomic_int_compare_and_exchange(&operation->outcome_claimed,
                                          0,
                                          1)) {
        if (expired) {
            g_cancellable_cancel(operation->cancellable);
            operation->delivery_error = g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "staging selected files exceeded its time budget");
        } else {
            operation->delivery_error = operation->worker_error;
            operation->worker_error = NULL;
        }
        operation->ready_func(operation, operation->ready_data);
    }
    if (operation->worker_result) {
        queue_staged_selection_cleanup(operation->worker_result);
        operation->worker_result = NULL;
    }
    g_clear_error(&operation->worker_error);
    release_reservations(operation);
    g_atomic_int_set(&operation->worker_dispatched, 1);
    return G_SOURCE_REMOVE;
}

static void
bridge_stage_ready(StageOperation *operation, gpointer user_data)
{
    MuxFileChooserBridge *bridge = operation->bridge;
    PendingChooser *pending;

    (void)user_data;
    pending = g_hash_table_lookup(bridge->pending,
                                  &operation->request_id);
    if (!pending || pending->operation != operation)
        return;
    pending = take_pending(bridge, operation->request_id);
    if (!pending)
        return;
    if (operation->delivery_error || bridge->disposing ||
        bridge->disposed) {
        if (!bridge->disposing && !bridge->disposed) {
            g_warning("File upload staging failed: %s",
                      operation->delivery_error
                          ? operation->delivery_error->message
                          : "file chooser was destroyed");
            send_cancel(bridge,
                        operation->request_id,
                        MUX_UI_CANCEL_UNDERLYING_GONE);
        }
        webkit_file_chooser_request_cancel(pending->request);
    } else {
        StagedSelection *staged =
            stage_operation_accept_result(operation);
        g_auto(GStrv) files = NULL;
        guint i;

        if (!staged) {
            webkit_file_chooser_request_cancel(pending->request);
        } else {
            files = g_new0(gchar *, staged->file_paths->len + 1);
            for (i = 0; i < staged->file_paths->len; i++)
                files[i] = g_strdup(
                    g_ptr_array_index(staged->file_paths, i));
            g_ptr_array_add(bridge->staged_selections, staged);
            webkit_file_chooser_request_select_files(
                pending->request, (const gchar *const *)files);
        }
    }
    pending_chooser_free(pending);
}

gboolean
mux_file_chooser_bridge_handle_payload(MuxFileChooserBridge *bridge,
                                       const guint8 *data,
                                       gsize length,
                                       GError **error)
{
    MuxUiRecordType type;

    g_return_val_if_fail(bridge, FALSE);
    if (bridge->disposing || bridge->disposed) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "file chooser bridge is closed");
        return FALSE;
    }
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;

    if (type == MUX_UI_RECORD_RESPONSE) {
        g_autoptr(MuxUiResponse) response = NULL;
        PendingChooser *pending;

        if (!mux_ui_response_decode(data, length, &response, error))
            return FALSE;
        pending = g_hash_table_lookup(bridge->pending,
                                      &response->request_id);
        if (!pending)
            return TRUE;
        if (response->action == MUX_UI_ACTION_SUBMIT) {
            StageOperation *operation;

            if (pending->operation) {
                g_set_error_literal(error,
                                    MUX_UI_ERROR,
                                    MUX_UI_ERROR_INVALID,
                                    "file chooser response was submitted twice");
                return FALSE;
            }
            operation = stage_operation_new(
                bridge,
                pending->request_id,
                pending->select_multiple,
                response->paths,
                MUX_UPLOAD_COPY_DEADLINE_MS,
                bridge_stage_ready,
                NULL,
                NULL,
                error);
            if (!operation || !stage_operation_start(operation, error)) {
                PendingChooser *failed =
                    take_pending(bridge, response->request_id);

                if (operation)
                    stage_operation_unref(operation);
                if (failed) {
                    webkit_file_chooser_request_cancel(failed->request);
                    pending_chooser_free(failed);
                }
                return FALSE;
            }
            pending->operation = operation;
        } else {
            pending = take_pending(bridge, response->request_id);
            if (pending) {
                webkit_file_chooser_request_cancel(pending->request);
                pending_chooser_free(pending);
            }
        }
        return TRUE;
    }

    if (type == MUX_UI_RECORD_CANCEL) {
        guint64 request_id;
        MuxUiCancelReason reason;
        PendingChooser *pending;

        if (!mux_ui_cancel_decode(
                data, length, &request_id, &reason, error))
            return FALSE;
        pending = take_pending(bridge, request_id);
        if (pending) {
            if (pending->operation)
                stage_operation_abandon(pending->operation);
            webkit_file_chooser_request_cancel(pending->request);
            pending_chooser_free(pending);
        }
        return TRUE;
    }

    g_set_error_literal(error,
                        MUX_UI_ERROR,
                        MUX_UI_ERROR_INVALID,
                        "file chooser received a UI request");
    return FALSE;
}

void
mux_file_chooser_bridge_cancel(MuxFileChooserBridge *bridge,
                               guint64 request_id,
                               MuxUiCancelReason reason,
                               gboolean notify_pane)
{
    PendingChooser *pending;

    g_return_if_fail(bridge);
    pending = take_pending(bridge, request_id);
    if (!pending)
        return;
    if (pending->operation)
        stage_operation_abandon(pending->operation);
    if (notify_pane)
        send_cancel(bridge, request_id, reason);
    webkit_file_chooser_request_cancel(pending->request);
    pending_chooser_free(pending);
}

void
mux_file_chooser_bridge_cancel_all(MuxFileChooserBridge *bridge,
                                   MuxUiCancelReason reason,
                                   gboolean notify_pane)
{
    MuxFileChooserBridge *keep_alive;

    g_return_if_fail(bridge);
    keep_alive = bridge_ref(bridge);
    while (g_hash_table_size(bridge->pending)) {
        GHashTableIter iterator;
        gpointer key;
        gpointer value;
        guint64 request_id;
        PendingChooser *pending;

        g_hash_table_iter_init(&iterator, bridge->pending);
        if (!g_hash_table_iter_next(&iterator, &key, &value))
            break;
        request_id = *(guint64 *)key;
        pending = take_pending(bridge, request_id);
        if (!pending)
            continue;
        if (pending->operation)
            stage_operation_abandon(pending->operation);
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        webkit_file_chooser_request_cancel(pending->request);
        pending_chooser_free(pending);
    }
    bridge_unref(keep_alive);
}

void
mux_file_chooser_bridge_free(MuxFileChooserBridge *bridge)
{
    if (!bridge || bridge->disposing || bridge->disposed)
        return;
    g_mutex_lock(&bridge->quota_lock);
    bridge->disposing = TRUE;
    g_mutex_unlock(&bridge->quota_lock);
    if (bridge->run_file_chooser_handler && bridge->web_view) {
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->run_file_chooser_handler);
        bridge->run_file_chooser_handler = 0;
    }
    mux_file_chooser_bridge_cancel_all(
        bridge, MUX_UI_CANCEL_VIEW_DESTROYED, TRUE);
    g_mutex_lock(&bridge->quota_lock);
    bridge->disposed = TRUE;
    g_mutex_unlock(&bridge->quota_lock);
    clear_staged_selections(bridge);
    if (bridge->user_data_destroy) {
        bridge->user_data_destroy(bridge->user_data);
        bridge->user_data_destroy = NULL;
        bridge->user_data = NULL;
    }
    g_clear_object(&bridge->web_view);
    bridge_unref(bridge);
}

guint
mux_file_chooser_bridge_pending_count(
    const MuxFileChooserBridge *bridge)
{
    g_return_val_if_fail(bridge, 0);
    return g_hash_table_size(bridge->pending);
}

#ifdef MUX_FILE_CHOOSER_ENGINE_TEST
static StageOperation *
file_chooser_test_stage(MuxFileChooserBridge **bridge_out,
                        const GPtrArray *paths,
                        guint deadline_ms,
                        gint stall_fd,
                        StageReadyFunc ready_func,
                        gpointer ready_data,
                        GError **error)
{
    MuxFileChooserBridge *bridge = bridge_alloc();
    StageOperation *operation = stage_operation_new(
        bridge,
        1,
        TRUE,
        paths,
        deadline_ms,
        ready_func,
        ready_data,
        NULL,
        error);

    if (!operation) {
        bridge_unref(bridge);
        return NULL;
    }
    if (stall_fd >= 0) {
        operation->stall_fd = fcntl(stall_fd, F_DUPFD_CLOEXEC, 3);
        if (operation->stall_fd < 0) {
            set_errno_error(error, "cannot duplicate test staging gate");
            stage_operation_unref(operation);
            bridge_unref(bridge);
            return NULL;
        }
    }
    if (!stage_operation_start(operation, error)) {
        stage_operation_unref(operation);
        bridge_unref(bridge);
        return NULL;
    }
    *bridge_out = bridge;
    return operation;
}
#endif
