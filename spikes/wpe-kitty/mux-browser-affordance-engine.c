#include "mux-browser-affordance-engine.h"

#include <string.h>

#define MUX_AFFORDANCE_AUTH_RAW_MAX 6000U
#define MUX_AFFORDANCE_CONTEXT_DEPTH_MAX 8U

typedef struct {
    GAction *action;
    GVariant *target;
} ContextAction;

typedef struct {
    guint64 request_id;
    MuxUiRequestKind kind;
    WebKitAuthenticationRequest *authentication;
    WebKitOptionMenu *option_menu;
    GPtrArray *context_actions;
    GHashTable *command_choice_ids;
    MuxBrowserAffordanceChoiceFunc command_choice_func;
    gpointer command_data;
    GDestroyNotify command_data_destroy;
    gulong underlying_handler;
} PendingAffordance;

struct _MuxBrowserAffordanceBridge {
    WebKitWebView *web_view;
    GHashTable *pending;
    MuxBrowserAffordanceSendFunc send_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gboolean private_profile;
    gboolean destroying;
    gulong authenticate_handler;
    gulong option_menu_handler;
    gulong context_menu_handler;
    gulong process_terminated_handler;
    gulong load_changed_handler;
};

static void send_cancel(MuxBrowserAffordanceBridge *bridge,
                        guint64 request_id,
                        MuxUiCancelReason reason);
static void supersede_kind(MuxBrowserAffordanceBridge *bridge,
                           MuxUiRequestKind kind);

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

static gchar *
view_origin(WebKitWebView *web_view)
{
    const gchar *uri = webkit_web_view_get_uri(web_view);

    return bounded_utf8(uri && *uri ? uri : "browser", 2048);
}

static void
context_action_free(ContextAction *item)
{
    if (!item)
        return;
    g_clear_object(&item->action);
    g_clear_pointer(&item->target, g_variant_unref);
    g_free(item);
}

static void
pending_affordance_free(PendingAffordance *pending)
{
    GObject *underlying = NULL;

    if (!pending)
        return;
    if (pending->authentication)
        underlying = G_OBJECT(pending->authentication);
    else if (pending->option_menu)
        underlying = G_OBJECT(pending->option_menu);
    if (underlying && pending->underlying_handler &&
        g_signal_handler_is_connected(underlying,
                                      pending->underlying_handler))
        g_signal_handler_disconnect(underlying,
                                    pending->underlying_handler);
    g_clear_object(&pending->authentication);
    g_clear_object(&pending->option_menu);
    g_clear_pointer(&pending->context_actions, g_ptr_array_unref);
    g_clear_pointer(&pending->command_choice_ids, g_hash_table_unref);
    if (pending->command_data_destroy)
        pending->command_data_destroy(pending->command_data);
    g_free(pending);
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

static PendingAffordance *
take_pending(MuxBrowserAffordanceBridge *bridge, guint64 request_id)
{
    return take_hash_value(bridge->pending, request_id);
}

static guint64
next_request_id(MuxBrowserAffordanceBridge *bridge)
{
    guint64 request_id;

    do {
        request_id = ((guint64)g_random_int() << 32) | g_random_int();
    } while (!request_id ||
             g_hash_table_contains(bridge->pending, &request_id));
    return request_id;
}

static gboolean
send_payload(MuxBrowserAffordanceBridge *bridge,
             GBytes *payload,
             GError **error)
{
    if (!bridge->send_func) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BROKEN_PIPE,
                            "browser affordance output is unavailable");
        return FALSE;
    }
    return bridge->send_func(payload, bridge->user_data, error);
}

