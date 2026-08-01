#include "mux-local-source.h"

typedef struct {
    GSource source;
    MuxLocalConnection *connection;
    GPollFD poll_fd;
    MuxLocalPacketFunc packet_func;
    MuxLocalConnectionEndFunc end_func;
    gpointer user_data;
    GDestroyNotify destroy_notify;
} MuxLocalConnectionSource;

typedef struct {
    GSource source;
    MuxLocalListener *listener;
    GPollFD poll_fd;
    uid_t expected_uid;
    gsize max_packet;
    gsize queue_limit;
    MuxLocalAcceptFunc accept_func;
    MuxLocalListenerFailureFunc failure_func;
    gpointer user_data;
    GDestroyNotify destroy_notify;
} MuxLocalListenerSource;

static gboolean
connection_source_prepare(GSource *base, gint *timeout_ms)
{
    MuxLocalConnectionSource *source = (MuxLocalConnectionSource *) base;

    source->poll_fd.events = (gushort)
        mux_local_connection_wanted_condition(source->connection);
    *timeout_ms = -1;
    return FALSE;
}

static gboolean
connection_source_check(GSource *base)
{
    MuxLocalConnectionSource *source = (MuxLocalConnectionSource *) base;
    return source->poll_fd.revents != 0;
}

static gboolean
connection_source_dispatch(GSource *base,
                           GSourceFunc ignored_callback,
                           gpointer ignored_user_data)
{
    MuxLocalConnectionSource *source = (MuxLocalConnectionSource *) base;
    g_autoptr(GError) error = NULL;
    GIOCondition condition = (GIOCondition) source->poll_fd.revents;
    MuxLocalDispatchResult result;

    (void) ignored_callback;
    (void) ignored_user_data;

    source->poll_fd.revents = 0;
    g_source_set_ready_time(base, -1);
    result = mux_local_connection_dispatch(source->connection,
                                           condition,
                                           source->packet_func,
                                           source->user_data,
                                           &error);
    source->poll_fd.events = (gushort)
        mux_local_connection_wanted_condition(source->connection);

    if (result == MUX_LOCAL_DISPATCH_OK)
        return G_SOURCE_CONTINUE;

    if (source->end_func != NULL)
        source->end_func(source->connection,
                         result,
                         error,
                         source->user_data);
    return G_SOURCE_REMOVE;
}

static void
connection_source_finalize(GSource *base)
{
    MuxLocalConnectionSource *source = (MuxLocalConnectionSource *) base;

    mux_local_connection_unref(source->connection);
    if (source->destroy_notify != NULL)
        source->destroy_notify(source->user_data);
}

static GSourceFuncs connection_source_funcs = {
    .prepare = connection_source_prepare,
    .check = connection_source_check,
    .dispatch = connection_source_dispatch,
    .finalize = connection_source_finalize,
};

GSource *
mux_local_connection_source_new(MuxLocalConnection *connection,
                                MuxLocalPacketFunc packet_func,
                                MuxLocalConnectionEndFunc end_func,
                                gpointer user_data,
                                GDestroyNotify destroy_notify)
{
    GSource *base;
    MuxLocalConnectionSource *source;

    g_return_val_if_fail(connection != NULL, NULL);
    g_return_val_if_fail(packet_func != NULL, NULL);

    base = g_source_new(&connection_source_funcs,
                        sizeof(MuxLocalConnectionSource));
    source = (MuxLocalConnectionSource *) base;
    source->connection = mux_local_connection_ref(connection);
    source->packet_func = packet_func;
    source->end_func = end_func;
    source->user_data = user_data;
    source->destroy_notify = destroy_notify;
    source->poll_fd.fd = mux_local_connection_get_fd(connection);
    source->poll_fd.events = (gushort)
        mux_local_connection_wanted_condition(connection);
    source->poll_fd.revents = 0;
    g_source_add_poll(base, &source->poll_fd);
    g_source_set_name(base, "mux-local-connection");
    return base;
}

