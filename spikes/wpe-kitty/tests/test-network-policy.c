#include "../mux-network-policy.h"
#include "../mux-privacy-policy.h"

#include <gio/gio.h>
#include <string.h>

#ifdef MUX_NETWORK_POLICY_TEST_WPE
#include "../mux-network-policy-wpe.h"
#endif

typedef struct {
    const gchar *name;
    const gchar *uri;
} InvalidProxy;

static void
test_direct_defaults(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxNetworkPolicy) implicit =
        mux_network_policy_new(NULL, NULL, &error);
    g_autoptr(MuxNetworkPolicy) explicit = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(implicit);
    explicit = mux_network_policy_new("direct", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(explicit);
    g_assert_cmpint(mux_network_policy_get_mode(implicit),
                    ==, MUX_NETWORK_MODE_DIRECT);
    g_assert_null(mux_network_policy_get_proxy_uri(implicit));
    g_assert_false(mux_network_policy_is_ephemeral(implicit, FALSE));
    g_assert_true(mux_network_policy_is_ephemeral(implicit, TRUE));
    g_assert_cmpstr(mux_network_policy_get_identity(implicit),
                    ==, "mux-network-v1-direct");
    g_assert_cmpstr(mux_network_policy_get_identity(implicit),
                    ==, mux_network_policy_get_identity(explicit));
}

static void
test_direct_rejects_proxy(void)
{
    const gchar *modes[] = { NULL, "direct" };
    const gchar *proxies[] = { "", "socks5://127.0.0.1:9050" };
    guint i;
    guint j;

    for (i = 0; i < G_N_ELEMENTS(modes); i++) {
        for (j = 0; j < G_N_ELEMENTS(proxies); j++) {
            g_autoptr(GError) error = NULL;
            g_autoptr(MuxNetworkPolicy) policy =
                mux_network_policy_new(modes[i], proxies[j], &error);

            g_assert_null(policy);
            g_assert_error(error, MUX_NETWORK_POLICY_ERROR,
                            MUX_NETWORK_POLICY_ERROR_INVALID_PROXY);
        }
    }
}

static void
test_invalid_mode(void)
{
    const gchar *modes[] = { "", "TOR", "Direct", "auto", " tor", "tor " };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(modes); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MuxNetworkPolicy) policy =
            mux_network_policy_new(modes[i], NULL, &error);

        g_assert_null(policy);
        g_assert_error(error, MUX_NETWORK_POLICY_ERROR,
                        MUX_NETWORK_POLICY_ERROR_INVALID_MODE);
    }
}

static void
test_tor_loopback_endpoints(void)
{
    const struct {
        const gchar *input;
        const gchar *canonical;
    } endpoints[] = {
        { "socks5://127.0.0.1:9050", "socks5://127.0.0.1:9050" },
        { "socks5://127.0.0.2:1", "socks5://127.0.0.2:1" },
        { "socks5://127.255.255.254:65535", "socks5://127.255.255.254:65535" },
        { "socks5://[::1]:9150", "socks5://[::1]:9150" },
        { "SOCKS5://[0:0:0:0:0:0:0:1]:9050", "socks5://[::1]:9050" },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(endpoints); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(MuxNetworkPolicy) policy = mux_network_policy_new(
            "tor", endpoints[i].input, &error);

        g_assert_no_error(error);
        g_assert_nonnull(policy);
        g_assert_cmpint(mux_network_policy_get_mode(policy),
                        ==, MUX_NETWORK_MODE_TOR);
        g_assert_cmpstr(mux_network_policy_get_proxy_uri(policy),
                        ==, endpoints[i].canonical);
        g_assert_true(mux_network_policy_is_ephemeral(policy, FALSE));
        g_assert_true(mux_network_policy_is_ephemeral(policy, TRUE));
    }
}

static void
test_invalid_proxy(gconstpointer data)
{
    const InvalidProxy *invalid = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new("tor", invalid->uri, &error);

    g_assert_null(policy);
    g_assert_error(error, MUX_NETWORK_POLICY_ERROR,
                    MUX_NETWORK_POLICY_ERROR_INVALID_PROXY);
}

