#include "muxd-clipboard.h"

#include "mux-clipboard-broker-bindings.h"
#include "mux-local-source.h"

#include <unistd.h>

#define MUXD_CLIPBOARD_PEER_SWEEP_MS 1000u

struct _MuxdClipboard {
    GMainContext *context;
    MuxClipboardBroker *broker;
    MuxLocalListener *listener;
    GSource *listener_source;
    GSource *sweep_source;
    GPtrArray *peers;
};

static gboolean
accept_peer(MuxLocalListener *listener,
            MuxLocalConnection *connection,
            gpointer user_data,
            GError **error)
{
    MuxdClipboard *clipboard = user_data;
    MuxClipboardBrokerTransport *transport;

    (void) listener;
    transport = mux_clipboard_broker_peer_transport_new(connection,
                                                        clipboard->broker,
                                                        clipboard->context,
                                                        error);
    if (transport == NULL)
        return FALSE;
    g_ptr_array_add(clipboard->peers, transport);
    return TRUE;
}

static void
listener_failure(MuxLocalListener *listener,
                 const GError *error,
                 gpointer user_data)
{
    (void) listener;
    (void) user_data;
    g_warning("clipboard listener: %s",
              error != NULL ? error->message : "unspecified failure");
}

static gboolean
sweep_closed_peers(gpointer user_data)
{
    MuxdClipboard *clipboard = user_data;
    gint index;

    for (index = (gint) clipboard->peers->len - 1; index >= 0; index--) {
        MuxClipboardBrokerTransport *transport =
            g_ptr_array_index(clipboard->peers, (guint) index);
        if (!mux_clipboard_broker_transport_is_open(transport))
            g_ptr_array_remove_index(clipboard->peers, (guint) index);
    }
    return G_SOURCE_CONTINUE;
}

MuxdClipboard *
muxd_clipboard_new(GMainContext *context, GError **error)
{
    MuxdClipboard *clipboard;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (context == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "muxd clipboard requires a main context");
        return NULL;
    }

    clipboard = g_new0(MuxdClipboard, 1);
    clipboard->context = g_main_context_ref(context);
    clipboard->broker = mux_clipboard_broker_new();
    clipboard->peers = g_ptr_array_new_with_free_func(
        (GDestroyNotify) mux_clipboard_broker_transport_unref);
    clipboard->listener = mux_local_listener_new(
        MUX_CLIPBOARD_BROKER_SERVICE,
        64,
        error);
    if (clipboard->listener == NULL) {
        muxd_clipboard_free(clipboard);
        return NULL;
    }

    clipboard->listener_source = mux_local_listener_source_new(
        clipboard->listener,
        geteuid(),
        MUX_LOCAL_TRANSPORT_DEFAULT_MAX_PACKET,
        MUX_LOCAL_TRANSPORT_DEFAULT_QUEUE_LIMIT,
        accept_peer,
        listener_failure,
        clipboard,
        NULL);
    if (clipboard->listener_source == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "failed to create clipboard listener source");
        muxd_clipboard_free(clipboard);
        return NULL;
    }
    g_source_attach(clipboard->listener_source, clipboard->context);

    clipboard->sweep_source =
        g_timeout_source_new(MUXD_CLIPBOARD_PEER_SWEEP_MS);
    g_source_set_callback(clipboard->sweep_source,
                          sweep_closed_peers,
                          clipboard,
                          NULL);
    g_source_set_name(clipboard->sweep_source,
                      "muxd-clipboard-peer-sweep");
    g_source_attach(clipboard->sweep_source, clipboard->context);
    return clipboard;
}

void
muxd_clipboard_free(MuxdClipboard *clipboard)
{
    if (clipboard == NULL)
        return;

    if (clipboard->sweep_source != NULL) {
        g_source_destroy(clipboard->sweep_source);
        g_source_unref(clipboard->sweep_source);
    }
    if (clipboard->listener_source != NULL) {
        g_source_destroy(clipboard->listener_source);
        g_source_unref(clipboard->listener_source);
    }
    g_clear_pointer(&clipboard->peers, g_ptr_array_unref);
    g_clear_pointer(&clipboard->listener, mux_local_listener_unref);
    g_clear_pointer(&clipboard->broker, mux_clipboard_broker_free);
    g_clear_pointer(&clipboard->context, g_main_context_unref);
    g_free(clipboard);
}