static void
send_cancel(MuxBrowserAffordanceBridge *bridge,
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
publish_request(MuxBrowserAffordanceBridge *bridge,
                PendingAffordance *pending,
                const MuxUiRequest *request,
                GError **error)
{
    g_autoptr(GBytes) payload = mux_ui_request_encode(request, error);

    if (!payload) {
        pending_affordance_free(pending);
        return FALSE;
    }

    g_hash_table_insert(bridge->pending,
                        request_key_new(pending->request_id),
                        pending);
    if (!send_payload(bridge, payload, error)) {
        PendingAffordance *failed =
            take_pending(bridge, pending->request_id);

        pending_affordance_free(failed);
        return FALSE;
    }
    return TRUE;
}

gboolean
mux_browser_affordance_bridge_show_command_surface(
    MuxBrowserAffordanceBridge *bridge,
    const gchar *heading,
    const gchar *message,
    const GPtrArray *choices,
    MuxBrowserAffordanceChoiceFunc choice_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy,
    GError **error)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_CONTEXT_MENU);
    PendingAffordance *pending = g_new0(PendingAffordance, 1);
    GHashTable *seen_ids = g_hash_table_new_full(g_int_hash,
                                                 g_int_equal,
                                                 g_free,
                                                 NULL);
    guint selectable = 0;
    guint i;

    pending->command_data = user_data;
    pending->command_data_destroy = user_data_destroy;
    if (!bridge || !choices || !choice_func ||
        choices->len == 0 || choices->len > MUX_UI_MAX_CHOICES) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "command surface choices are invalid");
        g_hash_table_unref(seen_ids);
        pending_affordance_free(pending);
        return FALSE;
    }

    pending->request_id = next_request_id(bridge);
    pending->kind = MUX_UI_REQUEST_CONTEXT_MENU;
    pending->command_choice_func = choice_func;
    pending->command_choice_ids = g_hash_table_new_full(g_int_hash,
                                                        g_int_equal,
                                                        g_free,
                                                        NULL);
    request->request_id = pending->request_id;
    request->flags = bridge->private_profile
                         ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                         : 0;
    request->deadline_ms = 300000;
    request->origin = view_origin(bridge->web_view);
    request->heading = bounded_utf8(heading, 1024);
    request->message = bounded_utf8(message, MUX_UI_MAX_MESSAGE);

    for (i = 0; i < choices->len; i++) {
        const MuxUiChoice *choice = g_ptr_array_index((GPtrArray *)choices, i);
        guint32 *seen_key;

        if (!choice || !choice->label) {
            g_set_error_literal(error,
                                MUX_UI_ERROR,
                                MUX_UI_ERROR_INVALID,
                                "command surface contains an invalid choice");
            g_hash_table_unref(seen_ids);
            pending_affordance_free(pending);
            return FALSE;
        }
        seen_key = g_new(guint32, 1);
        *seen_key = choice->id;
        if (g_hash_table_contains(seen_ids, seen_key)) {
            g_free(seen_key);
            g_set_error_literal(error,
                                MUX_UI_ERROR,
                                MUX_UI_ERROR_INVALID,
                                "command surface choice ids must be unique");
            g_hash_table_unref(seen_ids);
            pending_affordance_free(pending);
            return FALSE;
        }
        g_hash_table_add(seen_ids, seen_key);
        g_ptr_array_add(request->choices, mux_ui_choice_copy(choice));
        if (!(choice->flags & (MUX_UI_CHOICE_FLAG_DISABLED |
                              MUX_UI_CHOICE_FLAG_SEPARATOR))) {
            guint32 *selectable_key = g_new(guint32, 1);

            *selectable_key = choice->id;
            g_hash_table_add(pending->command_choice_ids, selectable_key);
            selectable++;
        }
    }
    g_hash_table_unref(seen_ids);
    if (!selectable) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "command surface has no selectable choices");
        pending_affordance_free(pending);
        return FALSE;
    }

    supersede_kind(bridge, MUX_UI_REQUEST_CONTEXT_MENU);
    return publish_request(bridge, pending, request, error);
}

static guint64
find_pending_object(MuxBrowserAffordanceBridge *bridge,
                    MuxUiRequestKind kind,
                    gpointer object)
{
    GHashTableIter iterator;
    gpointer value;

    g_hash_table_iter_init(&iterator, bridge->pending);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingAffordance *pending = value;
        gpointer candidate = NULL;

        if (pending->kind != kind)
            continue;
        if (kind == MUX_UI_REQUEST_AUTHENTICATION)
            candidate = pending->authentication;
        else if (kind == MUX_UI_REQUEST_OPTION_MENU)
            candidate = pending->option_menu;
        if (candidate == object)
            return pending->request_id;
    }
    return 0;
}

