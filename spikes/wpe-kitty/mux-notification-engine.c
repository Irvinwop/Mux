#include "mux-notification-engine.h"

#include <string.h>

#define MUX_NOTIFICATION_MAX_PENDING 64U

typedef struct {
    guint64 request_id;
    WebKitNotification *notification;
    gulong closed_handler;
    gboolean clicked_reported;
} PendingNotification;

struct _MuxNotificationEngine {
    gatomicrefcount references;
    WebKitWebView *web_view;
    GHashTable *pending;
    MuxNotificationEngineSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gboolean private_profile;
    gboolean destroying;
    gulong show_handler;
};

static guint64 *
request_key_new(guint64 request_id)
{
    guint64 *key = g_new(guint64, 1);

    *key = request_id;
    return key;
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

static void
pending_notification_free(PendingNotification *pending)
{
    if (!pending)
        return;
    if (pending->closed_handler &&
        g_signal_handler_is_connected(pending->notification,
                                      pending->closed_handler))
        g_signal_handler_disconnect(pending->notification,
                                    pending->closed_handler);
    g_clear_object(&pending->notification);
    g_free(pending);
}

static MuxNotificationEngine *
notification_engine_ref(MuxNotificationEngine *engine)
{
    g_atomic_ref_count_inc(&engine->references);
    return engine;
}

static void
notification_engine_unref(MuxNotificationEngine *engine)
{
    if (!g_atomic_ref_count_dec(&engine->references))
        return;
    g_clear_pointer(&engine->pending, g_hash_table_unref);
    if (engine->user_data_destroy)
        engine->user_data_destroy(engine->user_data);
    g_clear_object(&engine->web_view);
    g_free(engine);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNotificationEngine,
                              notification_engine_unref)

static PendingNotification *
take_pending(MuxNotificationEngine *engine, guint64 request_id)
{
    gpointer stored_key = NULL;
    gpointer stored_value = NULL;

    if (!g_hash_table_lookup_extended(engine->pending,
                                      &request_id,
                                      &stored_key,
                                      &stored_value))
        return NULL;
    g_hash_table_steal(engine->pending, &request_id);
    g_free(stored_key);
    return stored_value;
}

static guint64
next_request_id(MuxNotificationEngine *engine)
{
    guint64 request_id;

    do {
        request_id = ((guint64)g_random_int() << 32) | g_random_int();
    } while (!request_id ||
             g_hash_table_contains(engine->pending, &request_id));
    return request_id;
}

static gboolean
send_payload(MuxNotificationEngine *engine,
             GBytes *payload,
             GError **error)
{
    if (!engine->send_func) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BROKEN_PIPE,
                            "notification pane is unavailable");
        return FALSE;
    }
    return engine->send_func(payload, engine->user_data, error);
}

static void
send_cancel(MuxNotificationEngine *engine, guint64 request_id)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) payload = mux_ui_cancel_encode(
        request_id, MUX_UI_CANCEL_UNDERLYING_GONE, &error);

    if (payload && !send_payload(engine, payload, &error))
        g_clear_error(&error);
}

static guint64
find_notification(MuxNotificationEngine *engine,
                  WebKitNotification *notification)
{
    GHashTableIter iterator;
    gpointer value;

    g_hash_table_iter_init(&iterator, engine->pending);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingNotification *pending = value;

        if (pending->notification == notification)
            return pending->request_id;
    }
    return 0;
}

static guint64
take_matching_tag(MuxNotificationEngine *engine, const gchar *tag)
{
    GHashTableIter iterator;
    gpointer value;
    guint64 request_id = 0;
    PendingNotification *pending;

    if (!tag || !*tag)
        return 0;
    g_hash_table_iter_init(&iterator, engine->pending);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        pending = value;
        if (g_strcmp0(webkit_notification_get_tag(pending->notification),
                      tag) == 0) {
            request_id = pending->request_id;
            break;
        }
    }
    if (!request_id)
        return 0;

    pending = take_pending(engine, request_id);
    webkit_notification_close(pending->notification);
    pending_notification_free(pending);
    return request_id;
}

static void
on_notification_closed(WebKitNotification *notification,
                       MuxNotificationEngine *engine)
{
    g_autoptr(MuxNotificationEngine) guard =
        notification_engine_ref(engine);
    guint64 request_id;
    PendingNotification *pending;

    engine = guard;
    if (engine->destroying)
        return;
    request_id = find_notification(engine, notification);
    if (!request_id)
        return;
    pending = take_pending(engine, request_id);
    send_cancel(engine, request_id);
    pending_notification_free(pending);
}