static void
test_policy_identity(void)
{
    g_autoptr(MuxNetworkPolicy) direct =
        mux_network_policy_new("direct", NULL, NULL);
    g_autoptr(MuxNetworkPolicy) ipv4 =
        mux_network_policy_new("tor", "socks5://127.0.0.1:9050", NULL);
    g_autoptr(MuxNetworkPolicy) ipv6 =
        mux_network_policy_new("tor", "socks5://[::1]:9050", NULL);
    g_autoptr(MuxNetworkPolicy) equivalent = mux_network_policy_new(
        "tor", "SOCKS5://[0:0:0:0:0:0:0:1]:9050", NULL);
    g_autoptr(MuxNetworkPolicy) other_port =
        mux_network_policy_new("tor", "socks5://[::1]:9150", NULL);

    g_assert_nonnull(direct);
    g_assert_nonnull(ipv4);
    g_assert_nonnull(ipv6);
    g_assert_nonnull(equivalent);
    g_assert_nonnull(other_port);
    g_assert_true(g_str_has_prefix(mux_network_policy_get_identity(ipv6),
                                   "mux-network-v2-tor-"
                                   MUX_TOR_PRIVACY_POLICY_ID "-"));
    g_assert_cmpstr(mux_network_policy_get_identity(ipv6),
                    ==, mux_network_policy_get_identity(equivalent));
    g_assert_cmpstr(mux_network_policy_get_identity(direct),
                    !=, mux_network_policy_get_identity(ipv4));
    g_assert_cmpstr(mux_network_policy_get_identity(ipv4),
                    !=, mux_network_policy_get_identity(ipv6));
    g_assert_cmpstr(mux_network_policy_get_identity(ipv6),
                    !=, mux_network_policy_get_identity(other_port));
}

static void
test_policy_owns_input(void)
{
    g_autofree gchar *mode = g_strdup("tor");
    g_autofree gchar *uri = g_strdup("socks5://127.0.0.1:9050");
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new(mode, uri, &error);
    g_autofree gchar *identity = NULL;

    g_assert_no_error(error);
    g_assert_nonnull(policy);
    identity = g_strdup(mux_network_policy_get_identity(policy));
    memset(mode, 'x', strlen(mode));
    memset(uri, 'x', strlen(uri));
    g_clear_pointer(&mode, g_free);
    g_clear_pointer(&uri, g_free);
    g_assert_cmpint(mux_network_policy_get_mode(policy), ==, MUX_NETWORK_MODE_TOR);
    g_assert_cmpstr(mux_network_policy_get_proxy_uri(policy),
                    ==, "socks5://127.0.0.1:9050");
    g_assert_cmpstr(mux_network_policy_get_identity(policy), ==, identity);
}

static void
test_errors_do_not_expose_credentials(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MuxNetworkPolicy) policy = mux_network_policy_new(
        "tor", "socks5://secret-user:secret-password@127.0.0.1:9050", &error);

    g_assert_null(policy);
    g_assert_error(error, MUX_NETWORK_POLICY_ERROR,
                    MUX_NETWORK_POLICY_ERROR_INVALID_PROXY);
    g_assert_null(strstr(error->message, "secret-user"));
    g_assert_null(strstr(error->message, "secret-password"));
}

#ifdef MUX_NETWORK_POLICY_TEST_WPE
static void
assert_ephemeral_session(WebKitNetworkSession *session)
{
    g_assert_nonnull(session);
    g_assert_true(webkit_network_session_is_ephemeral(session));
    g_assert_true(webkit_network_session_get_itp_enabled(session));
    g_assert_false(
        webkit_network_session_get_persistent_credential_storage_enabled(session));
}

