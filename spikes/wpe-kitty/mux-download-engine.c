#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mux-download-engine.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif

typedef enum {
    DOWNLOAD_STATE_NEW,
    DOWNLOAD_STATE_WAITING_DESTINATION,
    DOWNLOAD_STATE_TRANSFERRING,
    DOWNLOAD_STATE_CANCELLING,
    DOWNLOAD_STATE_FAILED,
    DOWNLOAD_STATE_FINISHED,
} DownloadState;

typedef struct {
    MuxDownloadManager *manager;
    WebKitDownload *download;
    WebKitWebView *source_view;
    guint64 download_id;
    DownloadState state;
    gchar *suggested_filename;
    gchar *final_path;
    gchar *partial_path;
    gchar *final_name;
    gchar *partial_name;
    gchar *failure_message;
    gint directory_fd;
    gint reservation_fd;
    gint partial_fd;
    gboolean reservation_active;
    gboolean identity_mismatch;
    gboolean pending_slot_held;
    gboolean active_slot_held;
    gboolean destination_emitted;
    gboolean clipboard_target;
    gint64 last_progress_us;
    guint destination_timeout_id;
    gulong decide_destination_handler;
    gulong created_destination_handler;
    gulong received_data_handler;
    gulong failed_handler;
    gulong finished_handler;
} PendingDownload;

struct _MuxDownloadManager {
    WebKitNetworkSession *network_session;
    GHashTable *by_download;
    GHashTable *by_id;
    GHashTable *pending_by_view;
    GHashTable *active_by_view;
    guint pending_count;
    guint active_count;
    MuxDownloadSendFunc send_func;
    MuxDownloadEventFunc event_func;
    MuxDownloadClipboardFunc clipboard_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong download_started_handler;
    GHashTable *clipboard_requests;
    GPtrArray *clipboard_paths;
    gchar *clipboard_directory;
};

static gboolean safe_destination_directory(const struct stat *status);

static gpointer
download_view_key(MuxDownloadManager *manager,
                  WebKitWebView *source_view)
{
    return source_view ? (gpointer)source_view : (gpointer)manager;
}

static gboolean
reserve_count(MuxDownloadManager *manager,
              WebKitWebView *source_view,
              GHashTable *by_view,
              guint *global_count,
              guint per_view_limit,
              guint global_limit)
{
    gpointer key = download_view_key(manager, source_view);
    guint view_count =
        GPOINTER_TO_UINT(g_hash_table_lookup(by_view, key));

    if (*global_count >= global_limit ||
        view_count >= per_view_limit)
        return FALSE;
    (*global_count)++;
    g_hash_table_insert(
        by_view, key, GUINT_TO_POINTER(view_count + 1));
    return TRUE;
}

static void
release_count(MuxDownloadManager *manager,
              WebKitWebView *source_view,
              GHashTable *by_view,
              guint *global_count)
{
    gpointer key = download_view_key(manager, source_view);
    guint view_count =
        GPOINTER_TO_UINT(g_hash_table_lookup(by_view, key));

    g_assert(*global_count > 0);
    g_assert(view_count > 0);
    (*global_count)--;
    if (view_count == 1)
        g_hash_table_remove(by_view, key);
    else
        g_hash_table_insert(
            by_view, key, GUINT_TO_POINTER(view_count - 1));
}

static gboolean
reserve_pending_slot(PendingDownload *pending)
{
    MuxDownloadManager *manager = pending->manager;

    if (!reserve_count(manager,
                       pending->source_view,
                       manager->pending_by_view,
                       &manager->pending_count,
                       MUX_DOWNLOAD_MAX_PENDING_PER_VIEW,
                       MUX_DOWNLOAD_MAX_PENDING_GLOBAL))
        return FALSE;
    pending->pending_slot_held = TRUE;
    return TRUE;
}

static gboolean
reserve_active_slot(PendingDownload *pending)
{
    MuxDownloadManager *manager = pending->manager;

    if (!reserve_count(manager,
                       pending->source_view,
                       manager->active_by_view,
                       &manager->active_count,
                       MUX_DOWNLOAD_MAX_ACTIVE_PER_VIEW,
                       MUX_DOWNLOAD_MAX_ACTIVE_GLOBAL))
        return FALSE;
    pending->active_slot_held = TRUE;
    return TRUE;
}

static void
release_pending_slot(PendingDownload *pending)
{
    MuxDownloadManager *manager = pending->manager;

    if (!pending->pending_slot_held)
        return;
    release_count(manager,
                  pending->source_view,
                  manager->pending_by_view,
                  &manager->pending_count);
    pending->pending_slot_held = FALSE;
}

static void
release_active_slot(PendingDownload *pending)
{
    MuxDownloadManager *manager = pending->manager;

    if (!pending->active_slot_held)
        return;
    release_count(manager,
                  pending->source_view,
                  manager->active_by_view,
                  &manager->active_count);
    pending->active_slot_held = FALSE;
}

static void
release_download_slots(PendingDownload *pending)
{
    release_pending_slot(pending);
    release_active_slot(pending);
}

static guint64
next_download_id(MuxDownloadManager *manager)
{
    guint64 id;

    do {
        id = ((guint64)g_random_int() << 32) | g_random_int();
    } while (!id || g_hash_table_contains(manager->by_id, &id));
    return id;
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
sanitize_filename(const gchar *suggested)
{
    g_autofree gchar *basename =
        g_path_get_basename(suggested && *suggested ? suggested : "download");
    g_autofree gchar *valid = g_utf8_make_valid(basename, -1);
    GString *safe = g_string_sized_new(strlen(valid));
    const gchar *cursor;

    for (cursor = valid; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);

        if (g_unichar_iscntrl(character) || character == '/' ||
            character == '\\')
            g_string_append_c(safe, '_');
        else
            g_string_append_unichar(safe, character);
    }
    if (!safe->len || g_str_equal(safe->str, ".") ||
        g_str_equal(safe->str, "..")) {
        g_string_assign(safe, "download");
    }
    if (safe->str[0] == '.')
        g_string_prepend(safe, "download-");
    if (safe->len > 200) {
        gsize length = 200;

        while (length && !g_utf8_validate(safe->str, length, NULL))
            length--;
        g_string_truncate(safe, length);
    }
    return g_string_free(safe, FALSE);
}

