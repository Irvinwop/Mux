#include "mux-clipboard-picker-controller.h"

#include <gio/gio.h>

typedef enum {
    PENDING_NONE,
    PENDING_LIST,
    PENDING_SELECT,
    PENDING_SET_PINNED,
    PENDING_DELETE,
    PENDING_CLEAR,
} PendingRequest;

struct _MuxClipboardPickerController {
    gatomicrefcount references;
    MuxClipboardPicker *picker;
    MuxClipboardPickerBackend backend;
    gpointer backend_data;
    GDestroyNotify backend_destroy;
    MuxClipboardPickerControllerNotifyFunc changed_func;
    MuxClipboardPickerControllerNotifyFunc closed_func;
    gpointer notify_data;
    GDestroyNotify notify_destroy;
    MuxClipboardPickerControllerState state;
    PendingRequest pending;
    gboolean backend_invoked;
    guint64 next_serial;
    guint64 active_serial;
};

static void
notify_changed(MuxClipboardPickerController *controller)
{
    MuxClipboardPickerController *guard;

    if (controller->changed_func == NULL)
        return;
    guard = mux_clipboard_picker_controller_ref(controller);
    controller->changed_func(controller, controller->notify_data);
    mux_clipboard_picker_controller_unref(guard);
}

static guint64
allocate_serial(MuxClipboardPickerController *controller)
{
    controller->next_serial++;
    if (controller->next_serial == 0)
        controller->next_serial++;
    return controller->next_serial;
}

static void
set_failure_status(MuxClipboardPickerController *controller,
                   const gchar *operation,
                   const GError *error)
{
    g_autofree gchar *status = g_strdup_printf(
        "%s failed: %s",
        operation,
        error != NULL ? error->message : "unspecified error");

    mux_clipboard_picker_set_status(controller->picker, status);
}

static void
request_list(MuxClipboardPickerController *controller)
{
    g_autoptr(GError) error = NULL;
    MuxClipboardPickerController *guard;
    guint64 serial;
    gboolean queued;

    guard = mux_clipboard_picker_controller_ref(controller);
    if (controller->state == MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED)
        goto out;

    serial = allocate_serial(controller);
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING;
    controller->pending = PENDING_LIST;
    controller->backend_invoked = FALSE;
    controller->active_serial = serial;
    mux_clipboard_picker_set_status(controller->picker, "loading history");
    notify_changed(controller);

    if (controller->state != MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING ||
        controller->active_serial != serial)
        goto out;

    controller->backend_invoked = TRUE;
    queued = controller->backend.list != NULL &&
             controller->backend.list(serial,
                                      controller->backend_data,
                                      &error);
    if (!queued &&
        controller->state == MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING &&
        controller->active_serial == serial) {
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_READY;
        controller->pending = PENDING_NONE;
        controller->backend_invoked = FALSE;
        controller->active_serial = 0;
        set_failure_status(controller, "history request", error);
        notify_changed(controller);
    }

out:
    mux_clipboard_picker_controller_unref(guard);
}

MuxClipboardPickerController *
mux_clipboard_picker_controller_new(
    const gchar *profile,
    const MuxClipboardPickerBackend *backend,
    gpointer backend_data,
    GDestroyNotify backend_destroy,
    MuxClipboardPickerControllerNotifyFunc changed_func,
    MuxClipboardPickerControllerNotifyFunc closed_func,
    gpointer notify_data,
    GDestroyNotify notify_destroy)
{
    MuxClipboardPickerController *controller;

    g_return_val_if_fail(backend != NULL, NULL);
    g_return_val_if_fail(backend->list != NULL, NULL);

    controller = g_new0(MuxClipboardPickerController, 1);
    g_atomic_ref_count_init(&controller->references);
    controller->picker = mux_clipboard_picker_new(profile);
    controller->backend = *backend;
    controller->backend_data = backend_data;
    controller->backend_destroy = backend_destroy;
    controller->changed_func = changed_func;
    controller->closed_func = closed_func;
    controller->notify_data = notify_data;
    controller->notify_destroy = notify_destroy;
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED;
    return controller;
}

MuxClipboardPickerController *
mux_clipboard_picker_controller_ref(
    MuxClipboardPickerController *controller)
{
    g_return_val_if_fail(controller != NULL, NULL);
    g_atomic_ref_count_inc(&controller->references);
    return controller;
}