static void
on_authentication_cancelled(WebKitAuthenticationRequest *request,
                            MuxBrowserAffordanceBridge *bridge)
{
    guint64 request_id;
    PendingAffordance *pending;

    if (bridge->destroying)
        return;
    request_id = find_pending_object(bridge,
                                     MUX_UI_REQUEST_AUTHENTICATION,
                                     request);
    if (!request_id)
        return;
    pending = take_pending(bridge, request_id);
    send_cancel(bridge, request_id, MUX_UI_CANCEL_UNDERLYING_GONE);
    pending_affordance_free(pending);
}

static void
on_option_menu_closed(WebKitOptionMenu *menu,
                      MuxBrowserAffordanceBridge *bridge)
{
    guint64 request_id;
    PendingAffordance *pending;

    if (bridge->destroying)
        return;
    request_id = find_pending_object(bridge,
                                     MUX_UI_REQUEST_OPTION_MENU,
                                     menu);
    if (!request_id)
        return;
    pending = take_pending(bridge, request_id);
    send_cancel(bridge, request_id, MUX_UI_CANCEL_UNDERLYING_GONE);
    pending_affordance_free(pending);
}

static void
resolve_cancel(PendingAffordance *pending)
{
    if (!pending)
        return;
    if (pending->authentication)
        webkit_authentication_request_cancel(pending->authentication);
    else if (pending->option_menu)
        webkit_option_menu_close(pending->option_menu);
}

static void
cancel_matching(MuxBrowserAffordanceBridge *bridge,
                MuxUiRequestKind kind,
                MuxUiCancelReason reason,
                gboolean notify_pane)
{
    GHashTableIter iterator;
    gpointer value;
    GArray *request_ids = g_array_new(FALSE, FALSE, sizeof(guint64));
    guint i;

    g_hash_table_iter_init(&iterator, bridge->pending);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PendingAffordance *pending = value;

        if (!kind || pending->kind == kind) {
            guint64 request_id = pending->request_id;

            g_array_append_val(request_ids, request_id);
        }
    }

    for (i = 0; i < request_ids->len; i++) {
        guint64 request_id = g_array_index(request_ids, guint64, i);
        PendingAffordance *pending = take_pending(bridge, request_id);

        if (!pending)
            continue;
        if (notify_pane)
            send_cancel(bridge, request_id, reason);
        resolve_cancel(pending);
        pending_affordance_free(pending);
    }
    g_array_unref(request_ids);
}

void
mux_browser_affordance_bridge_cancel_all(
    MuxBrowserAffordanceBridge *bridge,
    MuxUiCancelReason reason,
    gboolean notify_pane)
{
    g_return_if_fail(bridge);
    cancel_matching(bridge, 0, reason, notify_pane);
}

static void
supersede_kind(MuxBrowserAffordanceBridge *bridge,
               MuxUiRequestKind kind)
{
    cancel_matching(bridge,
                    kind,
                    MUX_UI_CANCEL_SUPERSEDED,
                    TRUE);
}

