#include "mux-popup-engine.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

typedef struct {
    MuxPopupManager *manager;
    gchar *token;
    WebKitWebView *child;
    gint64 expires_at_us;
    gboolean ready;
    gboolean offered;
    gulong ready_handler;
    gulong close_handler;
} PopupRecord;

struct _MuxPopupManager {
    WebKitWebView *parent;
    GHashTable *by_token;
    GHashTable *by_child;
    guint creating_count;
    MuxPopupCreateFunc create_func;
    MuxPopupOfferFunc offer_func;
    MuxPopupDestroyFunc destroy_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gulong create_handler;
};

static gboolean
popup_request_is_admissible(gboolean user_gesture,
                            guint pending_count,
                            guint creating_count)
{
    return user_gesture && pending_count < MUX_POPUP_MAX_PENDING &&
           creating_count < MUX_POPUP_MAX_PENDING - pending_count;
}

static gboolean
random_bytes(guint8 *data, gsize length)
{
    gint descriptor;
    gsize offset = 0;

    descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return FALSE;
    while (offset < length) {
        ssize_t amount = read(descriptor, data + offset, length - offset);

        if (amount < 0) {
            if (errno == EINTR)
                continue;
            close(descriptor);
            return FALSE;
        }
        if (!amount) {
            close(descriptor);
            return FALSE;
        }
        offset += amount;
    }
    return close(descriptor) == 0;
}

static gchar *
new_token(MuxPopupManager *manager)
{
    static const gchar digits[] = "0123456789abcdef";
    guint8 random[MUX_POPUP_TOKEN_BYTES];
    gchar *token;
    guint i;

    do {
        if (!random_bytes(random, sizeof(random)))
            return NULL;
        token = g_malloc(MUX_POPUP_TOKEN_LENGTH + 1);
        for (i = 0; i < sizeof(random); i++) {
            token[i * 2] = digits[random[i] >> 4];
            token[i * 2 + 1] = digits[random[i] & 0x0f];
        }
        token[MUX_POPUP_TOKEN_LENGTH] = '\0';
        if (!g_hash_table_contains(manager->by_token, token))
            return token;
        g_free(token);
    } while (TRUE);
}

static void
disconnect_record(PopupRecord *record)
{
    if (record->ready_handler)
        g_signal_handler_disconnect(record->child,
                                    record->ready_handler);
    if (record->close_handler)
        g_signal_handler_disconnect(record->child,
                                    record->close_handler);
    record->ready_handler = 0;
    record->close_handler = 0;
}

static void
popup_record_free(PopupRecord *record)
{
    if (!record)
        return;
    if (record->child) {
        disconnect_record(record);
        record->manager->destroy_func(
            record->child, record->manager->user_data);
    }
    g_free(record->token);
    g_free(record);
}

static void
remove_record(PopupRecord *record)
{
    MuxPopupManager *manager = record->manager;
    g_autofree gchar *token = g_strdup(record->token);
    WebKitWebView *child = record->child;

    g_hash_table_remove(manager->by_child, child);
    g_hash_table_remove(manager->by_token, token);
}

static void
on_child_ready(WebKitWebView *child, PopupRecord *record)
{
    MuxPopupManager *manager = record->manager;
    g_autofree gchar *token = g_strdup(record->token);
    g_autoptr(GError) error = NULL;
    gboolean offered;

    if (record->ready)
        return;
    record->ready = TRUE;
    record->expires_at_us =
        g_get_monotonic_time() +
        ((gint64)MUX_POPUP_ATTACH_TIMEOUT_MS * 1000);
    record->offered = TRUE;
    offered = manager->offer_func(manager->parent,
                                  child,
                                  token,
                                  manager->user_data,
                                  &error);

    record = g_hash_table_lookup(manager->by_token, token);
    if (!record)
        return;
    if (!offered) {
        if (error)
            g_warning("could not place popup pane: %s", error->message);
        remove_record(record);
        return;
    }
}

static void
on_child_close(WebKitWebView *child, PopupRecord *record)
{
    (void)child;
    remove_record(record);
}

