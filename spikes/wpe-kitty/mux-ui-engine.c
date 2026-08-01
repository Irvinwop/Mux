#include "mux-ui-engine.h"

#include <string.h>

typedef struct {
    guint64 request_id;
    MuxUiRequestKind kind;
    WebKitScriptDialog *dialog;
} PendingDialog;

typedef struct {
    guint64 request_id;
    WebKitPermissionRequest *request;
    MuxPermissionStore *store;
    gchar *origin;
    gchar *category;
    gboolean persistence_available;
} PendingPermission;

struct _MuxUiEngineBridge {
    WebKitWebView *web_view;
    GHashTable *pending;
    GHashTable *permissions;
    MuxPermissionStore *permission_store;
    MuxUiEngineSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong script_dialog_handler;
    gulong permission_request_handler;
    gulong query_permission_state_handler;
    gulong load_changed_handler;
};

static void
pending_dialog_free(PendingDialog *pending)
{
    if (!pending)
        return;
    if (pending->dialog)
        webkit_script_dialog_unref(pending->dialog);
    g_free(pending);
}

static void
pending_permission_free(PendingPermission *pending)
{
    if (!pending)
        return;
    g_clear_object(&pending->request);
    mux_permission_store_free(pending->store);
    g_free(pending->origin);
    g_free(pending->category);
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
next_request_id(MuxUiEngineBridge *bridge)
{
    guint64 request_id;

    do {
        request_id = ((guint64)g_random_int() << 32) | g_random_int();
    } while (!request_id ||
             g_hash_table_contains(bridge->pending, &request_id) ||
             g_hash_table_contains(bridge->permissions, &request_id));
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

static gboolean
is_default_port(const gchar *scheme, gint port)
{
    return (g_strcmp0(scheme, "http") == 0 && port == 80) ||
           (g_strcmp0(scheme, "https") == 0 && port == 443) ||
           (g_strcmp0(scheme, "ws") == 0 && port == 80) ||
           (g_strcmp0(scheme, "wss") == 0 && port == 443);
}

static gchar *
security_origin_for_view(WebKitWebView *web_view)
{
    const gchar *uri_string = webkit_web_view_get_uri(web_view);
    g_autoptr(GError) error = NULL;
    g_autoptr(GUri) uri = NULL;
    const gchar *scheme;
    const gchar *host;
    gint port;
    g_autofree gchar *authority = NULL;
    g_autofree gchar *origin = NULL;

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

    if (strchr(host, ':'))
        authority = g_strdup_printf("[%s]", host);
    else
        authority = g_strdup(host);
    if (port >= 0 && !is_default_port(scheme, port))
        origin = g_strdup_printf("%s://%s:%d", scheme, authority, port);
    else
        origin = g_strdup_printf("%s://%s", scheme, authority);
    return bounded_utf8(origin, 2048);
}

static MuxUiRequestKind
request_kind_for_dialog(WebKitScriptDialogType type)
{
    switch (type) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        return MUX_UI_REQUEST_DIALOG_ALERT;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
        return MUX_UI_REQUEST_DIALOG_CONFIRM;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        return MUX_UI_REQUEST_DIALOG_PROMPT;
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        return MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD;
    default:
        return 0;
    }
}

static const gchar *
heading_for_kind(MuxUiRequestKind kind)
{
    switch (kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return "Page alert";
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        return "Page confirmation";
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        return "Page prompt";
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return "Leave this page?";
    default:
        return "Browser request";
    }
}

static MuxUiAction
safe_action_for_kind(MuxUiRequestKind kind)
{
    switch (kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return MUX_UI_ACTION_ACKNOWLEDGE;
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return MUX_UI_ACTION_STAY;
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        return MUX_UI_ACTION_CANCEL;
    default:
        return MUX_UI_ACTION_UNSUPPORTED;
    }
}

static void
resolve_dialog(PendingDialog *pending,
               MuxUiAction action,
               const gchar *value)
{
    if (!pending || !pending->dialog)
        return;

    switch (pending->kind) {
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed(
            pending->dialog, action == MUX_UI_ACTION_ACCEPT);
        break;
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        if (action == MUX_UI_ACTION_SUBMIT)
            webkit_script_dialog_prompt_set_text(
                pending->dialog, value ? value : "");
        break;
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        webkit_script_dialog_confirm_set_confirmed(
            pending->dialog, action == MUX_UI_ACTION_LEAVE);
        break;
    case MUX_UI_REQUEST_DIALOG_ALERT:
    default:
        break;
    }
    webkit_script_dialog_close(pending->dialog);
}

static gpointer
take_hash_value(GHashTable *table, guint64 request_id)
{
    gpointer stored_key = NULL;
    gpointer stored_value = NULL;

    if (!g_hash_table_lookup_extended(table,
                                      &request_id,
                                      &stored_key,
                                      &stored_value))
        return NULL;
    g_hash_table_steal(table, &request_id);
    g_free(stored_key);
    return stored_value;
}

static PendingDialog *
take_pending_dialog(MuxUiEngineBridge *bridge, guint64 request_id)
{
    return take_hash_value(bridge->pending, request_id);
}

static PendingPermission *
take_pending_permission(MuxUiEngineBridge *bridge, guint64 request_id)
{
    return take_hash_value(bridge->permissions, request_id);
}

static gboolean
send_payload(MuxUiEngineBridge *bridge, GBytes *payload, GError **error)
{
    if (!bridge->send_func)
        return FALSE;
    return bridge->send_func(payload, bridge->user_data, error);
}

static void
send_cancel(MuxUiEngineBridge *bridge,
            guint64 request_id,
            MuxUiCancelReason reason)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) payload =
        mux_ui_cancel_encode(request_id, reason, &error);

    if (payload && !send_payload(bridge, payload, &error))
        g_clear_error(&error);
}

