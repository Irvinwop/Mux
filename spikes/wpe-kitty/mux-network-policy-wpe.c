#include "mux-network-policy-wpe.h"

WebKitNetworkSession *
mux_network_policy_wpe_new_session(const MuxNetworkPolicy *policy,
                                   gboolean private_profile,
                                   const gchar *data_directory,
                                   const gchar *cache_directory,
                                   GError **error)
{
    WebKitNetworkProxySettings *proxy_settings = NULL;
    WebKitNetworkSession *session;
    gboolean ephemeral;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (!policy) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "A network policy is required before session creation");
        return NULL;
    }

    if (mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_TOR) {
        const gchar *const ignore_hosts[] = { NULL };

        /* A single explicit socks5 URI avoids socks:// protocol fallback.
         * Empty exclusions prevent an inherited NO_PROXY/direct bypass. */
        proxy_settings = webkit_network_proxy_settings_new(
            mux_network_policy_get_proxy_uri(policy), ignore_hosts);
        if (!proxy_settings) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "WebKit rejected the validated SOCKS5 policy");
            return NULL;
        }
    }

    ephemeral = mux_network_policy_is_ephemeral(policy, private_profile);
    session = ephemeral ? webkit_network_session_new_ephemeral() :
        webkit_network_session_new(data_directory, cache_directory);
    if (!session) {
        if (proxy_settings)
            webkit_network_proxy_settings_free(proxy_settings);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "WebKit network session construction failed");
        return NULL;
    }

    if (proxy_settings) {
        webkit_network_session_set_proxy_settings(
            session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy_settings);
        webkit_network_proxy_settings_free(proxy_settings);
    }
    if (ephemeral) {
        webkit_network_session_set_itp_enabled(session, TRUE);
        webkit_network_session_set_persistent_credential_storage_enabled(
            session, FALSE);
    }
    return session;
}

gboolean
mux_network_policy_wpe_apply_settings(const MuxNetworkPolicy *policy,
                                      WebKitSettings *settings,
                                      GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    if (!policy || !WEBKIT_IS_SETTINGS(settings)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "A network policy and WebKit settings are required");
        return FALSE;
    }

    if (mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_TOR) {
        webkit_settings_set_enable_webrtc(settings, FALSE);
        if (webkit_settings_get_enable_webrtc(settings)) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_SUPPORTED,
                                "WebKit could not disable WebRTC for Tor mode");
            return FALSE;
        }
    }
    return TRUE;
}
