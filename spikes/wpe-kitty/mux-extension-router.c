#include "mux-extension-router.h"

typedef struct {
    gint reference_count;
    MuxExtensionHandlerFunc handler_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
} ChannelHandler;

struct _MuxExtensionRouter {
    gint reference_count;
    MuxExtensionOutputFunc output_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    ChannelHandler *handlers[MUX_EXTENSION_MAX_CHANNEL + 1];
};

static ChannelHandler *
channel_handler_ref(ChannelHandler *handler)
{
    g_atomic_int_inc(&handler->reference_count);
    return handler;
}

static void
channel_handler_unref(ChannelHandler *handler)
{
    if (handler == NULL ||
        !g_atomic_int_dec_and_test(&handler->reference_count))
        return;
    if (handler->user_data_destroy != NULL)
        handler->user_data_destroy(handler->user_data);
    g_free(handler);
}

MuxExtensionRouter *
mux_extension_router_new(MuxExtensionOutputFunc output_func,
                         gpointer user_data,
                         GDestroyNotify user_data_destroy)
{
    MuxExtensionRouter *router;

    g_return_val_if_fail(output_func != NULL, NULL);
    router = g_new0(MuxExtensionRouter, 1);
    router->reference_count = 1;
    router->output_func = output_func;
    router->user_data = user_data;
    router->user_data_destroy = user_data_destroy;
    return router;
}

MuxExtensionRouter *
mux_extension_router_ref(MuxExtensionRouter *router)
{
    g_return_val_if_fail(router != NULL, NULL);
    g_atomic_int_inc(&router->reference_count);
    return router;
}

void
mux_extension_router_unref(MuxExtensionRouter *router)
{
    guint i;

    if (router == NULL ||
        !g_atomic_int_dec_and_test(&router->reference_count))
        return;

    for (i = 1; i <= MUX_EXTENSION_MAX_CHANNEL; i++)
        channel_handler_unref(router->handlers[i]);
    if (router->user_data_destroy != NULL)
        router->user_data_destroy(router->user_data);
    g_free(router);
}

gboolean
mux_extension_router_set_handler(MuxExtensionRouter *router,
                                 guint16 channel,
                                 MuxExtensionHandlerFunc handler_func,
                                 gpointer user_data,
                                 GDestroyNotify user_data_destroy,
                                 GError **error)
{
    ChannelHandler *handler = NULL;
    ChannelHandler *previous;

    g_return_val_if_fail(router != NULL, FALSE);
    if (channel == 0 || channel > MUX_EXTENSION_MAX_CHANNEL) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "extension handler channel is invalid");
        return FALSE;
    }

    if (handler_func != NULL) {
        handler = g_new0(ChannelHandler, 1);
        handler->reference_count = 1;
        handler->handler_func = handler_func;
        handler->user_data = user_data;
        handler->user_data_destroy = user_data_destroy;
    } else if (user_data_destroy != NULL) {
        user_data_destroy(user_data);
    }

    previous = router->handlers[channel];
    router->handlers[channel] = handler;
    channel_handler_unref(previous);
    return TRUE;
}

gboolean
mux_extension_router_send(MuxExtensionRouter *router,
                          guint16 channel,
                          GBytes *payload,
                          GError **error)
{
    MuxExtensionRouter *guard;
    MuxExtensionRecord record = {
        .channel = channel,
        .payload = payload
    };
    GBytes *packet;
    gboolean result;

    g_return_val_if_fail(router != NULL, FALSE);
    g_return_val_if_fail(payload != NULL, FALSE);
    guard = mux_extension_router_ref(router);
    packet = mux_extension_record_encode(&record, error);
    if (packet == NULL) {
        mux_extension_router_unref(guard);
        return FALSE;
    }

    result = router->output_func(router,
                                 packet,
                                 router->user_data,
                                 error);
    if (!result && error != NULL && *error == NULL)
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "extension output rejected a packet");
    g_bytes_unref(packet);
    mux_extension_router_unref(guard);
    return result;
}

gboolean
mux_extension_router_dispatch(MuxExtensionRouter *router,
                              const guint8 *packet,
                              gsize packet_length,
                              GError **error)
{
    MuxExtensionRouter *guard;
    MuxExtensionRecord record = { 0 };
    ChannelHandler *handler;
    gboolean result;

    g_return_val_if_fail(router != NULL, FALSE);
    guard = mux_extension_router_ref(router);
    if (!mux_extension_record_decode(packet,
                                     packet_length,
                                     &record,
                                     error)) {
        mux_extension_router_unref(guard);
        return FALSE;
    }

    handler = router->handlers[record.channel];
    if (handler == NULL) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "extension channel %u has no handler",
                    record.channel);
        mux_extension_record_clear(&record);
        mux_extension_router_unref(guard);
        return FALSE;
    }

    channel_handler_ref(handler);
    result = handler->handler_func(router,
                                   record.channel,
                                   record.payload,
                                   handler->user_data,
                                   error);
    channel_handler_unref(handler);
    mux_extension_record_clear(&record);
    mux_extension_router_unref(guard);
    return result;
}