gboolean
mux_local_connection_source_queue(GSource *base,
                                  GBytes *packet,
                                  GError **error)
{
    MuxLocalConnectionSource *source;
    GMainContext *context;

    g_return_val_if_fail(base != NULL, FALSE);
    g_return_val_if_fail(packet != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    source = (MuxLocalConnectionSource *) base;
    if (!mux_local_connection_queue(source->connection, packet, error))
        return FALSE;

    source->poll_fd.events = (gushort)
        mux_local_connection_wanted_condition(source->connection);
    g_source_set_ready_time(base, 0);
    context = g_source_get_context(base);
    if (context != NULL)
        g_main_context_wakeup(context);
    return TRUE;
}

static gboolean
listener_source_prepare(GSource *base, gint *timeout_ms)
{
    (void) base;
    *timeout_ms = -1;
    return FALSE;
}

static gboolean
listener_source_check(GSource *base)
{
    MuxLocalListenerSource *source = (MuxLocalListenerSource *) base;
    return source->poll_fd.revents != 0;
}

static gboolean
listener_source_dispatch(GSource *base,
                         GSourceFunc ignored_callback,
                         gpointer ignored_user_data)
{
    MuxLocalListenerSource *source = (MuxLocalListenerSource *) base;

    (void) ignored_callback;
    (void) ignored_user_data;

    if ((source->poll_fd.revents &
         (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        g_autoptr(GError) error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_CLOSED,
            "local listener became unavailable");

        source->poll_fd.revents = 0;
        if (source->failure_func != NULL)
            source->failure_func(source->listener,
                                 error,
                                 source->user_data);
        return G_SOURCE_REMOVE;
    }

    source->poll_fd.revents = 0;
    for (;;) {
        g_autoptr(GError) error = NULL;
        gboolean would_block = FALSE;
        MuxLocalConnection *connection = mux_local_listener_accept(
            source->listener,
            source->expected_uid,
            source->max_packet,
            source->queue_limit,
            &would_block,
            &error);

        if (connection == NULL) {
            if (would_block)
                return G_SOURCE_CONTINUE;

            if (source->failure_func != NULL)
                source->failure_func(source->listener,
                                     error,
                                     source->user_data);

            if (g_error_matches(error,
                                G_IO_ERROR,
                                G_IO_ERROR_PERMISSION_DENIED))
                continue;
            return G_SOURCE_REMOVE;
        }

        if (!source->accept_func(source->listener,
                                 connection,
                                 source->user_data,
                                 &error) &&
            source->failure_func != NULL) {
            if (error == NULL)
                error = g_error_new_literal(G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            "local peer setup was rejected");
            source->failure_func(source->listener,
                                 error,
                                 source->user_data);
        }
        mux_local_connection_unref(connection);
    }
}

static void
listener_source_finalize(GSource *base)
{
    MuxLocalListenerSource *source = (MuxLocalListenerSource *) base;

    mux_local_listener_unref(source->listener);
    if (source->destroy_notify != NULL)
        source->destroy_notify(source->user_data);
}

static GSourceFuncs listener_source_funcs = {
    .prepare = listener_source_prepare,
    .check = listener_source_check,
    .dispatch = listener_source_dispatch,
    .finalize = listener_source_finalize,
};

GSource *
mux_local_listener_source_new(MuxLocalListener *listener,
                              uid_t expected_uid,
                              gsize max_packet,
                              gsize queue_limit,
                              MuxLocalAcceptFunc accept_func,
                              MuxLocalListenerFailureFunc failure_func,
                              gpointer user_data,
                              GDestroyNotify destroy_notify)
{
    GSource *base;
    MuxLocalListenerSource *source;

    g_return_val_if_fail(listener != NULL, NULL);
    g_return_val_if_fail(max_packet > 0, NULL);
    g_return_val_if_fail(queue_limit >= max_packet, NULL);
    g_return_val_if_fail(accept_func != NULL, NULL);

    base = g_source_new(&listener_source_funcs,
                        sizeof(MuxLocalListenerSource));
    source = (MuxLocalListenerSource *) base;
    source->listener = mux_local_listener_ref(listener);
    source->expected_uid = expected_uid;
    source->max_packet = max_packet;
    source->queue_limit = queue_limit;
    source->accept_func = accept_func;
    source->failure_func = failure_func;
    source->user_data = user_data;
    source->destroy_notify = destroy_notify;
    source->poll_fd.fd = mux_local_listener_get_fd(listener);
    source->poll_fd.events = G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
    source->poll_fd.revents = 0;
    g_source_add_poll(base, &source->poll_fd);
    g_source_set_name(base, "mux-local-listener");
    return base;
}