static gboolean
on_authenticate(WebKitWebView *web_view,
                WebKitAuthenticationRequest *authentication,
                MuxBrowserAffordanceBridge *bridge)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_AUTHENTICATION);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *host = bounded_utf8(
        webkit_authentication_request_get_host(authentication), 1024);
    g_autofree gchar *realm = bounded_utf8(
        webkit_authentication_request_get_realm(authentication), 1024);
    WebKitCredential *proposed =
        webkit_authentication_request_get_proposed_credential(authentication);
    PendingAffordance *pending;
    guint port = webkit_authentication_request_get_port(authentication);

    request->request_id = next_request_id(bridge);
    request->flags = MUX_UI_REQUEST_FLAG_PASSWORD |
                     (bridge->private_profile
                          ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                          : 0);
    request->deadline_ms = 120000;
    request->origin = view_origin(web_view);
    request->heading = g_strdup(
        webkit_authentication_request_is_for_proxy(authentication)
            ? "Proxy authentication"
            : "HTTP authentication");
    request->message = g_strdup_printf(
        "%s%s:%u%s%s%s%s",
        webkit_authentication_request_is_retry(authentication)
            ? "Previous credentials were rejected. "
            : "",
        host && *host ? host : "Unknown host",
        port,
        realm && *realm ? " (realm: " : "",
        realm && *realm ? realm : "",
        realm && *realm ? ")" : "",
        bridge->private_profile
            ? ". Credentials will not be retained."
            : ". Credentials last for this browser session.");
    if (proposed) {
        request->default_value = bounded_utf8(
            webkit_credential_get_username(proposed), 2048);
        webkit_credential_free(proposed);
    }

    if (bridge->private_profile)
        webkit_authentication_request_set_can_save_credentials(
            authentication, FALSE);

    pending = g_new0(PendingAffordance, 1);
    pending->request_id = request->request_id;
    pending->kind = request->kind;
    pending->authentication = g_object_ref(authentication);
    pending->underlying_handler = g_signal_connect(
        authentication,
        "cancelled",
        G_CALLBACK(on_authentication_cancelled),
        bridge);

    if (!publish_request(bridge, pending, request, &error)) {
        webkit_authentication_request_cancel(authentication);
        return TRUE;
    }
    return TRUE;
}

static gboolean
option_menu_has_selectable(const MuxUiRequest *request)
{
    guint i;

    for (i = 0; request->choices && i < request->choices->len; i++) {
        const MuxUiChoice *choice =
            g_ptr_array_index(request->choices, i);

        if (choice &&
            !(choice->flags & (MUX_UI_CHOICE_FLAG_DISABLED |
                               MUX_UI_CHOICE_FLAG_SEPARATOR)))
            return TRUE;
    }
    return FALSE;
}

static gboolean
on_show_option_menu(WebKitWebView *web_view,
                    WebKitOptionMenu *menu,
                    WebKitRectangle *rectangle,
                    MuxBrowserAffordanceBridge *bridge)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_OPTION_MENU);
    g_autoptr(GError) error = NULL;
    PendingAffordance *pending;
    guint count = MIN(webkit_option_menu_get_n_items(menu),
                      MUX_UI_MAX_CHOICES);
    guint i;

    (void)rectangle;
    if (!count)
        return FALSE;

    request->request_id = next_request_id(bridge);
    request->flags = bridge->private_profile
                         ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                         : 0;
    request->deadline_ms = 120000;
    request->origin = view_origin(web_view);
    request->heading = g_strdup("Choose an option");
    request->message = g_strdup("Select a value for this field.");

    for (i = 0; i < count; i++) {
        WebKitOptionMenuItem *item = webkit_option_menu_get_item(menu, i);
        guint32 flags = 0;
        g_autofree gchar *label = NULL;

        if (!item)
            continue;
        if (!webkit_option_menu_item_is_enabled(item) ||
            webkit_option_menu_item_is_group_label(item))
            flags |= MUX_UI_CHOICE_FLAG_DISABLED;
        if (webkit_option_menu_item_is_selected(item))
            flags |= MUX_UI_CHOICE_FLAG_SELECTED;
        label = bounded_utf8(webkit_option_menu_item_get_label(item), 1024);
        if (webkit_option_menu_item_is_group_child(item)) {
            gchar *indented = g_strdup_printf("  %s", label);

            g_free(g_steal_pointer(&label));
            label = indented;
        }
        g_ptr_array_add(request->choices,
                        mux_ui_choice_new(i, flags, label));
    }
    if (!option_menu_has_selectable(request))
        return FALSE;

    supersede_kind(bridge, MUX_UI_REQUEST_OPTION_MENU);
    pending = g_new0(PendingAffordance, 1);
    pending->request_id = request->request_id;
    pending->kind = request->kind;
    pending->option_menu = g_object_ref(menu);
    pending->underlying_handler = g_signal_connect(
        menu,
        "close",
        G_CALLBACK(on_option_menu_closed),
        bridge);
    return publish_request(bridge, pending, request, &error);
}

