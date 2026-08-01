#define _GNU_SOURCE

#include "mux-session-state.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SESSION_VERSION 1
#define MAX_STATE_BYTES (4u * 1024u * 1024u)
#define MAX_LAYERS 512u
#define MAX_VIEWS 4096u
#define MAX_LAYER_BYTES 128u
#define MAX_URI_BYTES (64u * 1024u)
#define MAX_TITLE_BYTES (8u * 1024u)

struct _MuxSessionState {
    guint64 next_view_id;
    gchar *active_layer;
    GPtrArray *layers;
    GPtrArray *views;
};

static void
session_view_free(gpointer data)
{
    MuxSessionView *view = data;

    if (view == NULL)
        return;
    g_free(view->layer);
    g_free(view->uri);
    g_free(view->title);
    g_free(view);
}

static MuxSessionState *
session_state_alloc(void)
{
    MuxSessionState *state = g_new0(MuxSessionState, 1);

    state->next_view_id = 1;
    state->layers = g_ptr_array_new_with_free_func(g_free);
    state->views = g_ptr_array_new_with_free_func(session_view_free);
    return state;
}

static gboolean
valid_layer(const gchar *layer)
{
    gsize length;

    if (layer == NULL)
        return FALSE;
    length = strlen(layer);
    if (length == 0 || length > MAX_LAYER_BYTES)
        return FALSE;
    for (const guchar *cursor = (const guchar *)layer;
         *cursor != '\0';
         cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '.' &&
            *cursor != '_' && *cursor != '-')
            return FALSE;
    }
    return TRUE;
}

static gboolean
valid_text(const gchar *text, gsize maximum)
{
    return text != NULL && strlen(text) <= maximum &&
           g_utf8_validate(text, -1, NULL);
}

static gboolean
state_has_layer(const MuxSessionState *state, const gchar *layer)
{
    for (guint i = 0; i < state->layers->len; i++) {
        if (g_strcmp0(g_ptr_array_index(state->layers, i), layer) == 0)
            return TRUE;
    }
    return FALSE;
}

static MuxSessionView *
state_find_view(const MuxSessionState *state, guint64 id)
{
    for (guint i = 0; i < state->views->len; i++) {
        MuxSessionView *view = g_ptr_array_index(state->views, i);

        if (view->id == id)
            return view;
    }
    return NULL;
}

static void
set_errno_error(GError **error, const gchar *operation, int error_number)
{
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(error_number),
                "%s: %s",
                operation,
                g_strerror(error_number));
}

static gboolean
set_invalid_data(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_DATA,
                        message);
    return FALSE;
}

MuxSessionState *
mux_session_state_new(void)
{
    MuxSessionState *state = session_state_alloc();

    state->active_layer = g_strdup("main");
    g_ptr_array_add(state->layers, g_strdup("main"));
    return state;
}

void
mux_session_state_free(MuxSessionState *state)
{
    if (state == NULL)
        return;
    g_free(state->active_layer);
    g_ptr_array_unref(state->layers);
    g_ptr_array_unref(state->views);
    g_free(state);
}

gchar *
mux_session_state_default_path(void)
{
    return g_build_filename(g_get_user_state_dir(),
                            "mux",
                            "workspace-v1.ini",
                            NULL);
}

guint64
mux_session_state_get_next_view_id(const MuxSessionState *state)
{
    g_return_val_if_fail(state != NULL, 1);
    return state->next_view_id;
}

gboolean
mux_session_state_set_next_view_id(MuxSessionState *state,
                                   guint64 next_view_id)
{
    g_return_val_if_fail(state != NULL, FALSE);
    if (next_view_id == 0)
        return FALSE;
    state->next_view_id = next_view_id;
    return TRUE;
}

const gchar *
mux_session_state_get_active_layer(const MuxSessionState *state)
{
    g_return_val_if_fail(state != NULL, "main");
    return state->active_layer;
}