static gboolean
on_script_dialog(WebKitWebView *web_view,
                 WebKitScriptDialog *dialog,
                 MuxUiEngineBridge *bridge)
{
    MuxUiRequestKind kind = request_kind_for_dialog(
        webkit_script_dialog_get_dialog_type(dialog));
    g_autoptr(MuxUiRequest) request = NULL;
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GError) error = NULL;
    PendingDialog *pending;

    if (!kind) {
        webkit_script_dialog_close(dialog);
        return TRUE;
    }

    request = mux_ui_request_new(kind);
    request->request_id = next_request_id(bridge);
    request->deadline_ms = 120000;
    request->origin = security_origin_for_view(web_view);
    request->heading = g_strdup(heading_for_kind(kind));
    request->message = bounded_utf8(
        webkit_script_dialog_get_message(dialog), MUX_UI_MAX_MESSAGE);
    if (kind == MUX_UI_REQUEST_DIALOG_PROMPT)
        request->default_value = bounded_utf8(
            webkit_script_dialog_prompt_get_default_text(dialog),
            MUX_UI_MAX_VALUE);

    payload = mux_ui_request_encode(request, &error);
    if (!payload) {
        webkit_script_dialog_close(dialog);
        return TRUE;
    }

    pending = g_new0(PendingDialog, 1);
    pending->request_id = request->request_id;
    pending->kind = kind;
    pending->dialog = webkit_script_dialog_ref(dialog);
    g_hash_table_insert(bridge->pending,
                        request_key_new(pending->request_id),
                        pending);

    if (!send_payload(bridge, payload, &error)) {
        PendingDialog *failed =
            take_pending_dialog(bridge, request->request_id);

        if (failed) {
            resolve_dialog(failed, safe_action_for_kind(failed->kind), NULL);
            pending_dialog_free(failed);
        }
    }
    return TRUE;
}

