#include "mux-clipboard-picker-broker.h"

#include <gio/gio.h>

typedef enum {
    BROKER_REQUEST_NONE,
    BROKER_REQUEST_LIST,
    BROKER_REQUEST_SELECT,
    BROKER_REQUEST_SET_PINNED,
    BROKER_REQUEST_DELETE,
    BROKER_REQUEST_CLEAR,
} BrokerRequest;

struct _MuxClipboardPickerBroker {
    gatomicrefcount references;
    gboolean destroying;
    MuxClipboardPickerController *controller;
    gpointer client;
    MuxClipboardPickerBrokerOps ops;
    GDestroyNotify client_destroy;
    MuxClipboardPickerBrokerApplyFunc apply_func;
    gpointer apply_data;
    GDestroyNotify apply_destroy;
    MuxClipboardPickerBrokerNotifyFunc changed_func;
    MuxClipboardPickerBrokerNotifyFunc closed_func;
    gpointer notify_data;
    GDestroyNotify notify_destroy;
    BrokerRequest active_request;
    guint64 active_serial;
    GPtrArray *list_items;
};

static void
clear_active(MuxClipboardPickerBroker *broker)
{
    broker->active_request = BROKER_REQUEST_NONE;
    broker->active_serial = 0;
    g_clear_pointer(&broker->list_items, g_ptr_array_unref);
}

static gboolean
begin_active(MuxClipboardPickerBroker *broker,
             guint64 serial,
             BrokerRequest request,
             GError **error)
{
    if (serial == 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "picker request serial must be nonzero");
        return FALSE;
    }
    if (broker->active_request != BROKER_REQUEST_NONE) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_PENDING,
                    "a clipboard broker request is already active");
        return FALSE;
    }

    broker->active_request = request;
    broker->active_serial = serial;
    if (request == BROKER_REQUEST_LIST) {
        broker->list_items = g_ptr_array_new_with_free_func(
            (GDestroyNotify) mux_clipboard_picker_item_unref);
    }
    return TRUE;
}

static gboolean
backend_list(guint64 serial, gpointer user_data, GError **error)
{
    MuxClipboardPickerBroker *broker = user_data;
    gboolean queued;

    if (!begin_active(broker, serial, BROKER_REQUEST_LIST, error))
        return FALSE;
    queued = broker->ops.list(broker->client, error);
    if (!queued && broker->active_serial == serial)
        clear_active(broker);
    return queued;
}

static gboolean
backend_select(guint64 serial,
               guint64 entry_id,
               gpointer user_data,
               GError **error)
{
    MuxClipboardPickerBroker *broker = user_data;
    gboolean queued;

    if (!begin_active(broker, serial, BROKER_REQUEST_SELECT, error))
        return FALSE;
    queued = broker->ops.select(broker->client, entry_id, error);
    if (!queued && broker->active_serial == serial)
        clear_active(broker);
    return queued;
}

static gboolean
backend_set_pinned(guint64 serial,
                   guint64 entry_id,
                   gboolean pinned,
                   gpointer user_data,
                   GError **error)
{
    MuxClipboardPickerBroker *broker = user_data;
    gboolean queued;

    if (!begin_active(broker, serial, BROKER_REQUEST_SET_PINNED, error))
        return FALSE;
    queued = broker->ops.set_pinned(broker->client,
                                    entry_id,
                                    pinned,
                                    error);
    if (!queued && broker->active_serial == serial)
        clear_active(broker);
    return queued;
}

static gboolean
backend_delete(guint64 serial,
               guint64 entry_id,
               gpointer user_data,
               GError **error)
{
    MuxClipboardPickerBroker *broker = user_data;
    gboolean queued;

    if (!begin_active(broker, serial, BROKER_REQUEST_DELETE, error))
        return FALSE;
    queued = broker->ops.delete_entry(broker->client, entry_id, error);
    if (!queued && broker->active_serial == serial)
        clear_active(broker);
    return queued;
}