gboolean
mux_session_state_add_layer(MuxSessionState *state, const gchar *layer)
{
    g_return_val_if_fail(state != NULL, FALSE);
    if (!valid_layer(layer))
        return FALSE;
    if (state_has_layer(state, layer))
        return TRUE;
    if (state->layers->len >= MAX_LAYERS)
        return FALSE;
    g_ptr_array_add(state->layers, g_strdup(layer));
    return TRUE;
}

gboolean
mux_session_state_set_active_layer(MuxSessionState *state,
                                   const gchar *layer)
{
    g_return_val_if_fail(state != NULL, FALSE);
    if (!mux_session_state_add_layer(state, layer))
        return FALSE;
    if (g_strcmp0(state->active_layer, layer) != 0) {
        g_free(state->active_layer);
        state->active_layer = g_strdup(layer);
    }
    return TRUE;
}

guint
mux_session_state_get_layer_count(const MuxSessionState *state)
{
    g_return_val_if_fail(state != NULL, 0);
    return state->layers->len;
}

const gchar *
mux_session_state_get_layer(const MuxSessionState *state, guint index)
{
    g_return_val_if_fail(state != NULL, NULL);
    if (index >= state->layers->len)
        return NULL;
    return g_ptr_array_index(state->layers, index);
}

gboolean
mux_session_state_upsert_view(MuxSessionState *state,
                              guint64 id,
                              const gchar *layer,
                              const gchar *uri,
                              const gchar *title)
{
    MuxSessionView *view;

    g_return_val_if_fail(state != NULL, FALSE);
    if (id == 0 || !valid_layer(layer) ||
        !valid_text(uri, MAX_URI_BYTES) ||
        !valid_text(title, MAX_TITLE_BYTES))
        return FALSE;

    view = state_find_view(state, id);
    if (view == NULL && state->views->len >= MAX_VIEWS)
        return FALSE;
    if (!mux_session_state_add_layer(state, layer))
        return FALSE;

    if (view == NULL) {
        view = g_new0(MuxSessionView, 1);
        view->id = id;
        g_ptr_array_add(state->views, view);
    }
    g_free(view->layer);
    g_free(view->uri);
    g_free(view->title);
    view->layer = g_strdup(layer);
    view->uri = g_strdup(uri);
    view->title = g_strdup(title);
    return TRUE;
}

gboolean
mux_session_state_remove_view(MuxSessionState *state, guint64 id)
{
    g_return_val_if_fail(state != NULL, FALSE);
    for (guint i = 0; i < state->views->len; i++) {
        MuxSessionView *view = g_ptr_array_index(state->views, i);

        if (view->id == id) {
            g_ptr_array_remove_index(state->views, i);
            return TRUE;
        }
    }
    return FALSE;
}

guint
mux_session_state_get_view_count(const MuxSessionState *state)
{
    g_return_val_if_fail(state != NULL, 0);
    return state->views->len;
}

const MuxSessionView *
mux_session_state_get_view(const MuxSessionState *state, guint index)
{
    g_return_val_if_fail(state != NULL, NULL);
    if (index >= state->views->len)
        return NULL;
    return g_ptr_array_index(state->views, index);
}

