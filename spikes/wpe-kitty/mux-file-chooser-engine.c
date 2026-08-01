#define _GNU_SOURCE

#include "mux-file-chooser-engine.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MUX_UPLOAD_MAX_FILES_PER_REQUEST 32u
#define MUX_UPLOAD_MAX_STAGED_FILES 64u
#define MUX_UPLOAD_MAX_STAGED_SELECTIONS 16u
#define MUX_UPLOAD_MAX_FILE_BYTES ((guint64)256 * 1024 * 1024)
#define MUX_UPLOAD_MAX_STAGED_BYTES ((guint64)512 * 1024 * 1024)
#define MUX_UPLOAD_COPY_BUFFER_BYTES (128u * 1024u)
#define MUX_UPLOAD_COPY_DEADLINE_US (15 * G_USEC_PER_SEC)

typedef struct {
    guint64 request_id;
    WebKitFileChooserRequest *request;
    gboolean select_multiple;
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
    WebKitWebView *web_view;
    GHashTable *pending;
    GPtrArray *staged_selections;
    guint staged_file_count;
    guint64 staged_bytes;
    MuxFileChooserSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong run_file_chooser_handler;
};

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
clear_staged_selections(MuxFileChooserBridge *bridge)
{
    if (!bridge->staged_selections)
        return;
    g_ptr_array_set_size(bridge->staged_selections, 0);
    bridge->staged_file_count = 0;
    bridge->staged_bytes = 0;
}

static void
pending_chooser_free(PendingChooser *pending)
{
    if (!pending)
        return;
    g_clear_object(&pending->request);
    g_free(pending);
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
    g_autoptr(GBytes) payload =
        mux_ui_cancel_encode(request_id, reason, &error);

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
    guint i;

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
            g_autofree gchar *mime =
                bounded_utf8(mime_types[i], 1024);

            g_ptr_array_add(
                request->choices,
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
    if (!bridge->send_func(payload, bridge->user_data, &error)) {
        PendingChooser *failed =
            take_pending(bridge, request->request_id);

        if (failed) {
            webkit_file_chooser_request_cancel(failed->request);
            pending_chooser_free(failed);
        }
    }
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
    bridge = g_new0(MuxFileChooserBridge, 1);
    bridge->web_view = g_object_ref(web_view);
    bridge->pending = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)pending_chooser_free);
    bridge->staged_selections = g_ptr_array_new_with_free_func(
        (GDestroyNotify)staged_selection_free);
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

void
mux_file_chooser_bridge_free(MuxFileChooserBridge *bridge)
{
    if (!bridge)
        return;
    if (bridge->run_file_chooser_handler)
        g_signal_handler_disconnect(
            bridge->web_view, bridge->run_file_chooser_handler);
    mux_file_chooser_bridge_cancel_all(
        bridge, MUX_UI_CANCEL_VIEW_DESTROYED, TRUE);
    g_clear_pointer(&bridge->pending, g_hash_table_unref);
    clear_staged_selections(bridge);
    g_clear_pointer(&bridge->staged_selections, g_ptr_array_unref);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_clear_object(&bridge->web_view);
    g_free(bridge);
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
write_all(int fd,
          const guint8 *data,
          gsize length,
          gint64 deadline_us,
          GError **error)
{
    gsize offset = 0;

    while (offset < length) {
        ssize_t written;

        if (g_get_monotonic_time() > deadline_us) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "staging selected files exceeded its time budget");
            return FALSE;
        }
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
copy_stable_snapshot(int source_fd,
                     int destination_fd,
                     const struct stat *before,
                     gint64 deadline_us,
                     GError **error)
{
    g_autofree guint8 *buffer =
        g_malloc(MUX_UPLOAD_COPY_BUFFER_BYTES);
    guint64 remaining = (guint64)before->st_size;
    struct stat after;
    struct stat destination;
    guint8 extra_byte;
    ssize_t extra;

    while (remaining) {
        gsize requested = (gsize)MIN(
            remaining, (guint64)MUX_UPLOAD_COPY_BUFFER_BYTES);
        ssize_t received;

        if (g_get_monotonic_time() > deadline_us) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_TIMED_OUT,
                                "staging selected files exceeded its time budget");
            return FALSE;
        }
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
        if (!write_all(destination_fd,
                       buffer,
                       (gsize)received,
                       deadline_us,
                       error))
            return FALSE;
        remaining -= (guint64)received;
    }

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
stage_source_path(MuxFileChooserBridge *bridge,
                  StagedSelection *staged,
                  GArray *identities,
                  int staging_fd,
                  const gchar *path,
                  gint64 deadline_us,
                  GError **error)
{
    g_autofree gchar *basename = NULL;
    g_autofree gchar *item_directory = NULL;
    g_autofree gchar *staged_path = NULL;
    gchar item_name[32];
    struct stat status;
    SourceIdentity identity;
    guint64 source_bytes;
    int source_fd = -1;
    int item_fd = -1;
    int destination_fd = -1;
    gboolean success = FALSE;

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
    if (bridge->staged_file_count + staged->file_paths->len >=
        MUX_UPLOAD_MAX_STAGED_FILES) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "too many upload files are retained by this chooser manager");
        goto out;
    }
    if (bridge->staged_bytes > MUX_UPLOAD_MAX_STAGED_BYTES ||
        staged->bytes >
            MUX_UPLOAD_MAX_STAGED_BYTES - bridge->staged_bytes) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "upload snapshot budget is exhausted");
        goto out;
    }
    source_bytes = (guint64)status.st_size;
    if (source_bytes > MUX_UPLOAD_MAX_STAGED_BYTES -
                           bridge->staged_bytes - staged->bytes) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "selected files exceed the 512 MiB chooser manager staging limit");
        goto out;
    }

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
    item_directory = g_build_filename(
        staged->directory, item_name, NULL);
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
                            basename,
                            O_WRONLY | O_CREAT | O_EXCL |
                                O_CLOEXEC | O_NOFOLLOW,
                            S_IRUSR | S_IWUSR);
    if (destination_fd < 0) {
        set_errno_error(error, "cannot create staged upload");
        goto out;
    }
    staged_path = g_build_filename(
        staged->directory, item_name, basename, NULL);
    g_ptr_array_add(staged->file_paths,
                    g_steal_pointer(&staged_path));
    if (!copy_stable_snapshot(source_fd,
                              destination_fd,
                              &status,
                              deadline_us,
                              error))
        goto out;
    if (close(destination_fd) < 0) {
        destination_fd = -1;
        set_errno_error(error, "cannot finalize staged upload");
        goto out;
    }
    destination_fd = -1;
    identity.device = status.st_dev;
    identity.inode = status.st_ino;
    g_array_append_val(identities, identity);
    staged->bytes += source_bytes;
    success = TRUE;