static gchar *
default_download_directory(void)
{
    const gchar *configured =
        g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);

    if (configured && *configured)
        return g_strdup(configured);
    return g_build_filename(g_get_home_dir(), "Downloads", NULL);
}

static gboolean
ensure_clipboard_directory(MuxDownloadManager *manager, GError **error)
{
    struct stat status;

    if (manager->clipboard_directory)
        return TRUE;
    manager->clipboard_directory =
        g_dir_make_tmp("mux-download-clipboard-XXXXXX", error);
    if (!manager->clipboard_directory)
        return FALSE;
    errno = 0;
    if (g_chmod(manager->clipboard_directory, 0700) < 0 ||
        g_stat(manager->clipboard_directory, &status) < 0 ||
        !safe_destination_directory(&status) ||
        (status.st_mode & 0777) != 0700) {
        gint saved_errno = errno != 0 ? errno : EPERM;

        g_rmdir(manager->clipboard_directory);
        g_clear_pointer(&manager->clipboard_directory, g_free);
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot secure clipboard download directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    return TRUE;
}

static void
cleanup_clipboard_directory(MuxDownloadManager *manager)
{
    guint i;

    if (manager->clipboard_paths) {
        for (i = 0; i < manager->clipboard_paths->len; i++)
            g_unlink(g_ptr_array_index(manager->clipboard_paths, i));
        g_ptr_array_set_size(manager->clipboard_paths, 0);
    }
    if (manager->clipboard_directory) {
        g_rmdir(manager->clipboard_directory);
        g_clear_pointer(&manager->clipboard_directory, g_free);
    }
}

static gchar *
validated_destination_path(const gchar *path, GError **error)
{
    g_autofree gchar *canonical = NULL;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *basename = NULL;
    g_autofree gchar *safe_basename = NULL;
    gsize length;

    if (!path || !g_path_is_absolute(path)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "download destination must be absolute");
        return NULL;
    }
    length = strlen(path);
    if (!length || path[length - 1] == G_DIR_SEPARATOR) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "download destination must name a file");
        return NULL;
    }

    canonical = g_canonicalize_filename(path, NULL);
    directory = g_path_get_dirname(canonical);
    basename = g_path_get_basename(canonical);
    safe_basename = sanitize_filename(basename);
    if (!*basename || !g_str_equal(basename, safe_basename)) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "download filename contains hidden, control, path-like, or "
            "overlong content");
        return NULL;
    }
    if (g_file_test(canonical, G_FILE_TEST_IS_DIR) ||
        !g_file_test(directory, G_FILE_TEST_IS_DIR)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "download destination must name a file in an "
                            "existing directory");
        return NULL;
    }
    return g_steal_pointer(&canonical);
}

static gchar *
origin_for_view(WebKitWebView *web_view)
{
    const gchar *uri_string =
        web_view ? webkit_web_view_get_uri(web_view) : NULL;
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

static void
emit_event(PendingDownload *pending,
           MuxDownloadEventType type,
           const gchar *message)
{
    MuxDownloadManager *manager = pending->manager;
    MuxDownloadEvent event;

    if (!manager->event_func)
        return;
    event.type = type;
    event.download_id = pending->download_id;
    event.source_view = pending->source_view;
    event.path = pending->final_path;
    event.message = message;
    event.received_bytes =
        webkit_download_get_received_data_length(pending->download);
    event.estimated_progress =
        webkit_download_get_estimated_progress(pending->download);
    manager->event_func(&event, manager->user_data);
}

static void
close_owned_fd(gint *descriptor)
{
    if (*descriptor >= 0)
        close(*descriptor);
    *descriptor = -1;
}

static gboolean
same_inode(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev &&
           left->st_ino == right->st_ino;
}

static gboolean
safe_destination_directory(const struct stat *status)
{
    return S_ISDIR(status->st_mode) && status->st_uid == geteuid() &&
           !(status->st_mode & (S_IWGRP | S_IWOTH));
}

static gboolean
directory_path_matches(PendingDownload *pending)
{
    g_autofree gchar *directory = NULL;
    struct stat retained_status;
    struct stat pathname_status;
    gint pathname_fd;
    gboolean matches;

    if (pending->directory_fd < 0 || !pending->final_path)
        return FALSE;
    directory = g_path_get_dirname(pending->final_path);
    pathname_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pathname_fd < 0)
        return FALSE;
    matches = fstat(pending->directory_fd, &retained_status) == 0 &&
              fstat(pathname_fd, &pathname_status) == 0 &&
              safe_destination_directory(&retained_status) &&
              safe_destination_directory(&pathname_status) &&
              same_inode(&retained_status, &pathname_status);
    close(pathname_fd);
    return matches;
}

static gboolean
safe_owned_regular(const struct stat *status)
{
    return S_ISREG(status->st_mode) && status->st_uid == geteuid() &&
           status->st_nlink == 1;
}

static gboolean
path_matches_descriptor(gint directory_fd,
                        const gchar *name,
                        gint descriptor)
{
    struct stat descriptor_status;
    struct stat path_status;

    if (directory_fd < 0 || descriptor < 0 || !name || !*name ||
        fstat(descriptor, &descriptor_status) < 0 ||
        fstatat(directory_fd,
                name,
                &path_status,
                AT_SYMLINK_NOFOLLOW) < 0)
        return FALSE;
    return safe_owned_regular(&descriptor_status) &&
           safe_owned_regular(&path_status) &&
           same_inode(&descriptor_status, &path_status);
}