static WebKitWebView *
on_create(WebKitWebView *parent,
          WebKitNavigationAction *navigation_action,
          MuxPopupManager *manager)
{
    g_autoptr(GError) error = NULL;
    WebKitWebView *child;
    PopupRecord *record;
    gchar *token;
    guint pending_count = g_hash_table_size(manager->by_token);
    gboolean user_gesture =
        navigation_action &&
        webkit_navigation_action_is_user_gesture(navigation_action);

    if (!popup_request_is_admissible(user_gesture,
                                     pending_count,
                                     manager->creating_count))
        return NULL;

    manager->creating_count++;
    child = manager->create_func(parent,
                                 navigation_action,
                                 manager->user_data,
                                 &error);
    manager->creating_count--;
    if (!child) {
        if (error)
            g_warning("popup creation denied: %s", error->message);
        return NULL;
    }
    if (!WEBKIT_IS_WEB_VIEW(child) ||
        g_hash_table_contains(manager->by_child, child)) {
        manager->destroy_func(child, manager->user_data);
        return NULL;
    }
    token = new_token(manager);
    if (!token) {
        manager->destroy_func(child, manager->user_data);
        g_warning("could not read randomness for popup token");
        return NULL;
    }

    record = g_new0(PopupRecord, 1);
    record->manager = manager;
    record->token = token;
    record->child = child;
    record->expires_at_us =
        g_get_monotonic_time() +
        ((gint64)MUX_POPUP_ATTACH_TIMEOUT_MS * 1000);
    record->ready_handler =
        g_signal_connect(child,
                         "ready-to-show",
                         G_CALLBACK(on_child_ready),
                         record);
    record->close_handler =
        g_signal_connect(child,
                         "close",
                         G_CALLBACK(on_child_close),
                         record);
    g_hash_table_insert(manager->by_child, child, record);
    g_hash_table_insert(manager->by_token, record->token, record);
    return child;
}

MuxPopupManager *
mux_popup_manager_new(WebKitWebView *parent,
                      MuxPopupCreateFunc create_func,
                      MuxPopupOfferFunc offer_func,
                      MuxPopupDestroyFunc destroy_func,
                      gpointer user_data,
                      GDestroyNotify user_data_destroy)
{
    MuxPopupManager *manager;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(parent), NULL);
    g_return_val_if_fail(create_func, NULL);
    g_return_val_if_fail(offer_func, NULL);
    g_return_val_if_fail(destroy_func, NULL);

    manager = g_new0(MuxPopupManager, 1);
    manager->parent = g_object_ref(parent);
    manager->by_token = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        NULL,
        (GDestroyNotify)popup_record_free);
    manager->by_child =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    manager->create_func = create_func;
    manager->offer_func = offer_func;
    manager->destroy_func = destroy_func;
    manager->user_data = user_data;
    manager->user_data_destroy = user_data_destroy;
    manager->create_handler =
        g_signal_connect(parent,
                         "create",
                         G_CALLBACK(on_create),
                         manager);
    return manager;
}

void
mux_popup_manager_free(MuxPopupManager *manager)
{
    if (!manager)
        return;
    if (manager->create_handler)
        g_signal_handler_disconnect(manager->parent,
                                    manager->create_handler);
    g_hash_table_remove_all(manager->by_child);
    g_clear_pointer(&manager->by_token, g_hash_table_unref);
    g_clear_pointer(&manager->by_child, g_hash_table_unref);
    if (manager->user_data_destroy)
        manager->user_data_destroy(manager->user_data);
    g_clear_object(&manager->parent);
    g_free(manager);
}

WebKitWebView *
mux_popup_manager_claim(MuxPopupManager *manager,
                        const gchar *token,
                        GError **error)
{
    PopupRecord *record;
    WebKitWebView *child;

    g_return_val_if_fail(manager, NULL);
    if (!token || strlen(token) != MUX_POPUP_TOKEN_LENGTH) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "invalid popup attachment token");
        return NULL;
    }
    record = g_hash_table_lookup(manager->by_token, token);
    if (!record || !record->ready || !record->offered ||
        record->expires_at_us <= g_get_monotonic_time()) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "popup attachment token is unavailable");
        return NULL;
    }

    child = record->child;
    disconnect_record(record);
    g_hash_table_remove(manager->by_child, child);
    g_hash_table_steal(manager->by_token, token);
    record->child = NULL;
    g_free(record->token);
    g_free(record);
    return child;
}

guint
mux_popup_manager_tick(MuxPopupManager *manager,
                       gint64 monotonic_us)
{
    GHashTableIter iterator;
    gpointer value;
    g_autoptr(GPtrArray) expired =
        g_ptr_array_new_with_free_func(g_free);
    guint removed = 0;
    guint i;

    g_return_val_if_fail(manager, 0);
    g_hash_table_iter_init(&iterator, manager->by_token);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PopupRecord *record = value;

        if (record->expires_at_us <= monotonic_us)
            g_ptr_array_add(expired, g_strdup(record->token));
    }
    for (i = 0; i < expired->len; i++) {
        const gchar *token = g_ptr_array_index(expired, i);
        PopupRecord *record =
            g_hash_table_lookup(manager->by_token, token);

        if (record) {
            remove_record(record);
            removed++;
        }
    }
    return removed;
}

guint
mux_popup_manager_pending_count(const MuxPopupManager *manager)
{
    g_return_val_if_fail(manager, 0);
    return g_hash_table_size(manager->by_token);
}