static gboolean
on_show_notification(WebKitWebView *web_view,
                     WebKitNotification *notification,
                     MuxNotificationEngine *engine)
{
    g_autoptr(MuxNotificationEngine) guard =
        notification_engine_ref(engine);
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_NOTIFICATION);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) payload = NULL;
    PendingNotification *pending;
    const gchar *tag = webkit_notification_get_tag(notification);
    guint64 reused_id = take_matching_tag(engine, tag);

    engine = guard;
    if (engine->destroying)
        return FALSE;

    if (!reused_id &&
        g_hash_table_size(engine->pending) >=
            MUX_NOTIFICATION_MAX_PENDING)
        return FALSE;

    request->request_id = reused_id ? reused_id
                                    : next_request_id(engine);
    request->flags = engine->private_profile
                         ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                         : 0;
    request->origin = bounded_utf8(webkit_web_view_get_uri(web_view), 2048);
    request->heading = bounded_utf8(
        webkit_notification_get_title(notification), 2048);
    request->message = bounded_utf8(
        webkit_notification_get_body(notification), MUX_UI_MAX_MESSAGE);
    request->default_value = bounded_utf8(tag, MUX_UI_MAX_VALUE);
    if (!request->heading || !*request->heading) {
        g_free(request->heading);
        request->heading = g_strdup(
            request->origin && *request->origin
                ? request->origin
                : "Web notification");
    }

    payload = mux_ui_request_encode(request, &error);
    if (!payload)
        return FALSE;

    pending = g_new0(PendingNotification, 1);
    pending->request_id = request->request_id;
    pending->notification = g_object_ref(notification);
    pending->closed_handler = g_signal_connect(
        notification,
        "closed",
        G_CALLBACK(on_notification_closed),
        engine);
    g_hash_table_insert(engine->pending,
                        request_key_new(pending->request_id),
                        pending);
    if (!send_payload(engine, payload, &error)) {
        if (engine->destroying)
            return FALSE;
        pending = take_pending(engine, request->request_id);
        pending_notification_free(pending);
        return FALSE;
    }
    return TRUE;
}

MuxNotificationEngine *
mux_notification_engine_new(WebKitWebView *web_view,
                            gboolean private_profile,
                            MuxNotificationEngineSendFunc send_func,
                            gpointer user_data,
                            GDestroyNotify user_data_destroy)
{
    MuxNotificationEngine *engine;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);
    g_return_val_if_fail(send_func, NULL);

    engine = g_new0(MuxNotificationEngine, 1);
    g_atomic_ref_count_init(&engine->references);
    engine->web_view = g_object_ref(web_view);
    engine->pending = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)pending_notification_free);
    engine->send_func = send_func;
    engine->user_data = user_data;
    engine->user_data_destroy = user_data_destroy;
    engine->private_profile = private_profile;
    engine->show_handler = g_signal_connect(
        web_view,
        "show-notification",
        G_CALLBACK(on_show_notification),
        engine);
    return engine;
}

void
mux_notification_engine_free(MuxNotificationEngine *engine)
{
    GList *keys;
    GList *link;

    if (!engine || engine->destroying)
        return;
    engine->destroying = TRUE;
    if (engine->show_handler)
        g_signal_handler_disconnect(engine->web_view,
                                    engine->show_handler);

    keys = g_hash_table_get_keys(engine->pending);
    for (link = keys; link; link = link->next) {
        guint64 request_id = *(guint64 *)link->data;

        send_cancel(engine, request_id);
    }
    g_list_free(keys);
    notification_engine_unref(engine);
}

gboolean
mux_notification_engine_handle_payload(MuxNotificationEngine *engine,
                                       const guint8 *data,
                                       gsize length,
                                       GError **error)
{
    g_autoptr(MuxNotificationEngine) guard = NULL;
    MuxUiRecordType type;

    g_return_val_if_fail(engine, FALSE);
    guard = notification_engine_ref(engine);
    if (engine->destroying) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_CLOSED,
                            "notification engine is closing");
        return FALSE;
    }
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;
    if (type != MUX_UI_RECORD_RESPONSE)
        return TRUE;

    g_autoptr(MuxUiResponse) response = NULL;
    PendingNotification *pending;

    if (!mux_ui_response_decode(data, length, &response, error))
        return FALSE;
    pending = g_hash_table_lookup(engine->pending,
                                  &response->request_id);
    if (!pending)
        return TRUE;
    if (!mux_ui_action_is_valid(MUX_UI_REQUEST_NOTIFICATION,
                                response->action)) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "invalid notification response");
        return FALSE;
    }

    if (response->action == MUX_UI_ACTION_ACKNOWLEDGE) {
        WebKitNotification *notification;

        if (pending->clicked_reported)
            return TRUE;
        pending->clicked_reported = TRUE;
        notification = g_object_ref(pending->notification);
        webkit_notification_clicked(notification);
        g_object_unref(notification);
        return TRUE;
    }

    pending = take_pending(engine, response->request_id);
    webkit_notification_close(pending->notification);
    pending_notification_free(pending);
    return TRUE;
}

guint
mux_notification_engine_pending_count(const MuxNotificationEngine *engine)
{
    g_return_val_if_fail(engine, 0);
    return g_hash_table_size(engine->pending);
}