static const gchar *
permission_category(WebKitPermissionRequest *request)
{
    const gchar *type_name = G_OBJECT_TYPE_NAME(request);

    if (g_strcmp0(type_name, "WebKitGeolocationPermissionRequest") == 0)
        return "geolocation";
    if (g_strcmp0(type_name, "WebKitNotificationPermissionRequest") == 0)
        return "notifications";
    if (g_strcmp0(type_name, "WebKitUserMediaPermissionRequest") == 0) {
        WebKitUserMediaPermissionRequest *media =
            (WebKitUserMediaPermissionRequest *)request;
        gboolean audio =
            webkit_user_media_permission_is_for_audio_device(media);
        gboolean video =
            webkit_user_media_permission_is_for_video_device(media);
        gboolean display =
            webkit_user_media_permission_is_for_display_device(media);

        if (display)
            return "display-capture";
        if (audio && video)
            return "camera-microphone";
        if (video)
            return "camera";
        if (audio)
            return "microphone";
        return "media-capture";
    }
    if (g_strcmp0(type_name, "WebKitDeviceInfoPermissionRequest") == 0)
        return "media-device-information";
    if (g_strcmp0(type_name,
                  "WebKitMediaKeySystemPermissionRequest") == 0)
        return "encrypted-media";
    if (g_strcmp0(type_name,
                  "WebKitWebsiteDataAccessPermissionRequest") == 0)
        return "third-party-storage";
    if (g_strcmp0(type_name, "WebKitXRPermissionRequest") == 0)
        return "extended-reality";
    return type_name;
}

static gchar *
permission_message(WebKitPermissionRequest *request)
{
    const gchar *category = permission_category(request);

    if (g_strcmp0(category, "geolocation") == 0)
        return g_strdup("Allow this site to access your location?");
    if (g_strcmp0(category, "notifications") == 0)
        return g_strdup("Allow this site to show desktop notifications?");
    if (g_strcmp0(category, "camera-microphone") == 0)
        return g_strdup(
            "Allow this site to use your camera and microphone?");
    if (g_strcmp0(category, "camera") == 0)
        return g_strdup("Allow this site to use your camera?");
    if (g_strcmp0(category, "microphone") == 0)
        return g_strdup("Allow this site to use your microphone?");
    if (g_strcmp0(category, "display-capture") == 0)
        return g_strdup("Allow this site to capture your display?");
    if (g_strcmp0(category, "media-capture") == 0)
        return g_strdup("Allow this site to access a media device?");
    if (g_strcmp0(category, "media-device-information") == 0)
        return g_strdup(
            "Allow this site to enumerate cameras and microphones?");
    if (g_strcmp0(category, "encrypted-media") == 0)
        return g_strdup(
            "Allow this site to use an encrypted-media key system?");
    if (g_strcmp0(category, "third-party-storage") == 0)
        return g_strdup(
            "Allow this site to access third-party website data?");
    if (g_strcmp0(category, "extended-reality") == 0)
        return g_strdup("Allow this site to access an XR device?");
    return g_strdup("Allow this site to use a protected browser capability?");
}

static gboolean
permission_is_dangerous(WebKitPermissionRequest *request)
{
    const gchar *category = permission_category(request);

    return g_strcmp0(category, "camera-microphone") == 0 ||
           g_strcmp0(category, "camera") == 0 ||
           g_strcmp0(category, "microphone") == 0 ||
           g_strcmp0(category, "display-capture") == 0 ||
           g_strcmp0(category, "media-capture") == 0 ||
           g_strcmp0(category, "media-device-information") == 0 ||
           g_strcmp0(category, "encrypted-media") == 0 ||
           g_strcmp0(category, "extended-reality") == 0 ||
           g_str_has_prefix(category, "WebKit");
}

static void
resolve_permission(PendingPermission *pending, MuxUiAction action)
{
    gboolean allow;

    if (!pending || !pending->request)
        return;
    if (pending->persistence_available && pending->store &&
        (action == MUX_UI_ACTION_ALLOW_ALWAYS ||
         action == MUX_UI_ACTION_DENY_ALWAYS)) {
        g_autoptr(GError) error = NULL;
        MuxPermissionDecision decision =
            action == MUX_UI_ACTION_ALLOW_ALWAYS
                ? MUX_PERMISSION_DECISION_ALLOW
                : MUX_PERMISSION_DECISION_DENY;

        if (!mux_permission_store_set(pending->store,
                                      pending->origin,
                                      pending->category,
                                      decision,
                                      &error))
            g_warning("could not persist permission decision: %s",
                      error->message);
    }
    allow = action == MUX_UI_ACTION_ALLOW_ONCE ||
            (pending->persistence_available &&
             action == MUX_UI_ACTION_ALLOW_ALWAYS);
    if (allow)
        webkit_permission_request_allow(pending->request);
    else
        webkit_permission_request_deny(pending->request);
}

