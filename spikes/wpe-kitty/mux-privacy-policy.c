#include "mux-privacy-policy.h"

#include <gio/gio.h>

const gchar *
mux_privacy_policy_get_time_zone(const MuxNetworkPolicy *policy)
{
    g_return_val_if_fail(policy != NULL, NULL);
    return mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_TOR
        ? "UTC" : NULL;
}

const gchar *const *
mux_privacy_policy_get_languages(const MuxNetworkPolicy *policy)
{
    static const gchar *const languages[] = { "en-US", "en", NULL };

    g_return_val_if_fail(policy != NULL, NULL);
    return mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_TOR
        ? languages : NULL;
}

gboolean
mux_privacy_policy_check_time_zone_property(GParamSpec *property,
                                            GError **error)
{
    const GParamFlags required = G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY;

    if (property &&
        g_strcmp0(g_param_spec_get_name(property),
                  MUX_PRIVACY_TIME_ZONE_PROPERTY) == 0 &&
        G_PARAM_SPEC_VALUE_TYPE(property) == G_TYPE_STRING &&
        (property->flags & required) == required)
        return TRUE;

    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        "Tor requires WPE's readable, construct-only native "
                        "time-zone-override string property; refusing to use "
                        "the host timezone");
    return FALSE;
}