static gboolean
backend_clear(guint64 serial, gpointer user_data, GError **error)
{
    MuxClipboardPickerBroker *broker = user_data;
    gboolean queued;

    if (!begin_active(broker, serial, BROKER_REQUEST_CLEAR, error))
        return FALSE;
    queued = broker->ops.clear(broker->client, error);
    if (!queued && broker->active_serial == serial)
        clear_active(broker);
    return queued;
}

static void
backend_cancel(guint64 serial, gpointer user_data)
{
    MuxClipboardPickerBroker *broker = user_data;

    if (serial == 0 || serial != broker->active_serial)
        return;
    clear_active(broker);
    broker->ops.cancel(broker->client);
}

static void
forward_changed(MuxClipboardPickerController *controller, gpointer user_data)
{
    MuxClipboardPickerBroker *broker = user_data;
    MuxClipboardPickerBroker *guard;

    (void) controller;
    if (broker->destroying || broker->changed_func == NULL)
        return;
    guard = mux_clipboard_picker_broker_ref(broker);
    broker->changed_func(broker, broker->notify_data);
    mux_clipboard_picker_broker_unref(guard);
}

static void
forward_closed(MuxClipboardPickerController *controller, gpointer user_data)
{
    MuxClipboardPickerBroker *broker = user_data;
    MuxClipboardPickerBroker *guard;

    (void) controller;
    if (broker->destroying || broker->closed_func == NULL)
        return;
    guard = mux_clipboard_picker_broker_ref(broker);
    broker->closed_func(broker, broker->notify_data);
    mux_clipboard_picker_broker_unref(guard);
}

MuxClipboardPickerBroker *
mux_clipboard_picker_broker_new(
    const gchar *profile,
    gpointer client,
    const MuxClipboardPickerBrokerOps *ops,
    GDestroyNotify client_destroy,
    MuxClipboardPickerBrokerApplyFunc apply_func,
    gpointer apply_data,
    GDestroyNotify apply_destroy,
    MuxClipboardPickerBrokerNotifyFunc changed_func,
    MuxClipboardPickerBrokerNotifyFunc closed_func,
    gpointer notify_data,
    GDestroyNotify notify_destroy)
{
    static const MuxClipboardPickerBackend controller_backend = {
        .list = backend_list,
        .select = backend_select,
        .set_pinned = backend_set_pinned,
        .delete_entry = backend_delete,
        .clear = backend_clear,
        .cancel = backend_cancel,
    };
    MuxClipboardPickerBroker *broker;

    g_return_val_if_fail(client != NULL, NULL);
    g_return_val_if_fail(ops != NULL, NULL);
    g_return_val_if_fail(ops->list != NULL, NULL);
    g_return_val_if_fail(ops->select != NULL, NULL);
    g_return_val_if_fail(ops->set_pinned != NULL, NULL);
    g_return_val_if_fail(ops->delete_entry != NULL, NULL);
    g_return_val_if_fail(ops->clear != NULL, NULL);
    g_return_val_if_fail(ops->cancel != NULL, NULL);
    g_return_val_if_fail(apply_func != NULL, NULL);

    broker = g_new0(MuxClipboardPickerBroker, 1);
    g_atomic_ref_count_init(&broker->references);
    broker->client = client;
    broker->ops = *ops;
    broker->client_destroy = client_destroy;
    broker->apply_func = apply_func;
    broker->apply_data = apply_data;
    broker->apply_destroy = apply_destroy;
    broker->changed_func = changed_func;
    broker->closed_func = closed_func;
    broker->notify_data = notify_data;
    broker->notify_destroy = notify_destroy;
    broker->controller = mux_clipboard_picker_controller_new(
        profile,
        &controller_backend,
        broker,
        NULL,
        forward_changed,
        forward_closed,
        broker,
        NULL);
    if (broker->controller == NULL) {
        if (client_destroy != NULL)
            client_destroy(client);
        if (apply_destroy != NULL)
            apply_destroy(apply_data);
        if (notify_destroy != NULL)
            notify_destroy(notify_data);
        g_free(broker);
        return NULL;
    }
    return broker;
}