static void
append_context_menu(MuxUiRequest *request,
                    PendingAffordance *pending,
                    WebKitContextMenu *menu,
                    const gchar *prefix,
                    guint depth)
{
    guint count = webkit_context_menu_get_n_items(menu);
    guint i;

    for (i = 0; i < count &&
                request->choices->len < MUX_UI_MAX_CHOICES; i++) {
        WebKitContextMenuItem *item =
            webkit_context_menu_get_item_at_position(menu, i);
        ContextAction *stored = g_new0(ContextAction, 1);
        WebKitContextMenu *submenu;
        guint32 flags = 0;
        guint32 choice_id = pending->context_actions->len;
        g_autofree gchar *title = NULL;
        g_autofree gchar *label = NULL;

        if (!item) {
            context_action_free(stored);
            continue;
        }
        if (webkit_context_menu_item_is_separator(item)) {
            flags = MUX_UI_CHOICE_FLAG_DISABLED |
                    MUX_UI_CHOICE_FLAG_SEPARATOR;
            g_ptr_array_add(pending->context_actions, stored);
            g_ptr_array_add(request->choices,
                            mux_ui_choice_new(choice_id,
                                              flags,
                                              "----------------"));
            continue;
        }

        title = bounded_utf8(webkit_context_menu_item_get_title(item),
                             1024);
        if (!title || !*title) {
            g_free(g_steal_pointer(&title));
            title = g_strdup("Unnamed action");
        }
        label = prefix && *prefix
                    ? g_strdup_printf("%s > %s", prefix, title)
                    : g_strdup(title);
        submenu = webkit_context_menu_item_get_submenu(item);
        if (submenu) {
            flags |= MUX_UI_CHOICE_FLAG_DISABLED;
            g_ptr_array_add(pending->context_actions, stored);
            g_ptr_array_add(request->choices,
                            mux_ui_choice_new(choice_id, flags, label));
            if (depth < MUX_AFFORDANCE_CONTEXT_DEPTH_MAX)
                append_context_menu(request,
                                    pending,
                                    submenu,
                                    label,
                                    depth + 1);
            continue;
        }

        stored->action = webkit_context_menu_item_get_gaction(item);
        if (stored->action)
            g_object_ref(stored->action);
        stored->target =
            webkit_context_menu_item_get_gaction_target(item);
        if (stored->target)
            g_variant_ref(stored->target);
        if (!stored->action || !g_action_get_enabled(stored->action))
            flags |= MUX_UI_CHOICE_FLAG_DISABLED;
        g_ptr_array_add(pending->context_actions, stored);
        g_ptr_array_add(request->choices,
                        mux_ui_choice_new(choice_id, flags, label));
    }
}

static gboolean
context_has_selectable(const PendingAffordance *pending)
{
    guint i;

    for (i = 0; pending->context_actions &&
                i < pending->context_actions->len; i++) {
        const ContextAction *item =
            g_ptr_array_index(pending->context_actions, i);

        if (item && item->action && g_action_get_enabled(item->action))
            return TRUE;
    }
    return FALSE;
}

static gboolean
on_context_menu(WebKitWebView *web_view,
                WebKitContextMenu *menu,
                WebKitHitTestResult *hit_test,
                MuxBrowserAffordanceBridge *bridge)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_CONTEXT_MENU);
    g_autoptr(GError) error = NULL;
    PendingAffordance *pending = g_new0(PendingAffordance, 1);

    (void)hit_test;
    request->request_id = next_request_id(bridge);
    request->flags = bridge->private_profile
                         ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                         : 0;
    request->deadline_ms = 120000;
    request->origin = view_origin(web_view);
    request->heading = g_strdup("Context menu");
    request->message = g_strdup("Choose an action for this page element.");

    pending->request_id = request->request_id;
    pending->kind = request->kind;
    pending->context_actions = g_ptr_array_new_with_free_func(
        (GDestroyNotify)context_action_free);
    append_context_menu(request, pending, menu, NULL, 0);
    if (!context_has_selectable(pending)) {
        pending_affordance_free(pending);
        return FALSE;
    }

    supersede_kind(bridge, MUX_UI_REQUEST_CONTEXT_MENU);
    return publish_request(bridge, pending, request, &error);
}

