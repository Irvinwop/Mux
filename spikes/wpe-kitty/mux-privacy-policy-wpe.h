#pragma once

#include "mux-privacy-policy.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

/* Returns a full reference. Tor gets a newly constructed UTC context with
 * fixed preferred languages set before any view is handed that context.
 * Direct keeps the existing WebKit default context without normalization. */
WebKitWebContext *mux_privacy_policy_wpe_new_context(
    const MuxNetworkPolicy *policy,
    GError **error);

G_END_DECLS