static gboolean
reservation_matches(PendingDownload *pending)
{
    return pending->reservation_active &&
           path_matches_descriptor(pending->directory_fd,
                                   pending->final_name,
                                   pending->reservation_fd);
}

static gboolean
partial_matches(PendingDownload *pending)
{
    return path_matches_descriptor(pending->directory_fd,
                                   pending->partial_name,
                                   pending->partial_fd);
}

static void
remove_partial(PendingDownload *pending)
{
    if (!pending->identity_mismatch && partial_matches(pending))
        unlinkat(pending->directory_fd, pending->partial_name, 0);
    close_owned_fd(&pending->partial_fd);
}

static void
remove_reservation(PendingDownload *pending)
{
    if (!pending->identity_mismatch && reservation_matches(pending))
        unlinkat(pending->directory_fd, pending->final_name, 0);
    pending->reservation_active = FALSE;
    close_owned_fd(&pending->reservation_fd);
}

static void
close_download_descriptors(PendingDownload *pending)
{
    close_owned_fd(&pending->partial_fd);
    close_owned_fd(&pending->reservation_fd);
    close_owned_fd(&pending->directory_fd);
}

static void
cleanup_files(PendingDownload *pending)
{
    remove_partial(pending);
    remove_reservation(pending);
    close_owned_fd(&pending->directory_fd);
}

static gint
rename_exchange(gint directory_fd,
                const gchar *first_name,
                const gchar *second_name)
{
#if defined(__linux__) && defined(SYS_renameat2)
    return (gint)syscall(SYS_renameat2,
                         directory_fd,
                         first_name,
                         directory_fd,
                         second_name,
                         RENAME_EXCHANGE);
#else
    (void)directory_fd;
    (void)first_name;
    (void)second_name;
    errno = ENOSYS;
    return -1;
#endif
}

static gboolean
exchange_unavailable(gint error_number)
{
    return error_number == ENOSYS || error_number == EINVAL ||
           error_number == EOPNOTSUPP || error_number == ENOTSUP;
}

/* Rollback is permitted only for the exact post-exchange layout, and succeeds
 * only if both original inode/name relationships are restored. */
static gboolean
rollback_exchange(PendingDownload *pending, gint *error_number)
{
    if (!path_matches_descriptor(pending->directory_fd,
                                 pending->final_name,
                                 pending->partial_fd) ||
        !path_matches_descriptor(pending->directory_fd,
                                 pending->partial_name,
                                 pending->reservation_fd)) {
        pending->identity_mismatch = TRUE;
        *error_number = ESTALE;
        return FALSE;
    }
    if (rename_exchange(pending->directory_fd,
                        pending->partial_name,
                        pending->final_name) < 0) {
        pending->identity_mismatch = TRUE;
        *error_number = errno;
        return FALSE;
    }
    if (!reservation_matches(pending) || !partial_matches(pending)) {
        pending->identity_mismatch = TRUE;
        *error_number = ESTALE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
retain_partial_descriptor(PendingDownload *pending, GError **error)
{
    struct stat status;
    gint descriptor;

    if (pending->partial_fd >= 0) {
        if (partial_matches(pending))
            return TRUE;
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "partial download pathname changed");
        return FALSE;
    }
    descriptor = openat(pending->directory_fd,
                        pending->partial_name,
                        O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT || errno == ELOOP)
            pending->identity_mismatch = TRUE;
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "cannot open partial download: %s",
                    g_strerror(errno));
        return FALSE;
    }
    if (fstat(descriptor, &status) < 0) {
        gint saved_errno = errno;

        close(descriptor);
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot inspect partial download: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (!safe_owned_regular(&status)) {
        pending->identity_mismatch = TRUE;
        close(descriptor);
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "partial download is not a private regular file");
        return FALSE;
    }
    pending->partial_fd = descriptor;
    if (!partial_matches(pending)) {
        pending->identity_mismatch = TRUE;
        close_owned_fd(&pending->partial_fd);
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "partial download pathname changed while opening");
        return FALSE;
    }
    return TRUE;
}

static void
disconnect_download(PendingDownload *pending)
{
    if (pending->decide_destination_handler)
        g_signal_handler_disconnect(
            pending->download, pending->decide_destination_handler);
    if (pending->created_destination_handler)
        g_signal_handler_disconnect(
            pending->download, pending->created_destination_handler);
    if (pending->received_data_handler)
        g_signal_handler_disconnect(
            pending->download, pending->received_data_handler);
    if (pending->failed_handler)
        g_signal_handler_disconnect(
            pending->download, pending->failed_handler);
    if (pending->finished_handler)
        g_signal_handler_disconnect(
            pending->download, pending->finished_handler);
    pending->decide_destination_handler = 0;
    pending->created_destination_handler = 0;
    pending->received_data_handler = 0;
    pending->failed_handler = 0;
    pending->finished_handler = 0;
}

static void
clear_destination_timeout(PendingDownload *pending)
{
    if (pending->destination_timeout_id)
        g_source_remove(pending->destination_timeout_id);
    pending->destination_timeout_id = 0;
}

static gboolean
prepare_download_cancel(PendingDownload *pending,
                        const gchar *failure_message)
{
    if (pending->state == DOWNLOAD_STATE_CANCELLING ||
        pending->state == DOWNLOAD_STATE_FAILED ||
        pending->state == DOWNLOAD_STATE_FINISHED)
        return FALSE;
    clear_destination_timeout(pending);
    if (failure_message) {
        g_free(pending->failure_message);
        pending->failure_message = bounded_utf8(failure_message, 8192);
    }
    pending->state = DOWNLOAD_STATE_CANCELLING;
    return TRUE;
}

