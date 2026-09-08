#include "mux-network-handshake.h"

#include <gio/gio.h>

static const gchar *tor_profile = "tor-private-abcdefghijkl";

static MuxNetworkPolicy *
tor_policy(guint port)
{
    g_autofree gchar *uri = g_strdup_printf("socks5://127.0.0.1:%u", port);
    g_autoptr(GError) error = NULL;
    MuxNetworkPolicy *policy = mux_network_policy_new("tor", uri, &error);

    g_assert_no_error(error);
    g_assert_nonnull(policy);
    return policy;
}

static void
test_profile_mode_fence(void)
{
    g_autoptr(MuxNetworkPolicy) direct = mux_network_policy_new(NULL, NULL, NULL);
    g_autoptr(MuxNetworkPolicy) tor = tor_policy(9050);
    g_autoptr(GError) error = NULL;
    const gchar *invalid[] = { "default", "tor-private-", "tor-private-123",
                              "tor-private-abcdefgh/../direct", "..", "" };

    g_assert_true(mux_network_handshake_check_profile(direct, "default", &error));
    g_assert_true(mux_network_handshake_check_profile(direct, "tor-user", &error));
    g_assert_true(mux_network_handshake_check_profile(tor, tor_profile, &error));
    g_assert_no_error(error);
    g_assert_false(mux_network_handshake_check_profile(direct, tor_profile, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_assert_false(mux_network_handshake_check_profile(tor, invalid[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
}

static void
test_identity_trailer(void)
{
    g_autoptr(MuxNetworkPolicy) direct = mux_network_policy_new(NULL, NULL, NULL);
    g_autoptr(MuxNetworkPolicy) tor = tor_policy(9050);
    g_autoptr(MuxNetworkPolicy) changed = tor_policy(9150);
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GBytes) empty = g_bytes_new(NULL, 0);
    g_autoptr(GError) error = NULL;
    MuxEngineBuilder builder;
    MuxEngineCursor cursor;

    mux_engine_cursor_init(&cursor, empty);
    g_assert_true(mux_network_handshake_check_trailer(&cursor, direct, "default", &error));
    mux_engine_cursor_init(&cursor, empty);
    g_assert_false(mux_network_handshake_check_trailer(&cursor, tor, tor_profile, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    mux_engine_builder_init(&builder);
    mux_network_handshake_append(&builder, tor, tor_profile);
    payload = mux_engine_builder_finish(&builder);
    mux_engine_cursor_init(&cursor, payload);
    g_assert_true(mux_network_handshake_check_trailer(&cursor, tor, tor_profile, &error));
    g_assert_no_error(error);
    mux_engine_cursor_init(&cursor, payload);
    g_assert_false(mux_network_handshake_check_trailer(&cursor, changed, tor_profile, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    mux_engine_cursor_init(&cursor, payload);
    g_assert_false(mux_network_handshake_check_trailer(&cursor, direct, tor_profile, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    mux_engine_cursor_init(&cursor, payload);
    g_assert_false(mux_network_handshake_check_trailer(&cursor, tor,
                                                      "tor-private-otherlaunch", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

static void
test_truncated_and_extra_trailer(void)
{
    g_autoptr(MuxNetworkPolicy) tor = tor_policy(9050);
    g_autoptr(GError) error = NULL;

    for (guint extra = 0; extra < 2; extra++) {
        MuxEngineBuilder builder;
        MuxEngineCursor cursor;
        g_autoptr(GBytes) payload = NULL;

        mux_engine_builder_init(&builder);
        if (extra) {
            mux_network_handshake_append(&builder, tor, tor_profile);
            mux_engine_builder_put_u32(&builder, 123);
        } else {
            mux_engine_builder_put_string(&builder, tor_profile);
        }
        payload = mux_engine_builder_finish(&builder);
        mux_engine_cursor_init(&cursor, payload);
        g_assert_false(mux_network_handshake_check_trailer(&cursor, tor,
                                                          tor_profile, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_clear_error(&error);
    }
}

static GBytes *
welcome(const MuxNetworkPolicy *policy,
        const gchar *profile,
        const gchar *socket_path,
        gboolean trailer)
{
    MuxEngineBuilder builder;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, 123);
    mux_engine_builder_put_string(&builder, profile);
    mux_engine_builder_put_string(&builder, socket_path);
    if (trailer)
        mux_network_handshake_append(&builder, policy, profile);
    return mux_engine_builder_finish(&builder);
}

static void
test_welcome_reuse_fence(void)
{
    g_autoptr(MuxNetworkPolicy) direct = mux_network_policy_new(NULL, NULL, NULL);
    g_autoptr(MuxNetworkPolicy) tor = tor_policy(9050);
    g_autoptr(MuxNetworkPolicy) changed = tor_policy(9150);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) legacy = welcome(direct, "default", "/tmp/direct.sock", FALSE);
    g_autoptr(GBytes) missing = welcome(tor, tor_profile, "/tmp/tor.sock", FALSE);
    g_autoptr(GBytes) attested = welcome(tor, tor_profile, "/tmp/tor.sock", TRUE);

    g_assert_true(mux_network_handshake_check_welcome(legacy, direct,
                                                      "default", "/tmp/direct.sock", &error));
    g_assert_no_error(error);
    g_assert_false(mux_network_handshake_check_welcome(legacy, tor,
                                                       tor_profile, "/tmp/direct.sock", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_false(mux_network_handshake_check_welcome(missing, tor,
                                                       tor_profile, "/tmp/tor.sock", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_true(mux_network_handshake_check_welcome(attested, tor,
                                                      tor_profile, "/tmp/tor.sock", &error));
    g_assert_no_error(error);
    g_assert_false(mux_network_handshake_check_welcome(attested, tor,
                                                       tor_profile, "/tmp/direct.sock", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_false(mux_network_handshake_check_welcome(attested, changed,
                                                       tor_profile, "/tmp/tor.sock", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

typedef struct {
    GSocketListener *listener;
    guint8 response[2];
    gsize response_length;
} SocksFixture;

static gpointer
socks_server(gpointer data)
{
    SocksFixture *fixture = data;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    guint8 greeting[3] = { 0 };
    const guint8 expected[] = { 5, 1, 0 };
    gsize count = 0;

    connection = g_socket_listener_accept(fixture->listener, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(g_input_stream_read_all(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)),
        greeting, sizeof(greeting), &count, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(greeting, count, expected, sizeof(expected));
    g_assert_true(g_output_stream_write_all(
        g_io_stream_get_output_stream(G_IO_STREAM(connection)),
        fixture->response, fixture->response_length, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_io_stream_close(G_IO_STREAM(connection), NULL, &error));
    g_assert_no_error(error);
    return NULL;
}

static void
test_socks_preflight(void)
{
    const guint8 replies[][2] = { { 5, 0 }, { 5, 2 }, { 'H', 'T' }, { 5, 0 } };
    g_autoptr(MuxNetworkPolicy) direct = mux_network_policy_new(NULL, NULL, NULL);

    g_assert_true(mux_network_handshake_probe_socks(direct, NULL));
    for (guint i = 0; i < G_N_ELEMENTS(replies); i++) {
        g_autoptr(GSocketListener) listener = g_socket_listener_new();
        g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
        g_autoptr(GSocketAddress) bind_address = g_inet_socket_address_new(loopback, 0);
        g_autoptr(GSocketAddress) effective = NULL;
        g_autoptr(MuxNetworkPolicy) policy = NULL;
        g_autoptr(GError) error = NULL;
        SocksFixture fixture = { .listener = listener,
                                 .response_length = i == 3 ? 1 : 2 };
        GThread *thread;

        fixture.response[0] = replies[i][0];
        fixture.response[1] = replies[i][1];
        g_assert_true(g_socket_listener_add_address(listener, bind_address,
                                                     G_SOCKET_TYPE_STREAM,
                                                     G_SOCKET_PROTOCOL_TCP,
                                                     NULL, &effective, &error));
        g_assert_no_error(error);
        policy = tor_policy(g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective)));
        thread = g_thread_new("socks-fixture", socks_server, &fixture);
        if (i == 0) {
            g_assert_true(mux_network_handshake_probe_socks(policy, &error));
            g_assert_no_error(error);
        } else {
            g_assert_false(mux_network_handshake_probe_socks(policy, &error));
            g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        }
        g_thread_join(thread);
    }
}

static void
test_socks_refused_is_not_direct(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) reserved = g_socket_new(G_SOCKET_FAMILY_IPV4,
                                               G_SOCKET_TYPE_STREAM,
                                               G_SOCKET_PROTOCOL_TCP,
                                               &error);
    g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    g_autoptr(GSocketAddress) bind_address = g_inet_socket_address_new(loopback, 0);
    g_autoptr(GSocketAddress) effective = NULL;
    g_autoptr(MuxNetworkPolicy) policy = NULL;

    g_assert_no_error(error);
    g_assert_true(g_socket_bind(reserved, bind_address, FALSE, &error));
    g_assert_no_error(error);
    effective = g_socket_get_local_address(reserved, &error);
    g_assert_no_error(error);
    policy = tor_policy(g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective)));
    g_assert_false(mux_network_handshake_probe_socks(policy, &error));
    g_assert_nonnull(error);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/network-handshake/profile-mode", test_profile_mode_fence);
    g_test_add_func("/network-handshake/identity-trailer", test_identity_trailer);
    g_test_add_func("/network-handshake/trailer-framing", test_truncated_and_extra_trailer);
    g_test_add_func("/network-handshake/welcome-reuse", test_welcome_reuse_fence);
    g_test_add_func("/network-handshake/socks-preflight", test_socks_preflight);
    g_test_add_func("/network-handshake/socks-refused", test_socks_refused_is_not_direct);
    return g_test_run();
}