gchar *
mux_session_state_serialize(const MuxSessionState *state,
                            gsize *length,
                            GError **error)
{
    g_autoptr(GKeyFile) key_file = NULL;
    guint64 greatest_view_id = 0;

    g_return_val_if_fail(state != NULL, NULL);
    if (state->next_view_id == 0 || !valid_layer(state->active_layer) ||
        !state_has_layer(state, state->active_layer) ||
        state->layers->len == 0 || state->layers->len > MAX_LAYERS ||
        state->views->len > MAX_VIEWS) {
        set_invalid_data(error, "session state is internally invalid");
        return NULL;
    }

    key_file = g_key_file_new();
    g_key_file_set_integer(key_file, "session", "version", SESSION_VERSION);
    g_key_file_set_uint64(key_file,
                          "session",
                          "next-view-id",
                          state->next_view_id);
    g_key_file_set_string(key_file,
                          "session",
                          "active-layer",
                          state->active_layer);
    g_key_file_set_integer(key_file,
                           "session",
                           "layer-count",
                           (gint)state->layers->len);
    g_key_file_set_integer(key_file,
                           "session",
                           "view-count",
                           (gint)state->views->len);

    for (guint i = 0; i < state->layers->len; i++) {
        const gchar *layer = g_ptr_array_index(state->layers, i);
        g_autofree gchar *group = g_strdup_printf("layer %u", i);

        if (!valid_layer(layer)) {
            set_invalid_data(error, "session contains an invalid layer");
            return NULL;
        }
        g_key_file_set_string(key_file, group, "name", layer);
    }

    for (guint i = 0; i < state->views->len; i++) {
        const MuxSessionView *view = g_ptr_array_index(state->views, i);
        g_autofree gchar *group = g_strdup_printf("view %u", i);

        if (view->id == 0 || !state_has_layer(state, view->layer) ||
            !valid_text(view->uri, MAX_URI_BYTES) ||
            !valid_text(view->title, MAX_TITLE_BYTES)) {
            set_invalid_data(error, "session contains an invalid view");
            return NULL;
        }
        if (state_find_view(state, view->id) != view) {
            set_invalid_data(error, "session contains duplicate view IDs");
            return NULL;
        }
        greatest_view_id = MAX(greatest_view_id, view->id);
        g_key_file_set_uint64(key_file, group, "id", view->id);
        g_key_file_set_string(key_file, group, "layer", view->layer);
        g_key_file_set_string(key_file, group, "uri", view->uri);
        g_key_file_set_string(key_file, group, "title", view->title);
    }

    if (state->next_view_id <= greatest_view_id) {
        set_invalid_data(error,
                         "session next view ID does not exceed stored IDs");
        return NULL;
    }
    return g_key_file_to_data(key_file, length, error);
}

MuxSessionState *
mux_session_state_deserialize(const gchar *data,
                              gsize length,
                              GError **error)
{
    g_autoptr(GKeyFile) key_file = g_key_file_new();
    g_autoptr(MuxSessionState) state = NULL;
    g_autoptr(GError) local_error = NULL;
    g_autofree gchar *active_layer = NULL;
    gint version;
    gint layer_count;
    gint view_count;
    guint64 greatest_view_id = 0;

    if (data == NULL || length == 0 || length > MAX_STATE_BYTES) {
        set_invalid_data(error, "session state size is invalid");
        return NULL;
    }
    if (!g_key_file_load_from_data(key_file,
                                   data,
                                   length,
                                   G_KEY_FILE_NONE,
                                   error))
        return NULL;

    version = g_key_file_get_integer(key_file,
                                     "session",
                                     "version",
                                     &local_error);
    if (local_error != NULL || version != SESSION_VERSION) {
        g_clear_error(&local_error);
        set_invalid_data(error, "unsupported session state version");
        return NULL;
    }
    layer_count = g_key_file_get_integer(key_file,
                                         "session",
                                         "layer-count",
                                         &local_error);
    if (local_error != NULL || layer_count <= 0 ||
        layer_count > (gint)MAX_LAYERS) {
        g_clear_error(&local_error);
        set_invalid_data(error, "session layer count is invalid");
        return NULL;
    }
    view_count = g_key_file_get_integer(key_file,
                                        "session",
                                        "view-count",
                                        &local_error);
    if (local_error != NULL || view_count < 0 ||
        view_count > (gint)MAX_VIEWS) {
        g_clear_error(&local_error);
        set_invalid_data(error, "session view count is invalid");
        return NULL;
    }

    state = session_state_alloc();
    state->next_view_id = g_key_file_get_uint64(key_file,
                                                "session",
                                                "next-view-id",
                                                &local_error);
    if (local_error != NULL || state->next_view_id == 0) {
        g_clear_error(&local_error);
        set_invalid_data(error, "session next view ID is invalid");
        return NULL;
    }
    active_layer = g_key_file_get_string(key_file,
                                         "session",
                                         "active-layer",
                                         &local_error);
    if (local_error != NULL || !valid_layer(active_layer)) {
        g_clear_error(&local_error);
        set_invalid_data(error, "session active layer is invalid");
        return NULL;
    }

    for (gint i = 0; i < layer_count; i++) {
        g_autofree gchar *group = g_strdup_printf("layer %d", i);
        g_autofree gchar *layer = g_key_file_get_string(key_file,
                                                        group,
                                                        "name",
                                                        &local_error);

        if (local_error != NULL || !valid_layer(layer) ||
            state_has_layer(state, layer)) {
            g_clear_error(&local_error);
            set_invalid_data(error, "session layer entry is invalid");
            return NULL;
        }
        g_ptr_array_add(state->layers, g_steal_pointer(&layer));
    }
    if (!state_has_layer(state, active_layer)) {
        set_invalid_data(error, "session active layer is not declared");
        return NULL;
    }
    state->active_layer = g_steal_pointer(&active_layer);

    for (gint i = 0; i < view_count; i++) {
        g_autofree gchar *group = g_strdup_printf("view %d", i);
        g_autofree gchar *layer = NULL;
        g_autofree gchar *uri = NULL;
        g_autofree gchar *title = NULL;
        guint64 id = g_key_file_get_uint64(key_file,
                                           group,
                                           "id",
                                           &local_error);

        if (local_error == NULL)
            layer = g_key_file_get_string(key_file,
                                          group,
                                          "layer",
                                          &local_error);
        if (local_error == NULL)
            uri = g_key_file_get_string(key_file,
                                        group,
                                        "uri",
                                        &local_error);
        if (local_error == NULL)
            title = g_key_file_get_string(key_file,
                                          group,
                                          "title",
                                          &local_error);
        if (local_error != NULL || id == 0 ||
            !state_has_layer(state, layer) ||
            !valid_text(uri, MAX_URI_BYTES) ||
            !valid_text(title, MAX_TITLE_BYTES) ||
            state_find_view(state, id) != NULL) {
            g_clear_error(&local_error);
            set_invalid_data(error, "session view entry is invalid");
            return NULL;
        }
        if (!mux_session_state_upsert_view(state,
                                           id,
                                           layer,
                                           uri,
                                           title)) {
            set_invalid_data(error, "session view entry cannot be stored");
            return NULL;
        }
        greatest_view_id = MAX(greatest_view_id, id);
    }
    if (state->next_view_id <= greatest_view_id) {
        set_invalid_data(error,
                         "session next view ID does not exceed stored IDs");
        return NULL;
    }
    return g_steal_pointer(&state);
}

