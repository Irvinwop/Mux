#include "mux-extension-router.h"

typedef struct {
    guint16 expected_channel;
    guint calls;
    guint destroys;
} HandlerProbe;

typedef struct {
    MuxExtensionRouter *router;
    guint16 channel;
    guint old_destroys;
    gboolean reregistered;
    HandlerProbe replacement;
    HandlerProbe nested;
} ReplacementContext;

typedef struct {
    MuxExtensionRouter *router;
    guint16 channel;
    guint old_destroys;
    gboolean recursively_unregistered;
    gboolean reregistered;
    HandlerProbe nested;
} UnregisterContext;

typedef struct {
    guint calls;
    guint destroys;
    gboolean continued_after_unregister;
} DispatchContext;

typedef struct {
    MuxExtensionRouter *external_router;
    guint output_calls;
    guint router_destroys;
    gboolean continued_after_unref;
} OutputDropContext;

static gboolean
discard_output(MuxExtensionRouter *router,
               GBytes *packet,
               gpointer user_data,
               GError **error)
{
    (void) router;
    (void) packet;
    (void) user_data;
    (void) error;
    return TRUE;
}

static GBytes *
make_packet(guint16 channel)
{
    static const guint8 contents[] = { 'p', 'a', 'y', 'l', 'o', 'a', 'd' };
    g_autoptr(GBytes) payload = g_bytes_new_static(contents,
                                                   sizeof(contents));
    g_autoptr(GError) error = NULL;
    MuxExtensionRecord record = {
        .channel = channel,
        .payload = payload
    };
    GBytes *packet;

    packet = mux_extension_record_encode(&record, &error);
    g_assert_no_error(error);
    g_assert_nonnull(packet);
    return packet;
}

static gboolean
unexpected_handler(MuxExtensionRouter *router,
                   guint16 channel,
                   GBytes *payload,
                   gpointer user_data,
                   GError **error)
{
    (void) router;
    (void) channel;
    (void) payload;
    (void) user_data;
    (void) error;
    g_assert_not_reached();
    return FALSE;
}

static gboolean
probe_handler(MuxExtensionRouter *router,
              guint16 channel,
              GBytes *payload,
              gpointer user_data,
              GError **error)
{
    HandlerProbe *probe = user_data;

    (void) router;
    (void) payload;
    (void) error;
    g_assert_cmpuint(channel, ==, probe->expected_channel);
    probe->calls++;
    return TRUE;
}

static void
probe_destroy(gpointer user_data)
{
    HandlerProbe *probe = user_data;

    probe->destroys++;
}

static void
replacement_old_destroy(gpointer user_data)
{
    ReplacementContext *context = user_data;
    g_autoptr(GError) error = NULL;

    context->old_destroys++;
    context->reregistered = mux_extension_router_set_handler(
        context->router,
        context->channel,
        probe_handler,
        &context->nested,
        probe_destroy,
        &error);
    g_assert_no_error(error);
    g_assert_true(context->reregistered);
}

