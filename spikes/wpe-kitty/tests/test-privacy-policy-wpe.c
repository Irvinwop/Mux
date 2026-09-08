#include "mux-privacy-policy-wpe.h"

#include <gio/gio.h>

static MuxNetworkPolicy *
tor_policy(void)
{
    return mux_network_policy_new("tor", "socks5://127.0.0.1:9050", NULL);
}

static void
test_native_tor_context(void)
{
    g_autoptr(MuxNetworkPolicy) policy = tor_policy();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *property_value = NULL;
    WebKitWebContext *context =
        mux_privacy_policy_wpe_new_context(policy, &error);

    g_assert_no_error(error);
    g_assert_nonnull(context);
    g_assert_cmpstr(webkit_web_context_get_time_zone_override(context), ==, "UTC");
    g_object_get(context, MUX_PRIVACY_TIME_ZONE_PROPERTY, &property_value, NULL);
    g_assert_cmpstr(property_value, ==, "UTC");
    g_assert_true(mux_privacy_policy_check_time_zone_property(
        g_object_class_find_property(G_OBJECT_GET_CLASS(context),
                                      MUX_PRIVACY_TIME_ZONE_PROPERTY),
        &error));
    g_assert_no_error(error);
    g_object_unref(context);
}

static void
test_direct_uses_unchanged_default_context(void)
{
    g_autoptr(MuxNetworkPolicy) policy = mux_network_policy_new(NULL, NULL, NULL);
    g_autoptr(GError) error = NULL;
    WebKitWebContext *baseline = webkit_web_context_get_default();
    g_autofree gchar *original_timezone =
        g_strdup(webkit_web_context_get_time_zone_override(baseline));
    WebKitWebContext *context =
        mux_privacy_policy_wpe_new_context(policy, &error);

    g_assert_no_error(error);
    g_assert_true(context == baseline);
    g_assert_cmpstr(webkit_web_context_get_time_zone_override(context),
                    ==, original_timezone);
    g_object_unref(context);
}

static void
test_tor_contexts_do_not_reuse_default_or_each_other(void)
{
    g_autoptr(MuxNetworkPolicy) policy = tor_policy();
    g_autoptr(GError) error = NULL;
    WebKitWebContext *baseline = webkit_web_context_get_default();
    g_autofree gchar *original_timezone =
        g_strdup(webkit_web_context_get_time_zone_override(baseline));
    WebKitWebContext *first = mux_privacy_policy_wpe_new_context(policy, &error);
    WebKitWebContext *second;

    g_assert_no_error(error);
    g_assert_nonnull(first);
    second = mux_privacy_policy_wpe_new_context(policy, &error);
    g_assert_no_error(error);
    g_assert_nonnull(second);
    g_assert_true(first != second);
    g_assert_true(first != baseline);
    g_assert_true(second != baseline);
    g_assert_cmpstr(webkit_web_context_get_time_zone_override(first), ==, "UTC");
    g_assert_cmpstr(webkit_web_context_get_time_zone_override(second), ==, "UTC");
    g_assert_cmpstr(webkit_web_context_get_time_zone_override(baseline),
                    ==, original_timezone);
    g_object_unref(second);
    g_object_unref(first);
}

static void
test_missing_policy_is_not_direct(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_null(mux_privacy_policy_wpe_new_context(NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
    g_test_add_func("/privacy-policy/wpe/native-tor-context", test_native_tor_context);
    g_test_add_func("/privacy-policy/wpe/direct-default-context", test_direct_uses_unchanged_default_context);
    g_test_add_func("/privacy-policy/wpe/context-isolation", test_tor_contexts_do_not_reuse_default_or_each_other);
    g_test_add_func("/privacy-policy/wpe/required-policy", test_missing_policy_is_not_direct);
    return g_test_run();
}
