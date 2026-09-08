#include "mux-network-policy.h"
#include "mux-privacy-policy.h"

#include <gio/gio.h>
#include <string.h>

struct _MuxNetworkPolicy {
    MuxNetworkMode mode;
    gchar *proxy_uri;
    gchar *identity;
};

G_DEFINE_QUARK(mux-network-policy-error-quark, mux_network_policy_error)

static gchar *
normalize_socks5_proxy_uri(const gchar *proxy_uri, GError **error)
{
    g_autoptr(GUri) uri = NULL;
    g_autoptr(GInetAddress) address = NULL;
    g_autofree gchar *host = NULL;
    const gchar *path;
    const guchar *cursor;
    gint port;

    if (!proxy_uri || !*proxy_uri)
        goto invalid;

    /* Numeric endpoints need no escapes, Unicode, or whitespace normalization.
     * Reject these before URI decoding can change the supplied authority. */
    for (cursor = (const guchar *)proxy_uri; *cursor; cursor++) {
        if (*cursor >= 0x7f || g_ascii_isspace(*cursor) ||
            g_ascii_iscntrl(*cursor) || *cursor == '%' || *cursor == '\\')
            goto invalid;
    }

    uri = g_uri_parse(proxy_uri, G_URI_FLAGS_NONE, NULL);
    if (!uri ||
        g_ascii_strcasecmp(g_uri_get_scheme(uri), "socks5") != 0 ||
        !g_uri_get_host(uri) || g_uri_get_userinfo(uri) ||
        g_uri_get_query(uri) || g_uri_get_fragment(uri))
        goto invalid;

    path = g_uri_get_path(uri);
    port = g_uri_get_port(uri);
    if ((path && *path) || port < 1 || port > 65535)
        goto invalid;

    /* Parsing a numeric address is local; no hostname resolver is consulted. */
    address = g_inet_address_new_from_string(g_uri_get_host(uri));
    if (!address || !g_inet_address_get_is_loopback(address))
        goto invalid;

    host = g_inet_address_to_string(address);
    if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6)
        return g_strdup_printf("socks5://[%s]:%d", host, port);
    return g_strdup_printf("socks5://%s:%d", host, port);

invalid:
    /* Do not echo rejected input: it may accidentally contain credentials. */
    g_set_error_literal(error,
                        MUX_NETWORK_POLICY_ERROR,
                        MUX_NETWORK_POLICY_ERROR_INVALID_PROXY,
                        "Tor requires socks5://LOOPBACK_IP:PORT with port "
                        "1..65535 and no credentials, path, query, or fragment");
    return NULL;
}

MuxNetworkPolicy *
mux_network_policy_new(const gchar *mode,
                       const gchar *socks5_proxy_uri,
                       GError **error)
{
    g_autoptr(MuxNetworkPolicy) policy = NULL;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    if (mode && strcmp(mode, "direct") != 0 && strcmp(mode, "tor") != 0) {
        g_set_error_literal(error,
                            MUX_NETWORK_POLICY_ERROR,
                            MUX_NETWORK_POLICY_ERROR_INVALID_MODE,
                            "Network mode must be direct or tor");
        return NULL;
    }

    policy = g_new0(MuxNetworkPolicy, 1);
    policy->mode = mode && strcmp(mode, "tor") == 0 ?
        MUX_NETWORK_MODE_TOR : MUX_NETWORK_MODE_DIRECT;
    if (policy->mode == MUX_NETWORK_MODE_DIRECT) {
        if (socks5_proxy_uri != NULL) {
            g_set_error_literal(error,
                                MUX_NETWORK_POLICY_ERROR,
                                MUX_NETWORK_POLICY_ERROR_INVALID_PROXY,
                                "A SOCKS5 proxy requires explicit tor mode");
            return NULL;
        }
        policy->identity = g_strdup("mux-network-v1-direct");
    } else {
        g_autofree gchar *digest = NULL;

        policy->proxy_uri = normalize_socks5_proxy_uri(socks5_proxy_uri, error);
        if (!policy->proxy_uri)
            return NULL;
        digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                policy->proxy_uri,
                                                -1);
        /* The native context revision is part of attestation, not just the
         * proxy. Older Tor engines without normalization must not be reused. */
        policy->identity = g_strconcat("mux-network-v2-tor-",
                                        MUX_TOR_PRIVACY_POLICY_ID,
                                        "-",
                                        digest,
                                        NULL);
    }
    return g_steal_pointer(&policy);
}

void
mux_network_policy_free(MuxNetworkPolicy *policy)
{
    if (!policy)
        return;
    g_free(policy->proxy_uri);
    g_free(policy->identity);
    g_free(policy);
}

MuxNetworkMode
mux_network_policy_get_mode(const MuxNetworkPolicy *policy)
{
    g_return_val_if_fail(policy != NULL, MUX_NETWORK_MODE_TOR);
    return policy->mode;
}

const gchar *
mux_network_policy_get_proxy_uri(const MuxNetworkPolicy *policy)
{
    g_return_val_if_fail(policy != NULL, NULL);
    return policy->proxy_uri;
}

const gchar *
mux_network_policy_get_identity(const MuxNetworkPolicy *policy)
{
    g_return_val_if_fail(policy != NULL, NULL);
    return policy->identity;
}

gboolean
mux_network_policy_is_ephemeral(const MuxNetworkPolicy *policy,
                                gboolean private_profile)
{
    g_return_val_if_fail(policy != NULL, TRUE);
    return private_profile || policy->mode == MUX_NETWORK_MODE_TOR;
}