static void
test_replacement_destroy_reregisters(void)
{
    ReplacementContext context = {
        .channel = MUX_EXTENSION_CHANNEL_DIAGNOSTIC,
        .replacement = {
            .expected_channel = MUX_EXTENSION_CHANNEL_DIAGNOSTIC
        },
        .nested = {
            .expected_channel = MUX_EXTENSION_CHANNEL_DIAGNOSTIC
        }
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) packet = NULL;

    context.router = mux_extension_router_new(discard_output, NULL, NULL);
    g_assert_nonnull(context.router);
    g_assert_true(mux_extension_router_set_handler(
        context.router,
        context.channel,
        unexpected_handler,
        &context,
        replacement_old_destroy,
        &error));
    g_assert_no_error(error);

    g_assert_true(mux_extension_router_set_handler(
        context.router,
        context.channel,
        probe_handler,
        &context.replacement,
        probe_destroy,
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(context.old_destroys, ==, 1);
    g_assert_true(context.reregistered);
    g_assert_cmpuint(context.replacement.calls, ==, 0);
    g_assert_cmpuint(context.replacement.destroys, ==, 1);
    g_assert_cmpuint(context.nested.destroys, ==, 0);

    packet = make_packet(context.channel);
    g_assert_true(mux_extension_router_dispatch(
        context.router,
        g_bytes_get_data(packet, NULL),
        g_bytes_get_size(packet),
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(context.nested.calls, ==, 1);

    mux_extension_router_unref(context.router);
    context.router = NULL;
    g_assert_cmpuint(context.nested.destroys, ==, 1);
}

static void
unregister_old_destroy(gpointer user_data)
{
    UnregisterContext *context = user_data;
    g_autoptr(GError) error = NULL;

    context->old_destroys++;
    context->recursively_unregistered = mux_extension_router_set_handler(
        context->router,
        context->channel,
        NULL,
        NULL,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_true(context->recursively_unregistered);

    context->reregistered = mux_extension_router_set_handler(
        context->router,
        context->channel,
        probe_handler,
        &context->nested,
        probe_destroy,
        &error);
    g_assert_no_error(error);
    g_assert_true(context->reregistered);
}

static void
test_unregister_reentrancy(void)
{
    UnregisterContext context = {
        .channel = MUX_EXTENSION_CHANNEL_UI,
        .nested = {
            .expected_channel = MUX_EXTENSION_CHANNEL_UI
        }
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) packet = NULL;

    context.router = mux_extension_router_new(discard_output, NULL, NULL);
    g_assert_nonnull(context.router);
    g_assert_true(mux_extension_router_set_handler(
        context.router,
        context.channel,
        unexpected_handler,
        &context,
        unregister_old_destroy,
        &error));
    g_assert_no_error(error);

    g_assert_true(mux_extension_router_set_handler(
        context.router,
        context.channel,
        NULL,
        NULL,
        NULL,
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(context.old_destroys, ==, 1);
    g_assert_true(context.recursively_unregistered);
    g_assert_true(context.reregistered);
    g_assert_cmpuint(context.nested.destroys, ==, 0);

    packet = make_packet(context.channel);
    g_assert_true(mux_extension_router_dispatch(
        context.router,
        g_bytes_get_data(packet, NULL),
        g_bytes_get_size(packet),
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(context.nested.calls, ==, 1);

    mux_extension_router_unref(context.router);
    context.router = NULL;
    g_assert_cmpuint(context.nested.destroys, ==, 1);
}

static void
dispatch_context_destroy(gpointer user_data)
{
    DispatchContext *context = user_data;

    context->destroys++;
}

static gboolean
self_unregistering_handler(MuxExtensionRouter *router,
                           guint16 channel,
                           GBytes *payload,
                           gpointer user_data,
                           GError **error)
{
    DispatchContext *context = user_data;

    (void) payload;
    context->calls++;
    g_assert_cmpuint(context->destroys, ==, 0);
    g_assert_true(mux_extension_router_set_handler(router,
                                                   channel,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   error));
    g_assert_cmpuint(context->destroys, ==, 0);
    context->continued_after_unregister = TRUE;
    return TRUE;
}

static void
test_dispatch_retains_handler(void)
{
    DispatchContext context = { 0 };
    g_autoptr(MuxExtensionRouter) router = NULL;
    g_autoptr(GBytes) packet = NULL;
    g_autoptr(GError) error = NULL;

    router = mux_extension_router_new(discard_output, NULL, NULL);
    g_assert_nonnull(router);
    g_assert_true(mux_extension_router_set_handler(
        router,
        MUX_EXTENSION_CHANNEL_CLIPBOARD,
        self_unregistering_handler,
        &context,
        dispatch_context_destroy,
        &error));
    g_assert_no_error(error);

    packet = make_packet(MUX_EXTENSION_CHANNEL_CLIPBOARD);
    g_assert_true(mux_extension_router_dispatch(
        router,
        g_bytes_get_data(packet, NULL),
        g_bytes_get_size(packet),
        &error));
    g_assert_no_error(error);
    g_assert_cmpuint(context.calls, ==, 1);
    g_assert_true(context.continued_after_unregister);
    g_assert_cmpuint(context.destroys, ==, 1);

    g_assert_false(mux_extension_router_dispatch(
        router,
        g_bytes_get_data(packet, NULL),
        g_bytes_get_size(packet),
        &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
output_context_destroy(gpointer user_data)
{
    OutputDropContext *context = user_data;

    context->router_destroys++;
}

static gboolean
output_drops_external_reference(MuxExtensionRouter *router,
                                GBytes *packet,
                                gpointer user_data,
                                GError **error)
{
    static const guint8 expected[] = { 's', 'e', 'n', 'd' };
    OutputDropContext *context = user_data;
    MuxExtensionRouter *external_router;
    MuxExtensionRecord record = { 0 };
    const guint8 *packet_data;
    const guint8 *payload_data;
    gsize packet_size;
    gsize payload_size;

    context->output_calls++;
    g_assert_true(router == context->external_router);
    external_router = context->external_router;
    context->external_router = NULL;
    mux_extension_router_unref(external_router);
    g_assert_cmpuint(context->router_destroys, ==, 0);

    packet_data = g_bytes_get_data(packet, &packet_size);
    if (!mux_extension_record_decode(packet_data,
                                     packet_size,
                                     &record,
                                     error))
        return FALSE;
    g_assert_cmpuint(record.channel,
                     ==,
                     MUX_EXTENSION_CHANNEL_DIAGNOSTIC);
    payload_data = g_bytes_get_data(record.payload, &payload_size);
    g_assert_cmpmem(payload_data,
                    payload_size,
                    expected,
                    sizeof(expected));
    mux_extension_record_clear(&record);

    context->continued_after_unref = TRUE;
    return TRUE;
}

static void
test_output_drops_final_external_reference(void)
{
    static const guint8 contents[] = { 's', 'e', 'n', 'd' };
    OutputDropContext context = { 0 };
    g_autoptr(GBytes) payload = g_bytes_new_static(contents,
                                                   sizeof(contents));
    g_autoptr(GError) error = NULL;
    MuxExtensionRouter *router;

    context.external_router = mux_extension_router_new(
        output_drops_external_reference,
        &context,
        output_context_destroy);
    g_assert_nonnull(context.external_router);
    router = context.external_router;

    g_assert_true(mux_extension_router_send(
        router,
        MUX_EXTENSION_CHANNEL_DIAGNOSTIC,
        payload,
        &error));
    g_assert_no_error(error);
    g_assert_null(context.external_router);
    g_assert_cmpuint(context.output_calls, ==, 1);
    g_assert_true(context.continued_after_unref);
    g_assert_cmpuint(context.router_destroys, ==, 1);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/extension-router/replacement/reentrant-register",
                    test_replacement_destroy_reregisters);
    g_test_add_func("/extension-router/unregister/reentrant",
                    test_unregister_reentrancy);
    g_test_add_func("/extension-router/dispatch/handler-lifetime",
                    test_dispatch_retains_handler);
    g_test_add_func("/extension-router/send/drop-final-external-reference",
                    test_output_drops_final_external_reference);
    return g_test_run();
}