MuxClipboardPickerBroker *
mux_clipboard_picker_broker_ref(MuxClipboardPickerBroker *broker)
{
    g_return_val_if_fail(broker != NULL, NULL);
    g_atomic_ref_count_inc(&broker->references);
    return broker;
}

void
mux_clipboard_picker_broker_unref(MuxClipboardPickerBroker *broker)
{
    if (broker == NULL || !g_atomic_ref_count_dec(&broker->references))
        return;

    broker->destroying = TRUE;
    mux_clipboard_picker_controller_unref(broker->controller);
    clear_active(broker);
    if (broker->client_destroy != NULL)
        broker->client_destroy(broker->client);
    if (broker->apply_destroy != NULL)
        broker->apply_destroy(broker->apply_data);
    if (broker->notify_destroy != NULL)
        broker->notify_destroy(broker->notify_data);
    g_free(broker);
}

void
mux_clipboard_picker_broker_open(MuxClipboardPickerBroker *broker)
{
    MuxClipboardPickerBroker *guard;

    g_return_if_fail(broker != NULL);
    guard = mux_clipboard_picker_broker_ref(broker);
    mux_clipboard_picker_controller_open(broker->controller);
    mux_clipboard_picker_broker_unref(guard);
}

void
mux_clipboard_picker_broker_close(MuxClipboardPickerBroker *broker)
{
    MuxClipboardPickerBroker *guard;

    g_return_if_fail(broker != NULL);
    guard = mux_clipboard_picker_broker_ref(broker);
    mux_clipboard_picker_controller_close(broker->controller);
    mux_clipboard_picker_broker_unref(guard);
}

MuxClipboardPickerControllerState
mux_clipboard_picker_broker_get_state(const MuxClipboardPickerBroker *broker)
{
    g_return_val_if_fail(broker != NULL,
                         MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED);
    return mux_clipboard_picker_controller_get_state(broker->controller);
}

gboolean
mux_clipboard_picker_broker_handle_key(MuxClipboardPickerBroker *broker,
                                       MuxClipboardPickerKey key,
                                       gunichar text)
{
    MuxClipboardPickerBroker *guard;
    gboolean handled;

    g_return_val_if_fail(broker != NULL, FALSE);
    guard = mux_clipboard_picker_broker_ref(broker);
    handled = mux_clipboard_picker_controller_handle_key(broker->controller,
                                                        key,
                                                        text);
    mux_clipboard_picker_broker_unref(guard);
    return handled;
}

gchar *
mux_clipboard_picker_broker_render(MuxClipboardPickerBroker *broker,
                                   guint terminal_columns,
                                   guint terminal_rows)
{
    g_return_val_if_fail(broker != NULL, NULL);
    return mux_clipboard_picker_controller_render(broker->controller,
                                                  terminal_columns,
                                                  terminal_rows);
}

gboolean
mux_clipboard_picker_broker_add_summary(
    MuxClipboardPickerBroker *broker,
    guint64 id,
    gint64 created_us,
    const gchar *origin,
    guint64 source_view_id,
    gboolean pinned,
    gsize total_size,
    const gchar *preview,
    const gchar *const *mime_types,
    gsize mime_type_count,
    GError **error)
{
    MuxClipboardPickerItem *item;

    g_return_val_if_fail(broker != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (broker->active_request != BROKER_REQUEST_LIST ||
        broker->list_items == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "clipboard summary arrived outside a list request");
        return FALSE;
    }

    item = mux_clipboard_picker_item_new(id,
                                         created_us,
                                         origin,
                                         source_view_id,
                                         pinned,
                                         total_size,
                                         preview,
                                         mime_types,
                                         mime_type_count);
    if (item == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "clipboard broker returned an invalid summary");
        return FALSE;
    }
    g_ptr_array_add(broker->list_items, item);
    return TRUE;
}