static const gchar *
termination_reason_message(WebKitWebProcessTerminationReason reason)
{
    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:
        return "The page renderer crashed. Reloading starts a fresh renderer.";
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
        return "The page renderer exceeded its memory limit and was terminated.";
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
        return "The page renderer was terminated by the browser.";
    default:
        return "The page renderer terminated unexpectedly.";
    }
}

static void
on_web_process_terminated(WebKitWebView *web_view,
                          WebKitWebProcessTerminationReason reason,
                          MuxBrowserAffordanceBridge *bridge)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_CRASH);
    g_autoptr(GError) error = NULL;
    PendingAffordance *pending;

    mux_browser_affordance_bridge_cancel_all(
        bridge,
        MUX_UI_CANCEL_UNDERLYING_GONE,
        TRUE);

    request->request_id = next_request_id(bridge);
    request->flags = MUX_UI_REQUEST_FLAG_DANGER |
                     (bridge->private_profile
                          ? MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE
                          : 0);
    request->deadline_ms = 300000;
    request->origin = view_origin(web_view);
    request->heading = g_strdup("Web process terminated");
    request->message = g_strdup(termination_reason_message(reason));

    pending = g_new0(PendingAffordance, 1);
    pending->request_id = request->request_id;
    pending->kind = request->kind;
    (void)publish_request(bridge, pending, request, &error);
}

static void
on_load_changed(WebKitWebView *web_view,
                WebKitLoadEvent load_event,
                MuxBrowserAffordanceBridge *bridge)
{
    (void)web_view;
    if (load_event == WEBKIT_LOAD_COMMITTED)
        mux_browser_affordance_bridge_cancel_all(
            bridge,
            MUX_UI_CANCEL_NAVIGATION,
            TRUE);
}

static void
wipe_and_free(gchar *value)
{
    volatile gchar *cursor;
    gsize length;

    if (!value)
        return;
    length = strlen(value);
    cursor = (volatile gchar *)value;
    while (length--)
        *cursor++ = '\0';
    g_free(value);
}

static gboolean
decode_authentication_value(const gchar *value,
                            gchar **username_out,
                            gchar **password_out,
                            GError **error)
{
    g_auto(GStrv) parts = g_strsplit(value ? value : "", ":", -1);
    g_autofree guchar *username_bytes = NULL;
    g_autofree guchar *password_bytes = NULL;
    g_autofree gchar *username_canonical = NULL;
    g_autofree gchar *password_canonical = NULL;
    gsize username_length = 0;
    gsize password_length = 0;

    *username_out = NULL;
    *password_out = NULL;
    if (g_strv_length(parts) != 3 || g_strcmp0(parts[0], "v1") != 0) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "authentication response has an invalid envelope");
        return FALSE;
    }

    username_bytes = g_base64_decode(parts[1], &username_length);
    password_bytes = g_base64_decode(parts[2], &password_length);
    username_canonical = g_base64_encode(username_bytes, username_length);
    password_canonical = g_base64_encode(password_bytes, password_length);
    if (g_strcmp0(username_canonical, parts[1]) != 0 ||
        g_strcmp0(password_canonical, parts[2]) != 0 ||
        username_length + password_length > MUX_AFFORDANCE_AUTH_RAW_MAX ||
        (username_length && memchr(username_bytes, '\0', username_length)) ||
        (password_length && memchr(password_bytes, '\0', password_length)) ||
        (username_length &&
         !g_utf8_validate((const gchar *)username_bytes,
                          username_length,
                          NULL)) ||
        (password_length &&
         !g_utf8_validate((const gchar *)password_bytes,
                          password_length,
                          NULL))) {
        g_set_error_literal(error,
                            MUX_UI_ERROR,
                            MUX_UI_ERROR_INVALID,
                            "authentication response contains invalid credentials");
        return FALSE;
    }

    *username_out = g_strndup((const gchar *)username_bytes,
                              username_length);
    *password_out = g_strndup((const gchar *)password_bytes,
                              password_length);
    return TRUE;
}