static void
request_download_cancel(PendingDownload *pending,
                        const gchar *failure_message)
{
    if (prepare_download_cancel(pending, failure_message))
        webkit_download_cancel(pending->download);
}

static MuxDownloadEventType
failure_event_type(const PendingDownload *pending,
                   const GError *error)
{
    if (!pending->failure_message && error &&
        g_error_matches(error,
                        WEBKIT_DOWNLOAD_ERROR,
                        WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
        return MUX_DOWNLOAD_EVENT_CANCELLED;
    return MUX_DOWNLOAD_EVENT_FAILED;
}

static void
pending_download_free(PendingDownload *pending)
{
    if (!pending)
        return;
    clear_destination_timeout(pending);
    release_download_slots(pending);
    disconnect_download(pending);
    if (pending->state != DOWNLOAD_STATE_FINISHED)
        cleanup_files(pending);
    else
        close_download_descriptors(pending);
    g_clear_object(&pending->download);
    g_clear_object(&pending->source_view);
    g_free(pending->suggested_filename);
    g_free(pending->final_path);
    g_free(pending->partial_path);
    g_free(pending->final_name);
    g_free(pending->partial_name);
    g_free(pending->failure_message);
    g_free(pending);
}

static void
remove_pending(PendingDownload *pending)
{
    MuxDownloadManager *manager = pending->manager;

    g_hash_table_remove(manager->by_id, &pending->download_id);
    g_hash_table_steal(manager->by_download, pending->download);
    pending_download_free(pending);
}

static gchar *
numbered_path(const gchar *requested, guint number)
{
    g_autofree gchar *directory = g_path_get_dirname(requested);
    g_autofree gchar *basename = g_path_get_basename(requested);
    const gchar *dot = strrchr(basename, '.');
    g_autofree gchar *name = NULL;

    if (!number)
        return g_strdup(requested);
    if (dot && dot != basename) {
        g_autofree gchar *stem = g_strndup(basename, dot - basename);

        name = g_strdup_printf("%s (%u)%s", stem, number, dot);
    } else {
        name = g_strdup_printf("%s (%u)", basename, number);
    }
    return g_build_filename(directory, name, NULL);
}

static gboolean
reserve_final_path(PendingDownload *pending,
                   const gchar *requested,
                   GError **error)
{
    g_autofree gchar *directory = g_path_get_dirname(requested);
    struct stat directory_status;
    gint directory_fd;
    guint number;

    directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        gint saved_errno = errno;

        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot open download directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (fstat(directory_fd, &directory_status) < 0) {
        gint saved_errno = errno;

        close(directory_fd);
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot inspect download directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (!safe_destination_directory(&directory_status)) {
        close(directory_fd);
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "download directory must be owned by the current user and not "
            "group- or other-writable");
        return FALSE;
    }

    for (number = 0; number < 10000; number++) {
        g_autofree gchar *candidate = numbered_path(requested, number);
        g_autofree gchar *candidate_name =
            g_path_get_basename(candidate);
        struct stat status;
        gint descriptor = openat(directory_fd,
                                 candidate_name,
                                 O_WRONLY | O_CREAT | O_EXCL |
                                     O_CLOEXEC | O_NOFOLLOW,
                                 0600);

        if (descriptor < 0) {
            if (errno == EEXIST || errno == ELOOP)
                continue;
            close(directory_fd);
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(errno),
                        "cannot reserve download destination: %s",
                        g_strerror(errno));
            return FALSE;
        }
        if (fstat(descriptor, &status) < 0) {
            gint saved_errno = errno;

            if (path_matches_descriptor(
                    directory_fd, candidate_name, descriptor))
                unlinkat(directory_fd, candidate_name, 0);
            close(descriptor);
            close(directory_fd);
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(saved_errno),
                        "cannot inspect download destination: %s",
                        g_strerror(saved_errno));
            return FALSE;
        }
        if (!safe_owned_regular(&status) ||
            !path_matches_descriptor(
                directory_fd, candidate_name, descriptor)) {
            if (path_matches_descriptor(
                    directory_fd, candidate_name, descriptor))
                unlinkat(directory_fd, candidate_name, 0);
            close(descriptor);
            close(directory_fd);
            g_set_error_literal(error,
                                G_FILE_ERROR,
                                G_FILE_ERROR_FAILED,
                                "download reservation pathname changed");
            return FALSE;
        }
        pending->final_path = g_steal_pointer(&candidate);
        pending->final_name = g_steal_pointer(&candidate_name);
        pending->directory_fd = directory_fd;
        pending->reservation_fd = descriptor;
        pending->reservation_active = TRUE;
        return TRUE;
    }

    close(directory_fd);
    g_set_error_literal(error,
                        G_FILE_ERROR,
                        G_FILE_ERROR_EXIST,
                        "could not find an unused download filename");
    return FALSE;
}

static gboolean
choose_partial_path(PendingDownload *pending, GError **error)
{
    g_autofree gchar *directory =
        g_path_get_dirname(pending->final_path);
    g_autofree gchar *basename =
        g_path_get_basename(pending->final_path);
    guint attempt;

    for (attempt = 0; attempt < 128; attempt++) {
        g_autofree gchar *name = g_strdup_printf(
            ".%s.mux-part-%016" G_GINT64_MODIFIER "x-%08x",
            basename,
            pending->download_id,
            g_random_int());
        struct stat status;

        if (fstatat(pending->directory_fd,
                    name,
                    &status,
                    AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) {
                pending->partial_path =
                    g_build_filename(directory, name, NULL);
                pending->partial_name = g_steal_pointer(&name);
                return TRUE;
            }
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(errno),
                        "cannot inspect partial download destination: %s",
                        g_strerror(errno));
            return FALSE;
        }
    }
    g_set_error_literal(error,
                        G_FILE_ERROR,
                        G_FILE_ERROR_EXIST,
                        "could not allocate a partial download filename");
    return FALSE;
}

