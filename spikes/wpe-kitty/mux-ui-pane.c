#include "mux-ui-pane.h"

#include <string.h>

struct _MuxUiPaneBridge {
    gatomicrefcount references;
    MuxPaneOverlay *overlay;
    MuxUiPaneSendFunc send_func;
    MuxUiPaneWriteFunc write_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    guint columns;
    guint rows;
    gboolean painted;
    gboolean disposing;
};

static MuxUiPaneBridge *
ui_pane_bridge_ref(MuxUiPaneBridge *bridge)
{
    g_atomic_ref_count_inc(&bridge->references);
    return bridge;
}

static void
ui_pane_bridge_unref(MuxUiPaneBridge *bridge)
{
    if (!g_atomic_ref_count_dec(&bridge->references))
        return;
    mux_pane_overlay_free(bridge->overlay);
    if (bridge->user_data_destroy)
        bridge->user_data_destroy(bridge->user_data);
    g_free(bridge);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxUiPaneBridge, ui_pane_bridge_unref)

static gboolean
write_ansi(MuxUiPaneBridge *bridge,
           const gchar *sequence,
           GError **error)
{
    gsize length;

    if (!sequence || !(length = strlen(sequence)))
        return TRUE;
    return bridge->write_func((const guint8 *)sequence,
                              length,
                              bridge->user_data,
                              error);
}

static gboolean
send_response(MuxUiPaneBridge *bridge,
              const MuxUiResponse *response,
              GError **error)
{
    g_autoptr(GBytes) payload = mux_ui_response_encode(response, error);

    if (!payload)
        return FALSE;
    return bridge->send_func(payload, bridge->user_data, error);
}

static gboolean
send_unsupported(MuxUiPaneBridge *bridge,
                 guint64 request_id,
                 GError **error)
{
    g_autoptr(MuxUiResponse) response =
        mux_ui_response_new(request_id, MUX_UI_ACTION_UNSUPPORTED);

    return send_response(bridge, response, error);
}

MuxUiPaneBridge *
mux_ui_pane_bridge_new(MuxUiPaneSendFunc send_func,
                       MuxUiPaneWriteFunc write_func,
                       gpointer user_data,
                       GDestroyNotify user_data_destroy)
{
    MuxUiPaneBridge *bridge;

    g_return_val_if_fail(send_func, NULL);
    g_return_val_if_fail(write_func, NULL);

    bridge = g_new0(MuxUiPaneBridge, 1);
    g_atomic_ref_count_init(&bridge->references);
    bridge->overlay = mux_pane_overlay_new();
    bridge->send_func = send_func;
    bridge->write_func = write_func;
    bridge->user_data = user_data;
    bridge->user_data_destroy = user_data_destroy;
    return bridge;
}

void
mux_ui_pane_bridge_free(MuxUiPaneBridge *bridge)
{
    if (!bridge || bridge->disposing)
        return;
    bridge->disposing = TRUE;
    if (bridge->painted && bridge->rows) {
        g_autofree gchar *clear =
            mux_pane_overlay_render_clear(bridge->rows);

        write_ansi(bridge, clear, NULL);
    }
    ui_pane_bridge_unref(bridge);
}

gboolean
mux_ui_pane_bridge_repaint(MuxUiPaneBridge *bridge,
                           gint64 monotonic_us,
                           GError **error)
{
    g_autoptr(MuxUiPaneBridge) guard = NULL;
    g_autofree gchar *sequence = NULL;

    g_return_val_if_fail(bridge, FALSE);
    guard = ui_pane_bridge_ref(bridge);
    if (bridge->disposing)
        return FALSE;
    if (!bridge->columns || !bridge->rows)
        return TRUE;

    if (mux_pane_overlay_is_active(bridge->overlay)) {
        sequence = mux_pane_overlay_render(bridge->overlay,
                                           bridge->columns,
                                           bridge->rows,
                                           monotonic_us);
        if (!write_ansi(bridge, sequence, error) || bridge->disposing)
            return FALSE;
        bridge->painted = TRUE;
        return TRUE;
    }

    if (!bridge->painted)
        return TRUE;
    sequence = mux_pane_overlay_render_clear(bridge->rows);
    if (!write_ansi(bridge, sequence, error) || bridge->disposing)
        return FALSE;
    bridge->painted = FALSE;
    return TRUE;
}

