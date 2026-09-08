#ifndef MUX_NETWORK_POLICY_WPE_H
#define MUX_NETWORK_POLICY_WPE_H

#include "mux-network-policy.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

/*
 * Returns a fully configured, owned session. Apply this factory to every
 * persistent/private session path, including sessions inherited by popups and
 * downloads. Data/cache directories are unused for ephemeral sessions.
 * Direct mode preserves WebKit's existing default proxy configuration.
 */
WebKitNetworkSession *mux_network_policy_wpe_new_session(
    const MuxNetworkPolicy *policy,
    gboolean private_profile,
    const gchar *data_directory,
    const gchar *cache_directory,
    GError **error);

/* Apply to settings before constructing any view that uses them. A FALSE
 * result must abort that view's creation; callers must not browse on failure. */
gboolean mux_network_policy_wpe_apply_settings(const MuxNetworkPolicy *policy,
                                              WebKitSettings *settings,
                                              GError **error);

G_END_DECLS

#endif