out:
    if (destination_fd >= 0)
        close(destination_fd);
    if (item_fd >= 0)
        close(item_fd);
    if (source_fd >= 0)
        close(source_fd);
    return success;
}

static StagedSelection *
stage_paths(MuxFileChooserBridge *bridge,
            const PendingChooser *pending,
            const GPtrArray *paths,
            GError **error)
{
    StagedSelection *staged = NULL;
    GArray *identities = NULL;
    struct stat directory_status;
    gint64 deadline_us;
    int staging_fd = -1;
    guint i;

    if (!paths || !paths->len ||
        (!pending->select_multiple && paths->len != 1) ||
        paths->len > MUX_UI_MAX_PATHS ||
        paths->len > MUX_UPLOAD_MAX_FILES_PER_REQUEST) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid number of selected files");
        return NULL;
    }
    if (bridge->staged_selections->len >=
        MUX_UPLOAD_MAX_STAGED_SELECTIONS) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "the chooser manager already retains 16 upload snapshots");
        return NULL;
    }

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
    deadline_us = g_get_monotonic_time() +
                  MUX_UPLOAD_COPY_DEADLINE_US;
    for (i = 0; i < paths->len; i++) {
        const gchar *path =
            g_ptr_array_index((GPtrArray *)paths, i);

        if (!stage_source_path(bridge,
                               staged,
                               identities,
                               staging_fd,
                               path,
                               deadline_us,
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

gboolean
mux_file_chooser_bridge_handle_payload(MuxFileChooserBridge *bridge,
                                       const guint8 *data,
                                       gsize length,
                                       GError **error)
{
    MuxUiRecordType type;

    g_return_val_if_fail(bridge, FALSE);
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;

    if (type == MUX_UI_RECORD_RESPONSE) {
        g_autoptr(MuxUiResponse) response = NULL;
        PendingChooser *pending;

        if (!mux_ui_response_decode(data, length, &response, error))
            return FALSE;
        pending = take_pending(bridge, response->request_id);
        if (!pending)
            return TRUE;
        if (response->action == MUX_UI_ACTION_SUBMIT) {
            StagedSelection *staged;
            g_auto(GStrv) files = NULL;
            guint i;

            staged = stage_paths(
                bridge, pending, response->paths, error);
            if (!staged) {
                webkit_file_chooser_request_cancel(pending->request);
                pending_chooser_free(pending);
                return FALSE;
            }
            files = g_new0(gchar *, staged->file_paths->len + 1);
            for (i = 0; i < staged->file_paths->len; i++)
                files[i] = g_strdup(
                    g_ptr_array_index(staged->file_paths, i));
            bridge->staged_file_count += staged->file_paths->len;
            bridge->staged_bytes += staged->bytes;
            g_ptr_array_add(bridge->staged_selections, staged);
            webkit_file_chooser_request_select_files(
                pending->request, (const gchar *const *)files);
        } else {
            webkit_file_chooser_request_cancel(pending->request);
        }
        pending_chooser_free(pending);
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
    g_return_if_fail(bridge);
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
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        webkit_file_chooser_request_cancel(pending->request);
        pending_chooser_free(pending);
    }
}

guint
mux_file_chooser_bridge_pending_count(
    const MuxFileChooserBridge *bridge)
{
    g_return_val_if_fail(bridge, 0);
    return g_hash_table_size(bridge->pending);
}