static void
test_wpe_direct_persistent_defaults(void)
{
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new("direct", NULL, NULL);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *data_directory =
        g_build_filename(g_get_user_data_dir(), "policy", NULL);
    g_autofree gchar *cache_directory =
        g_build_filename(g_get_user_cache_dir(), "policy", NULL);
    g_autofree gchar *baseline_data_directory =
        g_build_filename(g_get_user_data_dir(), "baseline", NULL);
    g_autofree gchar *baseline_cache_directory =
        g_build_filename(g_get_user_cache_dir(), "baseline", NULL);
    WebKitNetworkSession *baseline = webkit_network_session_new(
        baseline_data_directory, baseline_cache_directory);
    WebKitNetworkSession *session = mux_network_policy_wpe_new_session(
        policy, FALSE, data_directory, cache_directory, &error);

    g_assert_no_error(error);
    g_assert_nonnull(baseline);
    g_assert_nonnull(session);
    g_assert_false(webkit_network_session_is_ephemeral(session));
    g_assert_cmpint(webkit_network_session_get_itp_enabled(session),
                    ==, webkit_network_session_get_itp_enabled(baseline));
    g_assert_cmpint(
        webkit_network_session_get_persistent_credential_storage_enabled(session),
        ==,
        webkit_network_session_get_persistent_credential_storage_enabled(baseline));
    g_object_unref(session);
    g_object_unref(baseline);
}

static void
test_wpe_direct_private_session(void)
{
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new("direct", NULL, NULL);
    g_autoptr(GError) error = NULL;
    WebKitNetworkSession *session = mux_network_policy_wpe_new_session(
        policy, TRUE, NULL, NULL, &error);

    g_assert_no_error(error);
    assert_ephemeral_session(session);
    g_object_unref(session);
}

static void
test_wpe_tor_sessions(void)
{
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new("tor", "socks5://127.0.0.1:9050", NULL);
    guint private_profile;

    for (private_profile = 0; private_profile <= 1; private_profile++) {
        g_autoptr(GError) error = NULL;
        WebKitNetworkSession *session = mux_network_policy_wpe_new_session(
            policy, private_profile, NULL, NULL, &error);

        g_assert_no_error(error);
        assert_ephemeral_session(session);
        g_object_unref(session);
    }
}

static void
test_wpe_view_settings(void)
{
    g_autoptr(MuxNetworkPolicy) direct =
        mux_network_policy_new("direct", NULL, NULL);
    g_autoptr(MuxNetworkPolicy) tor =
        mux_network_policy_new("tor", "socks5://127.0.0.1:9050", NULL);
    g_autoptr(GError) error = NULL;
    WebKitSettings *settings = webkit_settings_new();
    gboolean initial_webrtc = webkit_settings_get_enable_webrtc(settings);

    g_assert_true(mux_network_policy_wpe_apply_settings(direct, settings, &error));
    g_assert_no_error(error);
    g_assert_cmpint(webkit_settings_get_enable_webrtc(settings),
                    ==, initial_webrtc);
    g_assert_true(mux_network_policy_wpe_apply_settings(tor, settings, &error));
    g_assert_no_error(error);
    g_assert_false(webkit_settings_get_enable_webrtc(settings));

    /* Direct mode also preserves an explicitly disabled setting. */
    g_assert_true(mux_network_policy_wpe_apply_settings(direct, settings, &error));
    g_assert_no_error(error);
    g_assert_false(webkit_settings_get_enable_webrtc(settings));
    g_object_unref(settings);
}

