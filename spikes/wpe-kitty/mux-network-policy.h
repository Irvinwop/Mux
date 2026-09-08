#ifndef MUX_NETWORK_POLICY_H
#define MUX_NETWORK_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    MUX_NETWORK_MODE_DIRECT,
    MUX_NETWORK_MODE_TOR,
} MuxNetworkMode;

typedef enum {
    MUX_NETWORK_POLICY_ERROR_INVALID_MODE,
    MUX_NETWORK_POLICY_ERROR_INVALID_PROXY,
} MuxNetworkPolicyError;

#define MUX_NETWORK_POLICY_ERROR (mux_network_policy_error_quark())

typedef struct _MuxNetworkPolicy MuxNetworkPolicy;

GQuark mux_network_policy_error_quark(void);

/*
 * Create an immutable policy owned by one engine profile. A NULL mode selects
 * direct; otherwise only "direct" and "tor" are accepted. Direct mode requires
 * a NULL proxy URI. Tor requires an explicit socks5://LOOPBACK_IP:PORT URI,
 * with brackets around IPv6 and without credentials or any other URI parts.
 * Construction does not contact the proxy or establish that it is Tor.
 */
MuxNetworkPolicy *mux_network_policy_new(const gchar *mode,
                                        const gchar *socks5_proxy_uri,
                                        GError **error);
void mux_network_policy_free(MuxNetworkPolicy *policy);

MuxNetworkMode mux_network_policy_get_mode(const MuxNetworkPolicy *policy);
const gchar *mux_network_policy_get_proxy_uri(const MuxNetworkPolicy *policy);

/*
 * Borrowed, opaque, versioned identity for exact comparison alongside the
 * profile identity before engine reuse. Equivalent URI spellings normalize to
 * the same identity. This is neither a Tor circuit ID nor a security proof.
 */
const gchar *mux_network_policy_get_identity(const MuxNetworkPolicy *policy);
gboolean mux_network_policy_is_ephemeral(const MuxNetworkPolicy *policy,
                                         gboolean private_profile);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNetworkPolicy, mux_network_policy_free)

G_END_DECLS

#endif