void
mux_clipboard_picker_controller_unref(
    MuxClipboardPickerController *controller)
{
    if (controller == NULL ||
        !g_atomic_ref_count_dec(&controller->references))
        return;

    if (controller->state != MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED &&
        controller->active_serial != 0 && controller->backend_invoked &&
        controller->backend.cancel != NULL)
        controller->backend.cancel(controller->active_serial,
                                   controller->backend_data);
    mux_clipboard_picker_free(controller->picker);
    if (controller->backend_destroy != NULL)
        controller->backend_destroy(controller->backend_data);
    if (controller->notify_destroy != NULL)
        controller->notify_destroy(controller->notify_data);
    g_free(controller);
}

void
mux_clipboard_picker_controller_open(
    MuxClipboardPickerController *controller)
{
    GPtrArray *empty_items;

    g_return_if_fail(controller != NULL);

    if (controller->state != MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED)
        return;
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_READY;
    empty_items = g_ptr_array_new();
    mux_clipboard_picker_set_items(controller->picker, empty_items);
    g_ptr_array_unref(empty_items);
    mux_clipboard_picker_set_query(controller->picker, "");
    request_list(controller);
}

void
mux_clipboard_picker_controller_close(
    MuxClipboardPickerController *controller)
{
    MuxClipboardPickerController *guard;
    guint64 cancelled_serial;
    gboolean cancel_backend;

    g_return_if_fail(controller != NULL);

    if (controller->state == MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED)
        return;

    guard = mux_clipboard_picker_controller_ref(controller);
    cancelled_serial = controller->active_serial;
    cancel_backend = controller->backend_invoked;
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED;
    controller->pending = PENDING_NONE;
    controller->backend_invoked = FALSE;
    controller->active_serial = 0;
    controller->next_serial++;
    mux_clipboard_picker_set_status(controller->picker, "");

    if (cancelled_serial != 0 && cancel_backend &&
        controller->backend.cancel != NULL)
        controller->backend.cancel(cancelled_serial,
                                   controller->backend_data);
    notify_changed(controller);
    if (controller->closed_func != NULL)
        controller->closed_func(controller, controller->notify_data);
    mux_clipboard_picker_controller_unref(guard);
}

MuxClipboardPickerControllerState
mux_clipboard_picker_controller_get_state(
    const MuxClipboardPickerController *controller)
{
    g_return_val_if_fail(controller != NULL,
                         MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED);
    return controller->state;
}

guint64
mux_clipboard_picker_controller_get_active_serial(
    const MuxClipboardPickerController *controller)
{
    g_return_val_if_fail(controller != NULL, 0);
    return controller->active_serial;
}

void
mux_clipboard_picker_controller_complete_list(
    MuxClipboardPickerController *controller,
    guint64 serial,
    GPtrArray *items,
    const GError *error)
{
    g_return_if_fail(controller != NULL);

    if (serial == 0 || serial != controller->active_serial ||
        controller->pending != PENDING_LIST ||
        controller->state != MUX_CLIPBOARD_PICKER_CONTROLLER_LOADING)
        return;

    controller->active_serial = 0;
    controller->pending = PENDING_NONE;
    controller->backend_invoked = FALSE;
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_READY;
    if (error != NULL) {
        set_failure_status(controller, "history request", error);
    } else if (items == NULL) {
        g_autoptr(GError) missing_items = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "broker returned no history list");
        set_failure_status(controller, "history request", missing_items);
    } else {
        mux_clipboard_picker_set_items(controller->picker, items);
        mux_clipboard_picker_set_status(controller->picker, "");
    }
    notify_changed(controller);
}