static gboolean
begin_destination(PendingDownload *pending,
                  const gchar *path,
                  GError **error)
{
    g_autofree gchar *canonical = NULL;
    g_autofree gchar *partial_uri = NULL;

    clear_destination_timeout(pending);
    canonical = validated_destination_path(path, error);
    if (!canonical)
        return FALSE;
    if (!reserve_final_path(pending, canonical, error) ||
        !choose_partial_path(pending, error)) {
        cleanup_files(pending);
        return FALSE;
    }
    partial_uri = g_filename_to_uri(pending->partial_path, NULL, error);
    if (!partial_uri) {
        cleanup_files(pending);
        return FALSE;
    }
    if (!directory_path_matches(pending)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "download directory pathname changed before "
                            "handoff");
        cleanup_files(pending);
        return FALSE;
    }
    webkit_download_set_allow_overwrite(pending->download, FALSE);
    pending->state = DOWNLOAD_STATE_TRANSFERRING;
    release_pending_slot(pending);
    webkit_download_set_destination(pending->download, partial_uri);
    return TRUE;
}

static gboolean
finalize_download(PendingDownload *pending, GError **error)
{
    struct stat reservation_status;
    gint exchange_errno;
    gint rollback_errno = 0;

    if (!directory_path_matches(pending)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "download directory pathname changed before "
                            "finalization");
        return FALSE;
    }
    if (!retain_partial_descriptor(pending, error))
        return FALSE;
    if (!reservation_matches(pending)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "download destination reservation changed");
        return FALSE;
    }
    if (!partial_matches(pending)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "partial download pathname changed");
        return FALSE;
    }
    if (fsync(pending->partial_fd) < 0) {
        gint saved_errno = errno;

        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot synchronize completed download: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (rename_exchange(pending->directory_fd,
                        pending->partial_name,
                        pending->final_name) < 0) {
        exchange_errno = errno;
        if (exchange_unavailable(exchange_errno)) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        "atomic download finalization is unavailable: %s",
                        g_strerror(exchange_errno));
            return FALSE;
        }
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(exchange_errno),
                    "cannot finalize download: %s",
                    g_strerror(exchange_errno));
        return FALSE;
    }

    if (!path_matches_descriptor(pending->directory_fd,
                                 pending->final_name,
                                 pending->partial_fd) ||
        !path_matches_descriptor(pending->directory_fd,
                                 pending->partial_name,
                                 pending->reservation_fd)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_FAILED,
            "download destination changed during atomic finalization; no "
            "further pathname operation was attempted");
        return FALSE;
    }

    if (unlinkat(pending->directory_fd, pending->partial_name, 0) < 0) {
        exchange_errno = errno;
        if (rollback_exchange(pending, &rollback_errno)) {
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(exchange_errno),
                        "cannot remove download reservation; the exchange "
                        "was rolled back: %s",
                        g_strerror(exchange_errno));
        } else {
            g_set_error(error,
                        G_FILE_ERROR,
                        G_FILE_ERROR_FAILED,
                        "cannot remove download reservation (%s) and safe "
                        "rollback failed (%s)",
                        g_strerror(exchange_errno),
                        g_strerror(rollback_errno));
        }
        return FALSE;
    }

    if (fstat(pending->reservation_fd, &reservation_status) < 0 ||
        reservation_status.st_nlink != 0 ||
        !path_matches_descriptor(pending->directory_fd,
                                 pending->final_name,
                                 pending->partial_fd)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "atomic download finalization postcondition "
                            "failed");
        return FALSE;
    }
    pending->reservation_active = FALSE;
    if (fsync(pending->directory_fd) < 0) {
        gint saved_errno = errno;

        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "cannot synchronize finalized download directory: %s",
                    g_strerror(saved_errno));
        return FALSE;
    }
    if (!directory_path_matches(pending) ||
        !path_matches_descriptor(pending->directory_fd,
                                 pending->final_name,
                                 pending->partial_fd)) {
        pending->identity_mismatch = TRUE;
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "download directory pathname changed while "
                            "finalizing");
        return FALSE;
    }
    close_download_descriptors(pending);
    return TRUE;
}

static gboolean
destination_timeout(gpointer data)
{
    PendingDownload *pending = data;

    pending->destination_timeout_id = 0;
    if (pending->state != DOWNLOAD_STATE_WAITING_DESTINATION)
        return G_SOURCE_REMOVE;
    request_download_cancel(pending,
                            "download destination prompt timed out");
    return G_SOURCE_REMOVE;
}

static gboolean
on_decide_destination(WebKitDownload *download,
                      const gchar *suggested_filename,
                      PendingDownload *pending)
{
    g_autofree gchar *download_directory = NULL;
    g_autofree gchar *safe_name =
        sanitize_filename(suggested_filename);
    g_autofree gchar *default_path = NULL;
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_DOWNLOAD_DESTINATION);
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *failure = NULL;

    (void)download;
    if (pending->state != DOWNLOAD_STATE_NEW) {
        request_download_cancel(pending,
                                "download requested a destination twice");
        return TRUE;
    }
    if (pending->clipboard_target) {
        if (!ensure_clipboard_directory(pending->manager, &error)) {
            request_download_cancel(pending, error->message);
            return TRUE;
        }
        download_directory =
            g_strdup(pending->manager->clipboard_directory);
    } else {
        download_directory = default_download_directory();
    }
    default_path = g_build_filename(download_directory, safe_name, NULL);
    if (!pending->clipboard_target &&
        g_mkdir_with_parents(download_directory, 0700) < 0) {
        failure = g_strdup_printf("cannot create download directory: %s",
                                  g_strerror(errno));
        request_download_cancel(pending, failure);
        return TRUE;
    }
    g_free(pending->suggested_filename);
    pending->suggested_filename = g_strdup(safe_name);
    pending->state = DOWNLOAD_STATE_WAITING_DESTINATION;
    if (pending->clipboard_target) {
        if (!begin_destination(pending, default_path, &error))
            request_download_cancel(pending, error->message);
        return TRUE;
    }
    clear_destination_timeout(pending);
    pending->destination_timeout_id = g_timeout_add(
        MUX_DOWNLOAD_DESTINATION_TIMEOUT_MS,
        destination_timeout,
        pending);

    request->request_id = pending->download_id;
    request->deadline_ms = MUX_DOWNLOAD_DESTINATION_TIMEOUT_MS;
    request->origin = origin_for_view(pending->source_view);
    request->heading = g_strdup("Download");
    request->message =
        g_strdup_printf("Save %s", pending->suggested_filename);
    request->default_value = g_strdup(default_path);
    payload = mux_ui_request_encode(request, &error);
    if (!payload ||
        !pending->manager->send_func(pending->source_view,
                                     payload,
                                     pending->manager->user_data,
                                     &error)) {
        request_download_cancel(
            pending,
            error ? error->message
                  : "could not show the download destination prompt");
    }
    return TRUE;
}