static gboolean
parse_choice_id(const gchar *value, guint32 *choice_id, GError **error)
{
    guint64 parsed = 0;

    if (!value ||
        !g_ascii_string_to_unsigned(value,
                                    10,
                                    0,
                                    G_MAXUINT32,
                                    &parsed,
                                    error))
        return FALSE;
    *choice_id = (guint32)parsed;
    return TRUE;
}

static gboolean
close_web_view_idle(gpointer data)
{
    webkit_web_view_try_close(WEBKIT_WEB_VIEW(data));
    return G_SOURCE_REMOVE;
}

static gboolean
resolve_pending(MuxBrowserAffordanceBridge *bridge,
                PendingAffordance *pending,
                MuxUiAction action,
                const gchar *value,
                GError **error)
{
    guint32 choice_id;

    switch (pending->kind) {
    case MUX_UI_REQUEST_AUTHENTICATION:
        if (action == MUX_UI_ACTION_CANCEL) {
            webkit_authentication_request_cancel(pending->authentication);
            return TRUE;
        }
        if (action == MUX_UI_ACTION_SUBMIT) {
            gchar *username = NULL;
            gchar *password = NULL;
            WebKitCredential *credential;

            if (!decode_authentication_value(value,
                                             &username,
                                             &password,
                                             error))
                return FALSE;
            credential = webkit_credential_new(
                username,
                password,
                bridge->private_profile
                    ? WEBKIT_CREDENTIAL_PERSISTENCE_NONE
                    : WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
            webkit_authentication_request_authenticate(
                pending->authentication, credential);
            webkit_credential_free(credential);
            g_free(username);
            wipe_and_free(password);
            return TRUE;
        }
        break;
    case MUX_UI_REQUEST_OPTION_MENU:
        if (action == MUX_UI_ACTION_CANCEL) {
            webkit_option_menu_close(pending->option_menu);
            return TRUE;
        }
        if (action == MUX_UI_ACTION_SELECT &&
            parse_choice_id(value, &choice_id, error)) {
            WebKitOptionMenuItem *item;

            if (choice_id >=
                webkit_option_menu_get_n_items(pending->option_menu))
                break;
            item = webkit_option_menu_get_item(pending->option_menu,
                                               choice_id);
            if (!item || !webkit_option_menu_item_is_enabled(item) ||
                webkit_option_menu_item_is_group_label(item))
                break;
            webkit_option_menu_activate_item(pending->option_menu,
                                             choice_id);
            webkit_option_menu_close(pending->option_menu);
            return TRUE;
        }
        if (error && *error)
            return FALSE;
        break;
    case MUX_UI_REQUEST_CONTEXT_MENU:
        if (action == MUX_UI_ACTION_CANCEL)
            return TRUE;
        if (action == MUX_UI_ACTION_SELECT &&
            parse_choice_id(value, &choice_id, error)) {
            if (pending->command_choice_func) {
                if (!g_hash_table_contains(pending->command_choice_ids,
                                           &choice_id))
                    break;
                return pending->command_choice_func(choice_id,
                                                    pending->command_data,
                                                    error);
            }
            ContextAction *item;

            if (!pending->context_actions)
                break;
            if (choice_id >= pending->context_actions->len)
                break;
            item = g_ptr_array_index(pending->context_actions,
                                     choice_id);
            if (!item || !item->action ||
                !g_action_get_enabled(item->action))
                break;
            g_action_activate(item->action, item->target);
            return TRUE;
        }
        if (error && *error)
            return FALSE;
        break;
    case MUX_UI_REQUEST_CRASH:
        if (action == MUX_UI_ACTION_RELOAD) {
            webkit_web_view_reload(bridge->web_view);
            return TRUE;
        }
        if (action == MUX_UI_ACTION_CLOSE) {
            g_idle_add_full(G_PRIORITY_DEFAULT,
                            close_web_view_idle,
                            g_object_ref(bridge->web_view),
                            g_object_unref);
            return TRUE;
        }
        break;
    default:
        break;
    }

    g_set_error_literal(error,
                        MUX_UI_ERROR,
                        MUX_UI_ERROR_INVALID,
                        "browser affordance response is invalid");
    return FALSE;
}

MuxBrowserAffordanceBridge *
mux_browser_affordance_bridge_new(
    WebKitWebView *web_view,
    gboolean private_profile,
    MuxBrowserAffordanceSendFunc send_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy)
{
    MuxBrowserAffordanceBridge *bridge;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);
    g_return_val_if_fail(send_func, NULL);

    bridge = g_new0(MuxBrowserAffordanceBridge, 1);
    bridge->web_view = g_object_ref(web_view);
    bridge->pending = g_hash_table_new_full(
        g_int64_hash,
        g_int64_equal,
        g_free,
        (GDestroyNotify)pending_affordance_free);
    bridge->send_func = send_func;
    bridge->user_data = user_data;
    bridge->user_data_destroy = user_data_destroy;
    bridge->private_profile = private_profile;
    bridge->authenticate_handler = g_signal_connect(
        web_view, "authenticate", G_CALLBACK(on_authenticate), bridge);
    bridge->option_menu_handler = g_signal_connect(
        web_view,
        "show-option-menu",
        G_CALLBACK(on_show_option_menu),
        bridge);
    bridge->context_menu_handler = g_signal_connect(
        web_view, "context-menu", G_CALLBACK(on_context_menu), bridge);
    bridge->process_terminated_handler = g_signal_connect(
        web_view,
        "web-process-terminated",
        G_CALLBACK(on_web_process_terminated),
        bridge);
    bridge->load_changed_handler = g_signal_connect(
        web_view, "load-changed", G_CALLBACK(on_load_changed), bridge);
    return bridge;
}