static gboolean
on_permission_request(WebKitWebView *web_view,
                      WebKitPermissionRequest *permission,
                      MuxUiEngineBridge *bridge)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_PERMISSION);
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GError) error = NULL;
    PendingPermission *pending;
    MuxPermissionDecision remembered;
    const gchar *category = permission_category(permission);

    request->request_id = next_request_id(bridge);
    request->deadline_ms = 30000;
    request->origin = security_origin_for_view(web_view);
    remembered = bridge->permission_store
                     ? mux_permission_store_lookup(
                           bridge->permission_store,
                           request->origin,
                           category)
                     : MUX_PERMISSION_DECISION_ASK;
    if (remembered == MUX_PERMISSION_DECISION_ALLOW) {
        webkit_permission_request_allow(permission);
        return TRUE;
    }
    if (remembered == MUX_PERMISSION_DECISION_DENY) {
        webkit_permission_request_deny(permission);
        return TRUE;
    }

    request->heading = g_strdup("Permission request");
    request->message = permission_message(permission);
    request->default_value = g_strdup(category);
    if (permission_is_dangerous(permission))
        request->flags |= MUX_UI_REQUEST_FLAG_DANGER;
    if (bridge->permission_store &&
        mux_permission_store_is_persistent(
            bridge->permission_store))
        request->flags |=
            MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE;

    payload = mux_ui_request_encode(request, &error);
    if (!payload) {
        webkit_permission_request_deny(permission);
        return TRUE;
    }

    pending = g_new0(PendingPermission, 1);
    pending->request_id = request->request_id;
    pending->request = g_object_ref(permission);
    pending->store = bridge->permission_store
                         ? mux_permission_store_ref(
                               bridge->permission_store)
                         : NULL;
    pending->origin = g_strdup(request->origin);
    pending->category = g_strdup(category);
    pending->persistence_available =
        !!(request->flags &
           MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE);
    g_hash_table_insert(bridge->permissions,
                        request_key_new(pending->request_id),
                        pending);
    if (!send_payload(bridge, payload, &error)) {
        PendingPermission *failed =
            take_pending_permission(bridge, request->request_id);

        if (failed) {
            resolve_permission(failed, MUX_UI_ACTION_DENY_ONCE);
            pending_permission_free(failed);
        }
    }
    return TRUE;
}

static const gchar *
category_for_permission_name(const gchar *name)
{
    if (g_strcmp0(name, "camera") == 0)
        return "camera";
    if (g_strcmp0(name, "microphone") == 0)
        return "microphone";
    if (g_strcmp0(name, "display-capture") == 0)
        return "display-capture";
    if (g_strcmp0(name, "geolocation") == 0)
        return "geolocation";
    if (g_strcmp0(name, "notifications") == 0)
        return "notifications";
    if (g_strcmp0(name, "storage-access") == 0)
        return "third-party-storage";
    if (g_strcmp0(name, "xr-spatial-tracking") == 0)
        return "extended-reality";
    return name;
}

static gboolean
on_query_permission_state(WebKitWebView *web_view,
                          WebKitPermissionStateQuery *query,
                          MuxUiEngineBridge *bridge)
{
    WebKitSecurityOrigin *security_origin =
        webkit_permission_state_query_get_security_origin(query);
    g_autofree gchar *origin =
        security_origin
            ? webkit_security_origin_to_string(security_origin)
            : NULL;
    const gchar *name =
        webkit_permission_state_query_get_name(query);
    const gchar *category = category_for_permission_name(name);
    MuxPermissionDecision decision =
        bridge->permission_store && origin && category
            ? mux_permission_store_lookup(bridge->permission_store,
                                          origin,
                                          category)
            : MUX_PERMISSION_DECISION_ASK;
    WebKitPermissionState state;

    (void)web_view;
    switch (decision) {
    case MUX_PERMISSION_DECISION_ALLOW:
        state = WEBKIT_PERMISSION_STATE_GRANTED;
        break;
    case MUX_PERMISSION_DECISION_DENY:
        state = WEBKIT_PERMISSION_STATE_DENIED;
        break;
    case MUX_PERMISSION_DECISION_ASK:
    default:
        state = WEBKIT_PERMISSION_STATE_PROMPT;
        break;
    }
    webkit_permission_state_query_finish(query, state);
    return TRUE;
}