static void
on_created_destination(WebKitDownload *download,
                       const gchar *destination,
                       PendingDownload *pending)
{
    g_autoptr(GError) error = NULL;

    (void)download;
    (void)destination;
    if (pending->state != DOWNLOAD_STATE_TRANSFERRING ||
        pending->destination_emitted)
        return;
    if (!retain_partial_descriptor(pending, &error)) {
        request_download_cancel(pending, error->message);
        return;
    }
    pending->destination_emitted = TRUE;
    emit_event(pending, MUX_DOWNLOAD_EVENT_DESTINATION, NULL);
}

static void
on_received_data(WebKitDownload *download,
                 guint64 data_length,
                 PendingDownload *pending)
{
    gint64 now = g_get_monotonic_time();

    (void)download;
    (void)data_length;
    if (pending->state != DOWNLOAD_STATE_TRANSFERRING)
        return;
    if (now - pending->last_progress_us < 250000)
        return;
    pending->last_progress_us = now;
    emit_event(pending, MUX_DOWNLOAD_EVENT_PROGRESS, NULL);
}

static void
on_failed(WebKitDownload *download,
          GError *error,
          PendingDownload *pending)
{
    MuxDownloadEventType event_type;

    (void)download;
    if (pending->state == DOWNLOAD_STATE_FAILED ||
        pending->state == DOWNLOAD_STATE_FINISHED)
        return;
    clear_destination_timeout(pending);
    event_type = failure_event_type(pending, error);
    pending->state = DOWNLOAD_STATE_FAILED;
    release_download_slots(pending);
    if (!pending->failure_message &&
        event_type == MUX_DOWNLOAD_EVENT_FAILED) {
        pending->failure_message = bounded_utf8(
            error ? error->message : "download failed", 8192);
    }
    cleanup_files(pending);
    emit_event(pending, event_type, pending->failure_message);
}

static void
on_finished(WebKitDownload *download, PendingDownload *pending)
{
    g_autoptr(GError) error = NULL;
    MuxDownloadManager *manager;
    MuxDownloadEventFunc event_func;
    MuxDownloadEvent event = { 0 };
    WebKitWebView *source_view = NULL;
    gpointer event_user_data;
    g_autofree gchar *event_path = NULL;
    g_autofree gchar *event_message = NULL;
    gboolean emit_terminal = FALSE;

    (void)download;
    clear_destination_timeout(pending);
    if (pending->state == DOWNLOAD_STATE_CANCELLING) {
        release_download_slots(pending);
        cleanup_files(pending);
        pending->state = DOWNLOAD_STATE_FAILED;
        event.type = pending->failure_message
                         ? MUX_DOWNLOAD_EVENT_FAILED
                         : MUX_DOWNLOAD_EVENT_CANCELLED;
        event_message = g_strdup(pending->failure_message);
        emit_terminal = TRUE;
    } else if (pending->state != DOWNLOAD_STATE_FAILED) {
        if (finalize_download(pending, &error) &&
            pending->clipboard_target) {
            MuxDownloadManager *manager = pending->manager;
            MuxDownloadClipboardFunc clipboard_func =
                manager->clipboard_func;
            WebKitURIResponse *response =
                webkit_download_get_response(pending->download);
            g_autofree gchar *mime_type = g_strdup(
                response ? webkit_uri_response_get_mime_type(response)
                         : NULL);
            g_autofree gchar *path = g_strdup(pending->final_path);
            WebKitWebView *view = pending->source_view
                                      ? g_object_ref(pending->source_view)
                                      : NULL;
            gpointer callback_data = manager->user_data;

            if (g_chmod(path, 0600) < 0 || !clipboard_func) {
                g_autofree gchar *message = !clipboard_func
                    ? g_strdup("clipboard publication is unavailable")
                    : g_strdup_printf("cannot secure clipboard file: %s",
                                      g_strerror(errno));

                g_unlink(path);
                pending->state = DOWNLOAD_STATE_FAILED;
                remove_pending(pending);
                if (view)
                    g_object_unref(view);
                g_warning("download to clipboard failed: %s", message);
                return;
            }

            pending->state = DOWNLOAD_STATE_FINISHED;
            g_ptr_array_add(manager->clipboard_paths, g_strdup(path));
            remove_pending(pending);
            if (!clipboard_func(view,
                                path,
                                mime_type,
                                callback_data,
                                &error))
                g_warning("download to clipboard publication failed: %s",
                          error ? error->message : "unknown error");
            if (view)
                g_object_unref(view);
            return;
        } else if (!error) {
            pending->state = DOWNLOAD_STATE_FINISHED;
            event.type = MUX_DOWNLOAD_EVENT_FINISHED;
        } else {
            pending->state = DOWNLOAD_STATE_FAILED;
            cleanup_files(pending);
            event.type = MUX_DOWNLOAD_EVENT_FAILED;
            event_message = g_strdup(error->message);
        }
        emit_terminal = TRUE;
    }

    if (!emit_terminal) {
        remove_pending(pending);
        return;
    }

    manager = pending->manager;
    event_func = manager->event_func;
    event_user_data = manager->user_data;
    source_view = pending->source_view
                      ? g_object_ref(pending->source_view)
                      : NULL;
    event_path = g_strdup(pending->final_path);
    event.download_id = pending->download_id;
    event.source_view = source_view;
    event.path = event_path;
    event.message = event_message;
    event.received_bytes =
        webkit_download_get_received_data_length(pending->download);
    event.estimated_progress =
        webkit_download_get_estimated_progress(pending->download);

    /* The callback may free the manager, so detach first and touch no
     * manager-owned state after invoking it. */
    remove_pending(pending);
    if (event_func)
        event_func(&event, event_user_data);
    g_clear_object(&source_view);
}

