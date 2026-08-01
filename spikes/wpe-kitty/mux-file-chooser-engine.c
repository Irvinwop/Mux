#define _XOPEN_SOURCE 700

#include "mux-file-chooser-engine.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    guint64 request_id;
    WebKitFileChooserRequest *request;
    gboolean select_multiple;
} PendingChooser;

struct _MuxFileChooserBridge {
    WebKitWebView *web_view;
    GHashTable *pending;
    MuxFileChooserSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong run_file_chooser_handler;
    gulong load_changed_handler;
};

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

static void
on_load_changed(WebKitWebView *web_view,
                WebKitLoadEvent event,
                MuxFileChooserBridge *bridge)
{
    (void)web_view;
    if (event == WEBKIT_LOAD_COMMITTED)
        mux_file_chooser_bridge_cancel_all(
            bridge, MUX_UI_CANCEL_NAVIGATION, TRUE);
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
    bridge->send_func = send_func;
    bridge->user_data = user_data;
    bridge->user_data_destroy = user_data_destroy;
    bridge->run_file_chooser_handler =
        g_signal_connect(web_view,
                         "run-file-chooser",
                         G_CALLBACK(on_run_file_chooser),
                         bridge);
    bridge->load_changed_handler =
        g_signal_connect(web_view,
                         "load-changed",
                         G_CALLBACK(on_load_changed),
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
    if (bridge->load_changed_handler)
        g_signal_handler_disconnect(
            bridge->web_view, bridge->load_changed_handler);
    mux_file_chooser_bridge_cancel_all(
        bridge, MUX_UI_CANCEL_VIEW_DESTROYED, TRUE);
    g_clear_pointer(&bridge->pending, g_hash_table_unref);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_clear_object(&bridge->web_view);
    g_free(bridge);
}

static gboolean
validate_paths(const PendingChooser *pending,
               const GPtrArray *paths,
               gchar ***validated_out,
               GError **error)
{
    g_autoptr(GPtrArray) validated =
        g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint i;

    if (!paths || !paths->len ||
        (!pending->select_multiple && paths->len != 1) ||
        paths->len > MUX_UI_MAX_PATHS) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid number of selected files");
        return FALSE;
    }

    for (i = 0; i < paths->len; i++) {
        const gchar *path = g_ptr_array_index((GPtrArray *)paths, i);
        gchar *resolved;
        struct stat status;

        if (!path || !g_path_is_absolute(path)) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "selected file path must be absolute");
            return FALSE;
        }
        resolved = realpath(path, NULL);
        if (!resolved) {
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(errno),
                        "cannot resolve selected file: %s",
                        g_strerror(errno));
            return FALSE;
        }
        if (!g_utf8_validate(resolved, -1, NULL) ||
            stat(resolved, &status) < 0 ||
            !S_ISREG(status.st_mode) ||
            access(resolved, R_OK) < 0) {
            g_free(resolved);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "selected path is not a readable regular file");
            return FALSE;
        }
        if (g_hash_table_contains(seen, resolved)) {
            g_free(resolved);
            continue;
        }
        g_hash_table_add(seen, g_strdup(resolved));
        g_ptr_array_add(validated, resolved);
    }
    if (!validated->len) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "no distinct files were selected");
        return FALSE;
    }
    g_ptr_array_add(validated, NULL);
    *validated_out = (gchar **)g_ptr_array_free(
        g_steal_pointer(&validated), FALSE);
    return TRUE;
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
            g_auto(GStrv) files = NULL;

            if (!validate_paths(
                    pending, response->paths, &files, error)) {
                webkit_file_chooser_request_cancel(pending->request);
                pending_chooser_free(pending);
                return FALSE;
            }
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