void
mux_clipboard_picker_broker_complete_list(MuxClipboardPickerBroker *broker,
                                          const GError *error)
{
    MuxClipboardPickerBroker *guard;
    GPtrArray *items;
    guint64 serial;

    g_return_if_fail(broker != NULL);
    if (broker->active_request != BROKER_REQUEST_LIST)
        return;

    guard = mux_clipboard_picker_broker_ref(broker);
    serial = broker->active_serial;
    items = g_steal_pointer(&broker->list_items);
    broker->active_request = BROKER_REQUEST_NONE;
    broker->active_serial = 0;
    mux_clipboard_picker_controller_complete_list(broker->controller,
                                                  serial,
                                                  items,
                                                  error);
    g_ptr_array_unref(items);
    mux_clipboard_picker_broker_unref(guard);
}

void
mux_clipboard_picker_broker_complete_selection(
    MuxClipboardPickerBroker *broker,
    gpointer snapshot,
    const GError *error)
{
    g_autoptr(GError) apply_error = NULL;
    MuxClipboardPickerBroker *guard;
    guint64 serial;

    g_return_if_fail(broker != NULL);
    if (broker->active_request != BROKER_REQUEST_SELECT)
        return;

    guard = mux_clipboard_picker_broker_ref(broker);
    serial = broker->active_serial;
    if (error == NULL && snapshot == NULL) {
        apply_error = g_error_new_literal(G_IO_ERROR,
                                          G_IO_ERROR_INVALID_DATA,
                                          "broker returned no clipboard snapshot");
    } else if (error == NULL &&
               !broker->apply_func(snapshot,
                                   broker->apply_data,
                                   &apply_error) &&
               apply_error == NULL) {
        apply_error = g_error_new_literal(G_IO_ERROR,
                                          G_IO_ERROR_FAILED,
                                          "clipboard snapshot was not applied");
    }

    if (broker->active_request == BROKER_REQUEST_SELECT &&
        broker->active_serial == serial) {
        broker->active_request = BROKER_REQUEST_NONE;
        broker->active_serial = 0;
        mux_clipboard_picker_controller_complete_request(
            broker->controller,
            serial,
            error != NULL ? error : apply_error);
    }
    mux_clipboard_picker_broker_unref(guard);
}

void
mux_clipboard_picker_broker_complete_mutation(
    MuxClipboardPickerBroker *broker,
    const GError *error)
{
    MuxClipboardPickerBroker *guard;
    guint64 serial;

    g_return_if_fail(broker != NULL);
    if (broker->active_request != BROKER_REQUEST_SET_PINNED &&
        broker->active_request != BROKER_REQUEST_DELETE &&
        broker->active_request != BROKER_REQUEST_CLEAR)
        return;

    guard = mux_clipboard_picker_broker_ref(broker);
    serial = broker->active_serial;
    broker->active_request = BROKER_REQUEST_NONE;
    broker->active_serial = 0;
    mux_clipboard_picker_controller_complete_request(broker->controller,
                                                     serial,
                                                     error);
    mux_clipboard_picker_broker_unref(guard);
}

void
mux_clipboard_picker_broker_fail_active(MuxClipboardPickerBroker *broker,
                                        const GError *error)
{
    g_autoptr(GError) fallback = NULL;

    g_return_if_fail(broker != NULL);
    if (broker->active_request == BROKER_REQUEST_NONE)
        return;
    if (error == NULL) {
        fallback = g_error_new_literal(G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "clipboard broker request failed");
        error = fallback;
    }

    switch (broker->active_request) {
    case BROKER_REQUEST_LIST:
        mux_clipboard_picker_broker_complete_list(broker, error);
        break;
    case BROKER_REQUEST_SELECT:
        mux_clipboard_picker_broker_complete_selection(broker, NULL, error);
        break;
    case BROKER_REQUEST_SET_PINNED:
    case BROKER_REQUEST_DELETE:
    case BROKER_REQUEST_CLEAR:
        mux_clipboard_picker_broker_complete_mutation(broker, error);
        break;
    case BROKER_REQUEST_NONE:
        break;
    }
}