void
mux_browser_affordance_bridge_free(MuxBrowserAffordanceBridge *bridge)
{
    if (!bridge)
        return;
    bridge->destroying = TRUE;
    if (bridge->authenticate_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->authenticate_handler);
    if (bridge->option_menu_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->option_menu_handler);
    if (bridge->context_menu_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->context_menu_handler);
    if (bridge->process_terminated_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->process_terminated_handler);
    if (bridge->load_changed_handler)
        g_signal_handler_disconnect(bridge->web_view,
                                    bridge->load_changed_handler);
    mux_browser_affordance_bridge_cancel_all(
        bridge,
        MUX_UI_CANCEL_VIEW_DESTROYED,
        FALSE);
    g_hash_table_unref(bridge->pending);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_object_unref(bridge->web_view);
    g_free(bridge);
}

gboolean
mux_browser_affordance_bridge_handle_payload(
    MuxBrowserAffordanceBridge *bridge,
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
        PendingAffordance *pending;
        gboolean resolved;

        if (!mux_ui_response_decode(data, length, &response, error))
            return FALSE;
        pending = take_pending(bridge, response->request_id);
        if (!pending)
            return TRUE;
        if (!mux_ui_action_is_valid(pending->kind, response->action)) {
            g_set_error_literal(error,
                                MUX_UI_ERROR,
                                MUX_UI_ERROR_INVALID,
                                "UI action does not match browser request");
            resolve_cancel(pending);
            pending_affordance_free(pending);
            return FALSE;
        }
        resolved = resolve_pending(bridge,
                                   pending,
                                   response->action,
                                   response->value,
                                   error);
        if (!resolved)
            resolve_cancel(pending);
        pending_affordance_free(pending);
        return resolved;
    }

    if (type == MUX_UI_RECORD_CANCEL) {
        PendingAffordance *pending;
        guint64 request_id = 0;
        MuxUiCancelReason reason;

        if (!mux_ui_cancel_decode(data,
                                  length,
                                  &request_id,
                                  &reason,
                                  error))
            return FALSE;
        (void)reason;
        pending = take_pending(bridge, request_id);
        if (!pending)
            return TRUE;
        resolve_cancel(pending);
        pending_affordance_free(pending);
    }
    return TRUE;
}

guint
mux_browser_affordance_bridge_pending_count(
    const MuxBrowserAffordanceBridge *bridge)
{
    g_return_val_if_fail(bridge, 0);
    return g_hash_table_size(bridge->pending);
}
