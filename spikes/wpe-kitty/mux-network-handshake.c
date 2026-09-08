#include "mux-network-handshake.h"

#include <gio/gio.h>
#include <string.h>

gboolean
mux_network_handshake_check_profile(const MuxNetworkPolicy *policy,
                                     const gchar *profile,
                                     GError **error)
{
    gboolean tor = mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_TOR;
    gboolean reserved;
    gsize length;

    if (!profile || !*profile || g_str_equal(profile, ".") ||
        g_str_equal(profile, "..") || strlen(profile) > 64)
        goto invalid;
    length = strlen(profile);
    for (gsize i = 0; i < length; i++) {
        if (!g_ascii_isalnum(profile[i]) && profile[i] != '-' &&
            profile[i] != '_' && profile[i] != '.')
            goto invalid;
    }
    reserved = g_str_has_prefix(profile, MUX_TOR_PROFILE_PREFIX);
    if (tor != reserved ||
        (tor && length < strlen(MUX_TOR_PROFILE_PREFIX) + 8))
        goto invalid;
    return TRUE;

invalid:
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Tor requires an isolated tor-private-* profile; "
                        "that namespace cannot be used in direct mode "
                        "(launch with mux --tor)");
    return FALSE;
}

void
mux_network_handshake_append(MuxEngineBuilder *builder,
                              const MuxNetworkPolicy *policy,
                              const gchar *profile)
{
    mux_engine_builder_put_string(builder, profile);
    mux_engine_builder_put_string(builder,
                                  mux_network_policy_get_identity(policy));
}

gboolean
mux_network_handshake_check_trailer(MuxEngineCursor *cursor,
                                     const MuxNetworkPolicy *policy,
                                     const gchar *profile,
                                     GError **error)
{
    g_autofree gchar *received_profile = NULL;
    g_autofree gchar *identity = NULL;

    if (mux_engine_cursor_done(cursor) &&
        mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_DIRECT)
        return TRUE;
    if (mux_engine_cursor_get_string(cursor, &received_profile) &&
        mux_engine_cursor_get_string(cursor, &identity) &&
        mux_engine_cursor_done(cursor) &&
        g_strcmp0(received_profile, profile) == 0 &&
        g_strcmp0(identity, mux_network_policy_get_identity(policy)) == 0)
        return TRUE;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_PERMISSION_DENIED,
                        "engine profile or network policy attestation is "
                        "missing or mismatched; refusing to browse");
    return FALSE;
}

gboolean
mux_network_handshake_check_welcome(GBytes *payload,
                                     const MuxNetworkPolicy *policy,
                                     const gchar *profile,
                                     const gchar *socket_path,
                                     GError **error)
{
    MuxEngineCursor cursor;
    guint32 pid;
    g_autofree gchar *received_profile = NULL;
    g_autofree gchar *received_socket = NULL;

    mux_engine_cursor_init(&cursor, payload);
    if (!mux_engine_cursor_get_u32(&cursor, &pid) || !pid ||
        !mux_engine_cursor_get_string(&cursor, &received_profile) ||
        !mux_engine_cursor_get_string(&cursor, &received_socket) ||
        g_strcmp0(received_profile, profile) != 0 ||
        g_strcmp0(received_socket, socket_path) != 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "engine WELCOME has a different profile or socket; "
                            "refusing to browse");
        return FALSE;
    }
    return mux_network_handshake_check_trailer(&cursor,
                                                policy,
                                                profile,
                                                error);
}

gboolean
mux_network_handshake_probe_socks(const MuxNetworkPolicy *policy,
                                  GError **error)
{
    static const guint8 greeting[] = { 5, 1, 0 };
    g_autoptr(GUri) uri = NULL;
    g_autoptr(GInetAddress) host = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketClient) client = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    guint8 reply[2] = { 0 };
    gsize count = 0;

    if (mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_DIRECT)
        return TRUE;
    uri = g_uri_parse(mux_network_policy_get_proxy_uri(policy),
                       G_URI_FLAGS_NONE,
                       error);
    if (!uri)
        return FALSE;
    host = g_inet_address_new_from_string(g_uri_get_host(uri));
    if (!host || !g_inet_address_get_is_loopback(host)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Tor SOCKS preflight requires numeric loopback");
        return FALSE;
    }
    address = g_inet_socket_address_new(host, (guint16)g_uri_get_port(uri));
    client = g_socket_client_new();
    g_socket_client_set_enable_proxy(client, FALSE);
    g_socket_client_set_timeout(client, 3);
    connection = g_socket_client_connect(client,
                                          G_SOCKET_CONNECTABLE(address),
                                          NULL,
                                          error);
    if (!connection)
        return FALSE;
    if (!g_output_stream_write_all(
            g_io_stream_get_output_stream(G_IO_STREAM(connection)),
            greeting, sizeof(greeting), NULL, NULL, error) ||
        !g_input_stream_read_all(
            g_io_stream_get_input_stream(G_IO_STREAM(connection)),
            reply, sizeof(reply), &count, NULL, error))
        return FALSE;
    if (count != sizeof(reply) || reply[0] != 5 || reply[1] != 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Tor endpoint did not accept SOCKS5 without "
                            "authentication; refusing to browse");
        return FALSE;
    }
    return TRUE;
}