static void
on_load_changed(WebKitWebView *web_view,
                WebKitLoadEvent event,
                MuxUiEngineBridge *bridge)
{
    (void)web_view;
    if (event == WEBKIT_LOAD_COMMITTED)
        mux_ui_engine_bridge_cancel_all(
            bridge, MUX_UI_CANCEL_NAVIGATION, TRUE);
}

MuxUiEngineBridge *
mux_ui_engine_bridge_new(WebKitWebView *web_view,
                         MuxUiEngineSendFunc send_func,
                         gpointer user_data,
                         GDestroyNotify user_data_destroy)
{
    MuxUiEngineBridge *bridge;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);
    g_return_val_if_fail(send_func, NULL);

    bridge = g_new0(MuxUiEngineBridge, 1);
    bridge->web_view = g_object_ref(web_view);
    bridge->pending = g_hash_table_new_full(g_int64_hash,
                                            g_int64_equal,
                                            g_free,
                                            (GDestroyNotify)
                                                pending_dialog_free);
    bridge->permissions = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)pending_permission_free);
    bridge->send_func = send_func;
    bridge->user_data = user_data;
    bridge->user_data_destroy = user_data_destroy;
    bridge->script_dialog_handler =
        g_signal_connect(web_view,
                         "script-dialog",
                         G_CALLBACK(on_script_dialog),
                         bridge);
    bridge->permission_request_handler =
        g_signal_connect(web_view,
                         "permission-request",
                         G_CALLBACK(on_permission_request),
                         bridge);
    bridge->query_permission_state_handler =
        g_signal_connect(web_view,
                         "query-permission-state",
                         G_CALLBACK(on_query_permission_state),
                         bridge);
    bridge->load_changed_handler =
        g_signal_connect(web_view,
                         "load-changed",
                         G_CALLBACK(on_load_changed),
                         bridge);
    return bridge;
}

void
mux_ui_engine_bridge_set_permission_store(
    MuxUiEngineBridge *bridge,
    MuxPermissionStore *store)
{
    g_return_if_fail(bridge);
    if (bridge->permission_store == store)
        return;
    if (store)
        mux_permission_store_ref(store);
    mux_permission_store_free(bridge->permission_store);
    bridge->permission_store = store;
}

void
mux_ui_engine_bridge_free(MuxUiEngineBridge *bridge)
{
    if (!bridge)
        return;
    if (bridge->script_dialog_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->script_dialog_handler);
    if (bridge->permission_request_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->permission_request_handler);
    if (bridge->query_permission_state_handler)
        g_signal_handler_disconnect(
            bridge->web_view,
            bridge->query_permission_state_handler);
    if (bridge->load_changed_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->load_changed_handler);
    mux_ui_engine_bridge_cancel_all(
        bridge, MUX_UI_CANCEL_VIEW_DESTROYED, TRUE);
    g_clear_pointer(&bridge->pending, g_hash_table_unref);
    g_clear_pointer(&bridge->permissions, g_hash_table_unref);
    mux_permission_store_free(bridge->permission_store);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_clear_object(&bridge->web_view);
    g_free(bridge);
}