static int
ensure_private_directory(const gchar *directory, GError **error)
{
    struct stat status;
    int fd;

    if (g_mkdir_with_parents(directory, 0700) < 0) {
        set_errno_error(error, "cannot create session directory", errno);
        return -1;
    }
    if (lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid()) {
        int saved_errno = errno != 0 ? errno : EPERM;

        set_errno_error(error,
                        "session directory is not private",
                        saved_errno);
        return -1;
    }
    if (chmod(directory, 0700) < 0) {
        set_errno_error(error, "cannot secure session directory", errno);
        return -1;
    }
    fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        set_errno_error(error, "cannot open session directory", errno);
        return -1;
    }
    if (fstat(fd, &status) < 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0777) != 0700) {
        int saved_errno = errno != 0 ? errno : EPERM;

        close(fd);
        set_errno_error(error,
                        "session directory failed validation",
                        saved_errno);
        return -1;
    }
    return fd;
}

MuxSessionState *
mux_session_state_load(const gchar *path, GError **error)
{
    g_autofree gchar *directory = NULL;
    g_autofree gchar *basename = NULL;
    g_autoptr(GByteArray) bytes = NULL;
    struct stat status;
    int directory_fd;
    int fd;

    g_return_val_if_fail(path != NULL && g_path_is_absolute(path), NULL);
    directory = g_path_get_dirname(path);
    basename = g_path_get_basename(path);
    directory_fd = ensure_private_directory(directory, error);
    if (directory_fd < 0)
        return NULL;

    fd = openat(directory_fd,
                basename,
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        close(directory_fd);
        return mux_session_state_new();
    }
    if (fd < 0) {
        int saved_errno = errno;

        close(directory_fd);
        set_errno_error(error, "cannot open session state", saved_errno);
        return NULL;
    }
    close(directory_fd);

    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || status.st_size < 0 ||
        (guint64)status.st_size > MAX_STATE_BYTES) {
        int saved_errno = errno != 0 ? errno : EINVAL;

        close(fd);
        set_errno_error(error, "session state failed validation", saved_errno);
        return NULL;
    }
    if (fchmod(fd, 0600) < 0) {
        int saved_errno = errno;

        close(fd);
        set_errno_error(error, "cannot secure session state", saved_errno);
        return NULL;
    }

    bytes = g_byte_array_sized_new((guint)status.st_size);
    for (;;) {
        guint8 buffer[8192];
        ssize_t count;

        do {
            count = read(fd, buffer, sizeof(buffer));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            int saved_errno = errno;

            close(fd);
            set_errno_error(error, "cannot read session state", saved_errno);
            return NULL;
        }
        if (count == 0)
            break;
        if (bytes->len + (gsize)count > MAX_STATE_BYTES) {
            close(fd);
            set_invalid_data(error, "session state exceeds size limit");
            return NULL;
        }
        g_byte_array_append(bytes, buffer, (guint)count);
    }
    close(fd);
    return mux_session_state_deserialize((const gchar *)bytes->data,
                                         bytes->len,
                                         error);
}

