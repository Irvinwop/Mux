#include "mux-privacy-policy.h"
#include "mux-network-handshake.h"

#include <gio/gio.h>

static MuxNetworkPolicy *
tor_policy(void)
{
    g_autoptr(GError) error = NULL;
    MuxNetworkPolicy *policy = mux_network_policy_new(
        "tor", "socks5://127.0.0.1:9050", &error);

    g_assert_no_error(error);
    g_assert_nonnull(policy);
    return policy;
}

static void
test_direct_preserves_defaults(void)
{
    g_autoptr(MuxNetworkPolicy) policy =
        mux_network_policy_new(NULL, NULL, NULL);

    g_assert_null(mux_privacy_policy_get_time_zone(policy));
    g_assert_null(mux_privacy_policy_get_languages(policy));
    g_assert_cmpstr(mux_network_policy_get_identity(policy),
                    ==, "mux-network-v1-direct");
}

static void
test_tor_fixed_native_values(void)
{
    g_autoptr(MuxNetworkPolicy) policy = tor_policy();
    const gchar *const *languages = mux_privacy_policy_get_languages(policy);

    g_assert_cmpstr(mux_privacy_policy_get_time_zone(policy), ==, "UTC");
    g_assert_nonnull(languages);
    g_assert_cmpstr(languages[0], ==, "en-US");
    g_assert_cmpstr(languages[1], ==, "en");
    g_assert_null(languages[2]);
    g_assert_true(g_str_has_prefix(mux_network_policy_get_identity(policy),
                                   "mux-network-v2-tor-"
                                   MUX_TOR_PRIVACY_POLICY_ID "-"));
}

static void
test_supported_native_property(void)
{
    g_autoptr(GParamSpec) property = g_param_spec_string(
        MUX_PRIVACY_TIME_ZONE_PROPERTY, "Timezone", "Native timezone", NULL,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_autoptr(GError) error = NULL;

    g_assert_true(mux_privacy_policy_check_time_zone_property(property, &error));
    g_assert_no_error(error);
}

static void
test_missing_native_property(void)
{
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_privacy_policy_check_time_zone_property(NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_wrong_native_property_type(void)
{
    g_autoptr(GParamSpec) property = g_param_spec_boolean(
        MUX_PRIVACY_TIME_ZONE_PROPERTY, "Timezone", "Wrong native type", FALSE,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_privacy_policy_check_time_zone_property(property, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_mutable_native_property_rejected(void)
{
    g_autoptr(GParamSpec) property = g_param_spec_string(
        MUX_PRIVACY_TIME_ZONE_PROPERTY, "Timezone", "Not construct-only", NULL,
        G_PARAM_READWRITE);
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_privacy_policy_check_time_zone_property(property, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_unreadable_native_property_rejected(void)
{
    g_autoptr(GParamSpec) property = g_param_spec_string(
        MUX_PRIVACY_TIME_ZONE_PROPERTY, "Timezone", "No native readback", NULL,
        G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_privacy_policy_check_time_zone_property(property, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_pre_normalization_engine_rejected(void)
{
    const gchar *profile = "tor-private-abcdefghijkl";
    const gchar *socket_path = "/tmp/tor-native-policy.sock";
    g_autoptr(MuxNetworkPolicy) policy = tor_policy();
    g_autofree gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, mux_network_policy_get_proxy_uri(policy), -1);
    g_autofree gchar *old_identity =
        g_strconcat("mux-network-v1-tor-", digest, NULL);
    g_autoptr(GBytes) payload = NULL;
    g_autoptr(GError) error = NULL;
    MuxEngineBuilder builder;

    mux_engine_builder_init(&builder);
    mux_engine_builder_put_u32(&builder, 123);
    mux_engine_builder_put_string(&builder, profile);
    mux_engine_builder_put_string(&builder, socket_path);
    mux_engine_builder_put_string(&builder, profile);
    mux_engine_builder_put_string(&builder, old_identity);
    payload = mux_engine_builder_finish(&builder);
    g_assert_false(mux_network_handshake_check_welcome(payload,
                                                       policy,
                                                       profile,
                                                       socket_path,
                                                       &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/privacy-policy/direct-defaults", test_direct_preserves_defaults);
    g_test_add_func("/privacy-policy/tor-fixed-values", test_tor_fixed_native_values);
    g_test_add_func("/privacy-policy/native-property/supported", test_supported_native_property);
    g_test_add_func("/privacy-policy/native-property/missing", test_missing_native_property);
    g_test_add_func("/privacy-policy/native-property/wrong-type", test_wrong_native_property_type);
    g_test_add_func("/privacy-policy/native-property/mutable", test_mutable_native_property_rejected);
    g_test_add_func("/privacy-policy/native-property/unreadable", test_unreadable_native_property_rejected);
    g_test_add_func("/privacy-policy/reuse-rejects-old-tor", test_pre_normalization_engine_rejected);
    return g_test_run();
}