gboolean
mux_ui_engine_bridge_handle_payload(MuxUiEngineBridge *bridge,
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
        PendingDialog *pending;

        if (!mux_ui_response_decode(data, length, &response, error))
            return FALSE;
        pending = take_pending_dialog(bridge, response->request_id);
        if (pending) {
            if (!mux_ui_action_is_valid(pending->kind, response->action)) {
                resolve_dialog(
                    pending, safe_action_for_kind(pending->kind), NULL);
                pending_dialog_free(pending);
                g_set_error_literal(
                    error,
                    MUX_UI_ERROR,
                    MUX_UI_ERROR_INVALID,
                    "response action does not match request kind");
                return FALSE;
            }
            resolve_dialog(pending, response->action, response->value);
            pending_dialog_free(pending);
            return TRUE;
        }

        {
            PendingPermission *permission =
                take_pending_permission(bridge, response->request_id);

            if (!permission)
                return TRUE;
            if (!mux_ui_action_is_valid(MUX_UI_REQUEST_PERMISSION,
                                        response->action) ||
                (!permission->persistence_available &&
                 (response->action == MUX_UI_ACTION_ALLOW_ALWAYS ||
                  response->action == MUX_UI_ACTION_DENY_ALWAYS))) {
                resolve_permission(permission, MUX_UI_ACTION_DENY_ONCE);
                pending_permission_free(permission);
                g_set_error_literal(
                    error,
                    MUX_UI_ERROR,
                    MUX_UI_ERROR_INVALID,
                    "invalid permission response action");
                return FALSE;
            }
            resolve_permission(permission, response->action);
            pending_permission_free(permission);
        }
        return TRUE;
    }

    if (type == MUX_UI_RECORD_CANCEL) {
        guint64 request_id;
        MuxUiCancelReason reason;
        PendingDialog *pending;

        if (!mux_ui_cancel_decode(
                data, length, &request_id, &reason, error))
            return FALSE;
        pending = take_pending_dialog(bridge, request_id);
        if (pending) {
            resolve_dialog(pending,
                           safe_action_for_kind(pending->kind),
                           NULL);
            pending_dialog_free(pending);
            return TRUE;
        }
        {
            PendingPermission *permission =
                take_pending_permission(bridge, request_id);

            if (permission) {
                resolve_permission(permission, MUX_UI_ACTION_DENY_ONCE);
                pending_permission_free(permission);
            }
        }
        return TRUE;
    }

    g_set_error_literal(error,
                        MUX_UI_ERROR,
                        MUX_UI_ERROR_INVALID,
                        "engine received a UI request from a pane");
    return FALSE;
}

void
mux_ui_engine_bridge_cancel(MuxUiEngineBridge *bridge,
                            guint64 request_id,
                            MuxUiCancelReason reason,
                            gboolean notify_pane)
{
    PendingDialog *pending;

    g_return_if_fail(bridge);
    pending = take_pending_dialog(bridge, request_id);
    if (pending) {
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        resolve_dialog(pending, safe_action_for_kind(pending->kind), NULL);
        pending_dialog_free(pending);
        return;
    }
    {
        PendingPermission *permission =
            take_pending_permission(bridge, request_id);

        if (!permission)
            return;
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        resolve_permission(permission, MUX_UI_ACTION_DENY_ONCE);
        pending_permission_free(permission);
    }
}

void
mux_ui_engine_bridge_cancel_all(MuxUiEngineBridge *bridge,
                                MuxUiCancelReason reason,
                                gboolean notify_pane)
{
    g_return_if_fail(bridge);

    while (g_hash_table_size(bridge->pending)) {
        GHashTableIter iterator;
        gpointer key;
        gpointer value;
        guint64 request_id;
        PendingDialog *pending;

        g_hash_table_iter_init(&iterator, bridge->pending);
        if (!g_hash_table_iter_next(&iterator, &key, &value))
            break;
        request_id = *(guint64 *)key;
        pending = take_pending_dialog(bridge, request_id);
        if (!pending)
            continue;
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        resolve_dialog(pending, safe_action_for_kind(pending->kind), NULL);
        pending_dialog_free(pending);
    }
    while (g_hash_table_size(bridge->permissions)) {
        GHashTableIter iterator;
        gpointer key;
        gpointer value;
        guint64 request_id;
        PendingPermission *permission;

        g_hash_table_iter_init(&iterator, bridge->permissions);
        if (!g_hash_table_iter_next(&iterator, &key, &value))
            break;
        request_id = *(guint64 *)key;
        permission = take_pending_permission(bridge, request_id);
        if (!permission)
            continue;
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        resolve_permission(permission, MUX_UI_ACTION_DENY_ONCE);
        pending_permission_free(permission);
    }
}

guint
mux_ui_engine_bridge_pending_count(const MuxUiEngineBridge *bridge)
{
    g_return_val_if_fail(bridge, 0);
    return g_hash_table_size(bridge->pending) +
           g_hash_table_size(bridge->permissions);
}