static gboolean
write_all(int fd, const gchar *data, gsize length, GError **error)
{
    gsize offset = 0;

    while (offset < length) {
        ssize_t count;

        do {
            count = write(fd, data + offset, length - offset);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            set_errno_error(error,
                            "cannot write session state",
                            count < 0 ? errno : EIO);
            return FALSE;
        }
        offset += (gsize)count;
    }
    return TRUE;
}

gboolean
mux_session_state_save_atomic(const MuxSessionState *state,
                              const gchar *path,
                              GError **error)
{
    g_autofree gchar *data = NULL;
    g_autofree gchar *directory = NULL;
    g_autofree gchar *basename = NULL;
    g_autofree gchar *temporary = NULL;
    gsize length = 0;
    int directory_fd = -1;
    int fd = -1;
    gboolean renamed = FALSE;
    gboolean result = FALSE;

    g_return_val_if_fail(path != NULL && g_path_is_absolute(path), FALSE);
    data = mux_session_state_serialize(state, &length, error);
    if (data == NULL)
        return FALSE;
    if (length > MAX_STATE_BYTES)
        return set_invalid_data(error, "serialized session state is too large");

    directory = g_path_get_dirname(path);
    basename = g_path_get_basename(path);
    directory_fd = ensure_private_directory(directory, error);
    if (directory_fd < 0)
        return FALSE;

    for (guint attempt = 0; attempt < 32; attempt++) {
        g_free(temporary);
        temporary = g_strdup_printf(".%s.tmp.%ld.%08x",
                                    basename,
                                    (long)getpid(),
                                    g_random_int());
        fd = openat(directory_fd,
                    temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0) {
        set_errno_error(error, "cannot create temporary session state", errno);
        goto out;
    }
    if (fchmod(fd, 0600) < 0) {
        set_errno_error(error, "cannot secure temporary session state", errno);
        goto out;
    }
    if (!write_all(fd, data, length, error))
        goto out;
    if (fsync(fd) < 0) {
        set_errno_error(error, "cannot synchronize session state", errno);
        goto out;
    }
    if (close(fd) < 0) {
        fd = -1;
        set_errno_error(error, "cannot close session state", errno);
        goto out;
    }
    fd = -1;
    if (renameat(directory_fd,
                 temporary,
                 directory_fd,
                 basename) < 0) {
        set_errno_error(error, "cannot replace session state", errno);
        goto out;
    }
    renamed = TRUE;
    if (fsync(directory_fd) < 0) {
        set_errno_error(error, "cannot synchronize session directory", errno);
        goto out;
    }
    result = TRUE;

out:
    if (fd >= 0)
        close(fd);
    if (!renamed && temporary != NULL)
        unlinkat(directory_fd, temporary, 0);
    close(directory_fd);
    return result;
}