static void
on_download_started(WebKitNetworkSession *network_session,
                    WebKitDownload *download,
                    MuxDownloadManager *manager)
{
    PendingDownload *pending = g_new0(PendingDownload, 1);
    WebKitWebView *source_view =
        webkit_download_get_web_view(download);

    (void)network_session;
    pending->directory_fd = -1;
    pending->reservation_fd = -1;
    pending->partial_fd = -1;
    pending->manager = manager;
    pending->download = g_object_ref(download);
    pending->source_view = source_view ? g_object_ref(source_view) : NULL;
    pending->clipboard_target =
        g_hash_table_remove(manager->clipboard_requests, download);
    if (!reserve_pending_slot(pending) ||
        !reserve_active_slot(pending)) {
        pending_download_free(pending);
        webkit_download_cancel(download);
        return;
    }
    pending->download_id = next_download_id(manager);
    pending->state = DOWNLOAD_STATE_NEW;
    pending->last_progress_us = g_get_monotonic_time();
    pending->decide_destination_handler =
        g_signal_connect(download,
                         "decide-destination",
                         G_CALLBACK(on_decide_destination),
                         pending);
    pending->created_destination_handler =
        g_signal_connect(download,
                         "created-destination",
                         G_CALLBACK(on_created_destination),
                         pending);
    pending->received_data_handler =
        g_signal_connect(download,
                         "received-data",
                         G_CALLBACK(on_received_data),
                         pending);
    pending->failed_handler =
        g_signal_connect(download,
                         "failed",
                         G_CALLBACK(on_failed),
                         pending);
    pending->finished_handler =
        g_signal_connect(download,
                         "finished",
                         G_CALLBACK(on_finished),
                         pending);
    g_hash_table_insert(manager->by_download, download, pending);
    g_hash_table_insert(
        manager->by_id, &pending->download_id, pending);
    emit_event(pending, MUX_DOWNLOAD_EVENT_STARTED, NULL);
}

MuxDownloadManager *
mux_download_manager_new(WebKitNetworkSession *network_session,
                         MuxDownloadSendFunc send_func,
                         MuxDownloadEventFunc event_func,
                         gpointer user_data,
                         GDestroyNotify user_data_destroy)
{
    MuxDownloadManager *manager;

    g_return_val_if_fail(WEBKIT_IS_NETWORK_SESSION(network_session), NULL);
    g_return_val_if_fail(send_func, NULL);
    manager = g_new0(MuxDownloadManager, 1);
    manager->network_session = g_object_ref(network_session);
    manager->by_download = g_hash_table_new_full(
        g_direct_hash,
        g_direct_equal,
        NULL,
        (GDestroyNotify)pending_download_free);
    manager->by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
    manager->pending_by_view =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    manager->active_by_view =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    manager->clipboard_requests = g_hash_table_new_full(g_direct_hash,
                                                        g_direct_equal,
                                                        g_object_unref,
                                                        NULL);
    manager->clipboard_paths = g_ptr_array_new_with_free_func(g_free);
    manager->send_func = send_func;
    manager->event_func = event_func;
    manager->user_data = user_data;
    manager->user_data_destroy = user_data_destroy;
    manager->download_started_handler =
        g_signal_connect(network_session,
                         "download-started",
                         G_CALLBACK(on_download_started),
                         manager);
    return manager;
}

void
mux_download_manager_free(MuxDownloadManager *manager)
{
    GHashTableIter iterator;
    gpointer value;

    if (!manager)
        return;
    if (manager->download_started_handler)
        g_signal_handler_disconnect(
            manager->network_session,
            manager->download_started_handler);
    g_hash_table_iter_init(&iterator, manager->by_download);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingDownload *pending = value;

        disconnect_download(pending);
        webkit_download_cancel(pending->download);
    }
    g_hash_table_remove_all(manager->by_id);
    g_clear_pointer(&manager->by_download, g_hash_table_unref);
    g_clear_pointer(&manager->by_id, g_hash_table_unref);
    g_assert(manager->pending_count == 0);
    g_assert(manager->active_count == 0);
    g_clear_pointer(&manager->pending_by_view, g_hash_table_unref);
    g_clear_pointer(&manager->active_by_view, g_hash_table_unref);
    g_clear_pointer(&manager->clipboard_requests, g_hash_table_unref);
    cleanup_clipboard_directory(manager);
    g_clear_pointer(&manager->clipboard_paths, g_ptr_array_unref);
    if (manager->user_data_destroy)
        manager->user_data_destroy(manager->user_data);
    g_clear_object(&manager->network_session);
    g_free(manager);
}

void
mux_download_manager_set_clipboard_func(
    MuxDownloadManager *manager,
    MuxDownloadClipboardFunc clipboard_func)
{
    g_return_if_fail(manager);
    manager->clipboard_func = clipboard_func;
}