gboolean
mux_ui_pane_bridge_handle_payload(MuxUiPaneBridge *bridge,
                                  const guint8 *data,
                                  gsize length,
                                  GError **error)
{
    g_autoptr(MuxUiPaneBridge) guard = NULL;
    MuxUiRecordType type;

    g_return_val_if_fail(bridge, FALSE);
    guard = ui_pane_bridge_ref(bridge);
    if (bridge->disposing)
        return FALSE;
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;

    if (type == MUX_UI_RECORD_REQUEST) {
        g_autoptr(MuxUiRequest) request = NULL;
        g_autoptr(GError) queue_error = NULL;
        guint64 request_id;

        if (!mux_ui_request_decode(data, length, &request, error))
            return FALSE;
        request_id = request->request_id;
        if (mux_pane_overlay_contains(bridge->overlay, request_id))
            return TRUE;
        if (!mux_pane_overlay_push(bridge->overlay,
                                   request,
                                   &queue_error)) {
            if (!send_unsupported(bridge, request_id, error))
                return FALSE;
            return TRUE;
        }
        request = NULL;
        return mux_ui_pane_bridge_repaint(
            bridge, g_get_monotonic_time(), error);
    }

    if (type == MUX_UI_RECORD_CANCEL) {
        guint64 request_id;
        MuxUiCancelReason reason;

        if (!mux_ui_cancel_decode(
                data, length, &request_id, &reason, error))
            return FALSE;
        if (!mux_pane_overlay_cancel(bridge->overlay, request_id))
            return TRUE;
        return mux_ui_pane_bridge_repaint(
            bridge, g_get_monotonic_time(), error);
    }

    g_set_error_literal(error,
                        MUX_UI_ERROR,
                        MUX_UI_ERROR_INVALID,
                        "pane received a UI response from mux-engine");
    return FALSE;
}

static gboolean
finish_response(MuxUiPaneBridge *bridge,
                MuxUiResponse *response,
                gint64 monotonic_us,
                GError **error)
{
    g_autoptr(MuxUiPaneBridge) guard = ui_pane_bridge_ref(bridge);
    g_autoptr(MuxUiResponse) owned_response = response;
    g_autoptr(GError) send_error = NULL;
    gboolean sent;
    gboolean painted;

    bridge = guard;
    if (bridge->disposing)
        return FALSE;
    sent = send_response(bridge, owned_response, &send_error);
    if (bridge->disposing)
        return FALSE;
    painted = mux_ui_pane_bridge_repaint(
        bridge, monotonic_us, sent ? error : NULL);
    if (!sent) {
        g_propagate_error(error, g_steal_pointer(&send_error));
        return FALSE;
    }
    return painted;
}

gboolean
mux_ui_pane_bridge_handle_key(MuxUiPaneBridge *bridge,
                              MuxPaneOverlayKey key,
                              gunichar text,
                              gboolean *consumed,
                              GError **error)
{
    MuxUiResponse *response = NULL;

    g_return_val_if_fail(bridge, FALSE);
    g_return_val_if_fail(consumed, FALSE);
    *consumed = mux_pane_overlay_handle_key(
        bridge->overlay, key, text, &response);
    if (!*consumed)
        return TRUE;
    if (response)
        return finish_response(bridge,
                               response,
                               g_get_monotonic_time(),
                               error);
    return mux_ui_pane_bridge_repaint(
        bridge, g_get_monotonic_time(), error);
}

gboolean
mux_ui_pane_bridge_tick(MuxUiPaneBridge *bridge,
                        gint64 monotonic_us,
                        gboolean *resolved,
                        GError **error)
{
    MuxUiResponse *response = NULL;

    g_return_val_if_fail(bridge, FALSE);
    g_return_val_if_fail(resolved, FALSE);
    *resolved = mux_pane_overlay_tick(
        bridge->overlay, monotonic_us, &response);
    if (!*resolved)
        return TRUE;
    return finish_response(bridge, response, monotonic_us, error);
}

gboolean
mux_ui_pane_bridge_set_size(MuxUiPaneBridge *bridge,
                            guint columns,
                            guint rows,
                            GError **error)
{
    g_return_val_if_fail(bridge, FALSE);
    bridge->columns = columns;
    bridge->rows = rows;
    return mux_ui_pane_bridge_repaint(
        bridge, g_get_monotonic_time(), error);
}

gboolean
mux_ui_pane_bridge_is_active(const MuxUiPaneBridge *bridge)
{
    g_return_val_if_fail(bridge, FALSE);
    return mux_pane_overlay_is_active(bridge->overlay);
}

guint
mux_ui_pane_bridge_pending_count(const MuxUiPaneBridge *bridge)
{
    g_return_val_if_fail(bridge, 0);
    return mux_pane_overlay_pending_count(bridge->overlay);
}