static void
test_wpe_requires_policy_and_settings(void)
{
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new("direct", NULL, NULL);
    g_autoptr(GError) error = NULL;
    WebKitSettings *settings = webkit_settings_new();

    g_assert_null(mux_network_policy_wpe_new_session(
        NULL, FALSE, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(mux_network_policy_wpe_apply_settings(NULL, settings, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(mux_network_policy_wpe_apply_settings(policy, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_object_unref(settings);
}
#endif

int
main(int argc, char **argv)
{
    const InvalidProxy invalid_proxies[] = {
        { "missing", NULL },
        { "empty", "" },
        { "bare-address", "127.0.0.1:9050" },
        { "socks-fallback", "socks://127.0.0.1:9050" },
        { "socks4", "socks4://127.0.0.1:9050" },
        { "socks4a", "socks4a://127.0.0.1:9050" },
        { "socks5h", "socks5h://127.0.0.1:9050" },
        { "http", "http://127.0.0.1:9050" },
        { "https", "https://127.0.0.1:9050" },
        { "direct", "direct://" },
        { "missing-port", "socks5://127.0.0.1" },
        { "empty-port", "socks5://127.0.0.1:" },
        { "zero-port", "socks5://127.0.0.1:0" },
        { "negative-port", "socks5://127.0.0.1:-1" },
        { "large-port", "socks5://127.0.0.1:65536" },
        { "text-port", "socks5://127.0.0.1:tor" },
        { "hostname", "socks5://localhost:9050" },
        { "remote-ipv4", "socks5://192.0.2.1:9050" },
        { "remote-ipv6", "socks5://[2001:db8::1]:9050" },
        { "any-ipv4", "socks5://0.0.0.0:9050" },
        { "any-ipv6", "socks5://[::]:9050" },
        { "multicast", "socks5://224.0.0.1:9050" },
        { "invalid-ipv4", "socks5://127.0.0.999:9050" },
        { "short-ipv4", "socks5://127.1:9050" },
        { "decimal-ipv4", "socks5://2130706433:9050" },
        { "hex-ipv4", "socks5://0x7f000001:9050" },
        { "octal-ipv4", "socks5://0177.0.0.1:9050" },
        { "unbracketed-ipv6", "socks5://::1:9050" },
        { "ipv6-zone", "socks5://[::1%25lo0]:9050" },
        { "credentials", "socks5://user:password@127.0.0.1:9050" },
        { "empty-credentials", "socks5://@127.0.0.1:9050" },
        { "path", "socks5://127.0.0.1:9050/route" },
        { "root-path", "socks5://127.0.0.1:9050/" },
        { "query", "socks5://127.0.0.1:9050?bypass=*" },
        { "empty-query", "socks5://127.0.0.1:9050?" },
        { "fragment", "socks5://127.0.0.1:9050#direct" },
        { "empty-fragment", "socks5://127.0.0.1:9050#" },
        { "leading-space", " socks5://127.0.0.1:9050" },
        { "trailing-space", "socks5://127.0.0.1:9050 " },
        { "newline", "socks5://127.0.0.1:9050\n" },
        { "encoded-host", "socks5://%31%32%37.0.0.1:9050" },
        { "backslash", "socks5://127.0.0.1:9050\\route" },
        { "fallback-list", "socks5://127.0.0.1:9050,direct://" },
    };
    guint i;

#ifdef MUX_NETWORK_POLICY_TEST_WPE
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
#else
    g_test_init(&argc, &argv, NULL);
#endif
    g_test_add_func("/network-policy/direct/defaults", test_direct_defaults);
    g_test_add_func("/network-policy/direct/reject-proxy", test_direct_rejects_proxy);
    g_test_add_func("/network-policy/reject-mode", test_invalid_mode);
    g_test_add_func("/network-policy/tor/loopback-endpoints", test_tor_loopback_endpoints);
    g_test_add_func("/network-policy/identity", test_policy_identity);
    g_test_add_func("/network-policy/owns-input", test_policy_owns_input);
    g_test_add_func("/network-policy/error-redaction", test_errors_do_not_expose_credentials);
    for (i = 0; i < G_N_ELEMENTS(invalid_proxies); i++) {
        g_autofree gchar *path = g_strconcat(
            "/network-policy/tor/reject/", invalid_proxies[i].name, NULL);

        g_test_add_data_func(path, &invalid_proxies[i], test_invalid_proxy);
    }
#ifdef MUX_NETWORK_POLICY_TEST_WPE
    g_test_add_func("/network-policy/wpe/direct-persistent-defaults",
                    test_wpe_direct_persistent_defaults);
    g_test_add_func("/network-policy/wpe/direct-private-session",
                    test_wpe_direct_private_session);
    g_test_add_func("/network-policy/wpe/tor-sessions", test_wpe_tor_sessions);
    g_test_add_func("/network-policy/wpe/view-settings", test_wpe_view_settings);
    g_test_add_func("/network-policy/wpe/required-arguments",
                    test_wpe_requires_policy_and_settings);
#endif
    return g_test_run();
}
