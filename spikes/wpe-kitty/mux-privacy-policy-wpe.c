#include "mux-privacy-policy-wpe.h"

#include <gio/gio.h>

WebKitWebContext *
mux_privacy_policy_wpe_new_context(const MuxNetworkPolicy *policy,
                                   GError **error)
{
    const gchar *time_zone;
    const gchar *const *languages;
    GObjectClass *context_class;
    GParamSpec *property;
    WebKitWebContext *context;
    gboolean supported;

    if (!policy) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "A network policy is required before creating "
                            "a privacy context");
        return NULL;
    }
    if (mux_network_policy_get_mode(policy) == MUX_NETWORK_MODE_DIRECT)
        return g_object_ref(webkit_web_context_get_default());

    time_zone = mux_privacy_policy_get_time_zone(policy);
    languages = mux_privacy_policy_get_languages(policy);
    context_class = g_type_class_ref(WEBKIT_TYPE_WEB_CONTEXT);
    property = g_object_class_find_property(context_class,
                                             MUX_PRIVACY_TIME_ZONE_PROPERTY);
    supported = mux_privacy_policy_check_time_zone_property(property, error);
    g_type_class_unref(context_class);
    if (!supported)
        return NULL;

    /* This property feeds WebKit's process-pool configuration during
     * construction. Setting it on the already-created default context would
     * be too late, and changing the process-wide TZ env would affect direct. */
    context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
                            MUX_PRIVACY_TIME_ZONE_PROPERTY, time_zone,
                            NULL);
    if (!context) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Native Tor WebKit context construction failed");
        return NULL;
    }
    if (g_strcmp0(webkit_web_context_get_time_zone_override(context),
                  time_zone) != 0) {
        g_object_unref(context);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "WebKit did not retain the required native UTC "
                            "timezone override; refusing to browse");
        return NULL;
    }

    /* The native API sets the context process pool's language overrides.
     * No WebView or browsing request may be created before this returns. */
    webkit_web_context_set_preferred_languages(context, languages);
    return context;
}
