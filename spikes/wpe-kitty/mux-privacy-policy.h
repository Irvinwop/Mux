#pragma once

#include "mux-network-policy.h"

#include <glib-object.h>

G_BEGIN_DECLS

/* Included in the Tor engine attestation. Change this when fixed native
 * normalization rules change; never reuse an engine with an older policy. */
#define MUX_TOR_PRIVACY_POLICY_ID "tz-utc-lang-en-us-en-v1"
#define MUX_PRIVACY_TIME_ZONE_PROPERTY "time-zone-override"

/* Borrowed immutable values. Direct mode returns NULL and keeps host defaults. */
const gchar *mux_privacy_policy_get_time_zone(const MuxNetworkPolicy *policy);
const gchar *const *mux_privacy_policy_get_languages(
    const MuxNetworkPolicy *policy);

/* Portable contract check used before native Tor context construction. */
gboolean mux_privacy_policy_check_time_zone_property(GParamSpec *property,
                                                     GError **error);

G_END_DECLS
