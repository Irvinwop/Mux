#ifndef MUX_NAVIGATION_POLICY_H
#define MUX_NAVIGATION_POLICY_H

#include <gio/gio.h>
#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxNavigationPolicy MuxNavigationPolicy;

typedef gboolean (*MuxNavigationPolicyOutputFunc)(GBytes *payload,
                                                  gpointer user_data,
                                                  GError **error);

MuxNavigationPolicy *mux_navigation_policy_new(
    WebKitWebView *web_view,
    gboolean private_profile,
    MuxNavigationPolicyOutputFunc output,
    gpointer output_data,
    GDestroyNotify output_destroy);

gboolean mux_navigation_policy_handle_payload(MuxNavigationPolicy *policy,
                                              const guint8 *data,
                                              gsize length,
                                              GError **error);

void mux_navigation_policy_free(MuxNavigationPolicy *policy);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNavigationPolicy, mux_navigation_policy_free)

G_END_DECLS

#endif