gboolean
mux_download_manager_download_uri_to_clipboard(
    MuxDownloadManager *manager,
    WebKitWebView *source_view,
    const gchar *uri,
    GError **error)
{
    g_autofree gchar *scheme = NULL;
    WebKitDownload *download;
    PendingDownload *pending;

    g_return_val_if_fail(manager, FALSE);
    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(source_view), FALSE);
    if (!uri || !*uri || strlen(uri) > MUX_UI_MAX_PATH ||
        !g_utf8_validate(uri, -1, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "clipboard download URI is invalid");
        return FALSE;
    }
    scheme = g_uri_parse_scheme(uri);
    if (!scheme ||
        !(g_ascii_strcasecmp(scheme, "http") == 0 ||
          g_ascii_strcasecmp(scheme, "https") == 0 ||
          g_ascii_strcasecmp(scheme, "ftp") == 0 ||
          g_ascii_strcasecmp(scheme, "data") == 0 ||
          g_ascii_strcasecmp(scheme, "blob") == 0 ||
          g_ascii_strcasecmp(scheme, "file") == 0)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "clipboard download URI scheme is unsupported");
        return FALSE;
    }

    download = webkit_web_view_download_uri(source_view, uri);
    if (!download) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "WebKit did not create the clipboard download");
        return FALSE;
    }
    pending = g_hash_table_lookup(manager->by_download, download);
    if (pending) {
        pending->clipboard_target = TRUE;
    } else {
        g_hash_table_add(manager->clipboard_requests,
                         g_object_ref(download));
    }
    g_object_unref(download);
    return TRUE;
}

gboolean
mux_download_manager_handle_payload(MuxDownloadManager *manager,
                                    const guint8 *data,
                                    gsize length,
                                    GError **error)
{
    MuxUiRecordType type;
    guint64 download_id;
    PendingDownload *pending;

    g_return_val_if_fail(manager, FALSE);
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;

    if (type == MUX_UI_RECORD_CANCEL) {
        MuxUiCancelReason reason;

        if (!mux_ui_cancel_decode(
                data, length, &download_id, &reason, error))
            return FALSE;
        pending = g_hash_table_lookup(manager->by_id, &download_id);
        if (pending) {
            request_download_cancel(pending, NULL);
        }
        return TRUE;
    }

    if (type == MUX_UI_RECORD_RESPONSE) {
        g_autoptr(MuxUiResponse) response = NULL;

        if (!mux_ui_response_decode(data, length, &response, error))
            return FALSE;
        pending =
            g_hash_table_lookup(manager->by_id, &response->request_id);
        if (!pending ||
            pending->state != DOWNLOAD_STATE_WAITING_DESTINATION)
            return TRUE;
        if (response->action == MUX_UI_ACTION_CANCEL ||
            response->action == MUX_UI_ACTION_UNSUPPORTED) {
            request_download_cancel(pending, NULL);
            return TRUE;
        }
        if (response->action != MUX_UI_ACTION_SUBMIT) {
            g_set_error_literal(
                error,
                MUX_UI_ERROR,
                MUX_UI_ERROR_INVALID,
                "invalid download destination response");
            request_download_cancel(
                pending, "invalid download destination response");
            return FALSE;
        }
        if (!begin_destination(pending, response->value, error)) {
            request_download_cancel(
                pending,
                error && *error
                    ? (*error)->message
                    : "invalid download destination");
            return FALSE;
        }
        return TRUE;
    }

    g_set_error_literal(error,
                        MUX_UI_ERROR,
                        MUX_UI_ERROR_INVALID,
                        "download manager received a UI request");
    return FALSE;
}

void
mux_download_manager_cancel(MuxDownloadManager *manager,
                            guint64 download_id)
{
    PendingDownload *pending;

    g_return_if_fail(manager);
    pending = g_hash_table_lookup(manager->by_id, &download_id);
    if (pending)
        request_download_cancel(pending, NULL);
}

void
mux_download_manager_cancel_view(MuxDownloadManager *manager,
                                 WebKitWebView *source_view)
{
    GHashTableIter iterator;
    gpointer value;
    g_autoptr(GPtrArray) downloads =
        g_ptr_array_new_with_free_func(g_object_unref);

    g_return_if_fail(manager);
    g_return_if_fail(WEBKIT_IS_WEB_VIEW(source_view));

    g_hash_table_iter_init(&iterator, manager->by_download);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingDownload *pending = value;

        if (pending->source_view == source_view)
            g_ptr_array_add(downloads,
                            g_object_ref(pending->download));
    }
    for (guint index = 0; index < downloads->len; index++) {
        WebKitDownload *download = g_ptr_array_index(downloads, index);
        PendingDownload *pending =
            g_hash_table_lookup(manager->by_download, download);

        if (!pending || pending->source_view != source_view)
            continue;
        prepare_download_cancel(pending, NULL);
        disconnect_download(pending);
        remove_pending(pending);
        webkit_download_cancel(download);
    }
}

void
mux_download_manager_cancel_all(MuxDownloadManager *manager)
{
    GHashTableIter iterator;
    gpointer value;
    g_autoptr(GPtrArray) downloads =
        g_ptr_array_new_with_free_func(g_object_unref);
    guint i;

    g_return_if_fail(manager);
    g_hash_table_iter_init(&iterator, manager->by_download);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingDownload *pending = value;

        prepare_download_cancel(pending, NULL);
        g_ptr_array_add(
            downloads,
            g_object_ref(pending->download));
    }
    for (i = 0; i < downloads->len; i++)
        webkit_download_cancel(g_ptr_array_index(downloads, i));
}

guint
mux_download_manager_count(const MuxDownloadManager *manager)
{
    g_return_val_if_fail(manager, 0);
    return g_hash_table_size(manager->by_download);
}