static gboolean
start_action_request(MuxClipboardPickerController *controller,
                     const MuxClipboardPickerAction *action)
{
    g_autoptr(GError) error = NULL;
    MuxClipboardPickerController *guard =
        mux_clipboard_picker_controller_ref(controller);
    guint64 serial = allocate_serial(controller);
    gboolean queued = FALSE;
    const gchar *operation;

    controller->active_serial = serial;
    controller->backend_invoked = FALSE;
    switch (action->kind) {
    case MUX_CLIPBOARD_PICKER_ACTION_SELECT:
        controller->pending = PENDING_SELECT;
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_SELECTING;
        operation = "paste";
        mux_clipboard_picker_set_status(controller->picker, "loading entry");
        notify_changed(controller);
        if (controller->active_serial != serial)
            goto cancelled;
        controller->backend_invoked = TRUE;
        queued = controller->backend.select != NULL &&
                 controller->backend.select(serial,
                                            action->entry_id,
                                            controller->backend_data,
                                            &error);
        break;
    case MUX_CLIPBOARD_PICKER_ACTION_SET_PINNED:
        controller->pending = PENDING_SET_PINNED;
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING;
        operation = "pin update";
        mux_clipboard_picker_set_status(controller->picker,
                                        "updating pinned state");
        notify_changed(controller);
        if (controller->active_serial != serial)
            goto cancelled;
        controller->backend_invoked = TRUE;
        queued = controller->backend.set_pinned != NULL &&
                 controller->backend.set_pinned(serial,
                                                action->entry_id,
                                                action->pinned,
                                                controller->backend_data,
                                                &error);
        break;
    case MUX_CLIPBOARD_PICKER_ACTION_DELETE:
        controller->pending = PENDING_DELETE;
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING;
        operation = "delete";
        mux_clipboard_picker_set_status(controller->picker, "deleting entry");
        notify_changed(controller);
        if (controller->active_serial != serial)
            goto cancelled;
        controller->backend_invoked = TRUE;
        queued = controller->backend.delete_entry != NULL &&
                 controller->backend.delete_entry(serial,
                                                  action->entry_id,
                                                  controller->backend_data,
                                                  &error);
        break;
    case MUX_CLIPBOARD_PICKER_ACTION_CLEAR:
        controller->pending = PENDING_CLEAR;
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_MUTATING;
        operation = "clear";
        mux_clipboard_picker_set_status(controller->picker,
                                        "clearing unpinned history");
        notify_changed(controller);
        if (controller->active_serial != serial)
            goto cancelled;
        controller->backend_invoked = TRUE;
        queued = controller->backend.clear != NULL &&
                 controller->backend.clear(serial,
                                           controller->backend_data,
                                           &error);
        break;
    default:
        controller->active_serial = 0;
        controller->backend_invoked = FALSE;
        mux_clipboard_picker_controller_unref(guard);
        return FALSE;
    }

    if (!queued && controller->active_serial == serial) {
        controller->active_serial = 0;
        controller->pending = PENDING_NONE;
        controller->backend_invoked = FALSE;
        controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_READY;
        set_failure_status(controller, operation, error);
        notify_changed(controller);
        mux_clipboard_picker_controller_unref(guard);
        return FALSE;
    }
    mux_clipboard_picker_controller_unref(guard);
    return TRUE;

cancelled:
    mux_clipboard_picker_controller_unref(guard);
    return TRUE;
}

void
mux_clipboard_picker_controller_complete_request(
    MuxClipboardPickerController *controller,
    guint64 serial,
    const GError *error)
{
    PendingRequest completed;

    g_return_if_fail(controller != NULL);

    if (serial == 0 || serial != controller->active_serial ||
        controller->pending == PENDING_NONE ||
        controller->pending == PENDING_LIST)
        return;

    completed = controller->pending;
    controller->active_serial = 0;
    controller->pending = PENDING_NONE;
    controller->backend_invoked = FALSE;
    controller->state = MUX_CLIPBOARD_PICKER_CONTROLLER_READY;

    if (error != NULL) {
        const gchar *operation = completed == PENDING_SELECT
                                     ? "paste"
                                     : "history update";
        set_failure_status(controller, operation, error);
        notify_changed(controller);
        return;
    }

    if (completed == PENDING_SELECT) {
        mux_clipboard_picker_controller_close(controller);
        return;
    }

    request_list(controller);
}

gboolean
mux_clipboard_picker_controller_handle_key(
    MuxClipboardPickerController *controller,
    MuxClipboardPickerKey key,
    gunichar text)
{
    MuxClipboardPickerAction action;

    g_return_val_if_fail(controller != NULL, FALSE);

    if (controller->state == MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED)
        return FALSE;
    if (key == MUX_CLIPBOARD_PICKER_KEY_ESCAPE) {
        mux_clipboard_picker_controller_close(controller);
        return TRUE;
    }
    if (controller->state != MUX_CLIPBOARD_PICKER_CONTROLLER_READY)
        return FALSE;

    if (!mux_clipboard_picker_handle_key(controller->picker,
                                         key,
                                         text,
                                         &action))
        return FALSE;

    if (action.kind == MUX_CLIPBOARD_PICKER_ACTION_NONE) {
        mux_clipboard_picker_set_status(controller->picker, "");
        notify_changed(controller);
        return TRUE;
    }
    if (action.kind == MUX_CLIPBOARD_PICKER_ACTION_CLOSE) {
        mux_clipboard_picker_controller_close(controller);
        return TRUE;
    }
    return start_action_request(controller, &action);
}

gchar *
mux_clipboard_picker_controller_render(
    MuxClipboardPickerController *controller,
    guint terminal_columns,
    guint terminal_rows)
{
    g_return_val_if_fail(controller != NULL, NULL);

    if (controller->state == MUX_CLIPBOARD_PICKER_CONTROLLER_CLOSED)
        return NULL;
    return mux_clipboard_picker_render(controller->picker,
                                       terminal_columns,
                                       terminal_rows);
}
