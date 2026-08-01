#include "mux-uri.h"

#include <glib.h>
#include <string.h>

static void
assert_resolves(const gchar *input,
                const gchar *search_url,
                const gchar *expected)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *resolved = NULL;

    resolved = mux_uri_resolve_user_input(input, search_url, &error);
    g_assert_no_error(error);
    g_assert_nonnull(resolved);
    g_assert_cmpstr(resolved, ==, expected);
}

static void
assert_rejected(const gchar *input,
                const gchar *search_url,
                MuxUriError expected_code,
                const gchar *message_fragment)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *resolved = NULL;

    resolved = mux_uri_resolve_user_input(input, search_url, &error);
    g_assert_null(resolved);
    g_assert_error(error, MUX_URI_ERROR, (gint)expected_code);
    g_assert_nonnull(strstr(error->message, message_fragment));
}

static void
test_direct_urls(void)
{
    assert_resolves("  https://example.com/a?b=c#d\n",
                    NULL,
                    "https://example.com/a?b=c#d");
    assert_resolves("http://localhost:8080/health",
                    NULL,
                    "http://localhost:8080/health");
    assert_resolves("https://[2001:db8::1]:8443/path",
                    NULL,
                    "https://[2001:db8::1]:8443/path");
    assert_resolves("about:blank", NULL, "about:blank");
    assert_resolves("ABOUT:BLANK", NULL, "about:blank");
    assert_resolves("\xe2\x80\x83" "example.com" "\xe2\x80\x83",
                    NULL,
                    "https://example.com");
}

static void
test_inferred_hosts(void)
{
    assert_resolves("example.com", NULL, "https://example.com");
    assert_resolves("Example.COM:8443/path?q=yes",
                    NULL,
                    "https://example.com:8443/path?q=yes");
    assert_resolves("localhost", NULL, "http://localhost");
    assert_resolves("localhost:3000/app",
                    NULL,
                    "http://localhost:3000/app");
    assert_resolves("dev.localhost:5173",
                    NULL,
                    "http://dev.localhost:5173");
    assert_resolves("127.0.0.1:8080/status",
                    NULL,
                    "http://127.0.0.1:8080/status");
    assert_resolves("192.168.1.20", NULL, "http://192.168.1.20");
    assert_resolves("[::1]:9000/api",
                    NULL,
                    "http://[::1]:9000/api");
    assert_resolves("2001:db8::1", NULL, "http://[2001:db8::1]");
}

static void
test_search_default(void)
{
    assert_resolves("weather tomorrow",
                    NULL,
                    "https://duckduckgo.com/?q=weather%20tomorrow");
    assert_resolves("caf\xc3\xa9 & tea",
                    NULL,
                    "https://duckduckgo.com/?q=caf%C3%A9%20%26%20tea");
    assert_resolves("singleword",
                    NULL,
                    "https://duckduckgo.com/?q=singleword");
    assert_resolves("person@example.com",
                    NULL,
                    "https://duckduckgo.com/?q=person%40example.com");
    assert_resolves("cats/dogs",
                    NULL,
                    "https://duckduckgo.com/?q=cats%2Fdogs");
    assert_resolves("   \t\n", NULL, "about:blank");
}

static void
test_search_configuration(void)
{
    assert_resolves("mux browser",
                    "https://search.example/find?q={query}&source=mux",
                    "https://search.example/find?q=mux%20browser&source=mux");
    assert_resolves("a+b",
                    "http://localhost:8888/search?q=",
                    "http://localhost:8888/search?q=a%2Bb");
    assert_resolves("direct.example", "file:///unused", "https://direct.example");
}

static void
test_rejected_schemes(void)
{
    static const gchar *const rejected[] = {
        "javascript:alert(1)",
        "file:///etc/passwd",
        "data:text/html,hello",
        "blob:https://example.com/id",
        "ftp://example.com/file",
        "mailto:user@example.com",
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(rejected); i++) {
        assert_rejected(rejected[i],
                        NULL,
                        MUX_URI_ERROR_DISALLOWED_SCHEME,
                        "not allowed");
    }
    assert_rejected("about:config",
                    NULL,
                    MUX_URI_ERROR_DISALLOWED_SCHEME,
                    "Only about:blank");
}

static void
test_malformed_urls(void)
{
    assert_rejected("https:///missing-host",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "missing a host");
    assert_rejected("https://example.com/a b",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "whitespace");
    assert_rejected("example.com:abc",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "not a decimal");
    assert_rejected("localhost:0",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "port 0");
    assert_rejected("localhost:65536",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "range 1-65535");
    assert_rejected("999.1.2.3",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "not a valid domain");
    assert_rejected("[not-ip]:8000",
                    NULL,
                    MUX_URI_ERROR_INVALID_URL,
                    "not a valid bracketed IPv6");
}

static void
test_invalid_search_configuration(void)
{
    assert_rejected("search terms",
                    "file:///search?q={query}",
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "http:// or https://");
    assert_rejected("search terms",
                    "https:///search?q={query}",
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "Search URL is invalid");
    assert_rejected("search terms",
                    "https://{query}.example.com/search",
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "after the search host");
    assert_rejected("search terms",
                    "https://search.example/?a={query}&b={query}",
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "at most one");
}

static void
test_invalid_input(void)
{
    const gchar invalid_utf8[] = {'b', 'a', 'd', (gchar)0xff, '\0'};

    assert_rejected(NULL,
                    NULL,
                    MUX_URI_ERROR_INVALID_INPUT,
                    "missing");
    assert_rejected(invalid_utf8,
                    NULL,
                    MUX_URI_ERROR_INVALID_INPUT,
                    "UTF-8");
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/uri/direct/allowed", test_direct_urls);
    g_test_add_func("/uri/direct/inferred-hosts", test_inferred_hosts);
    g_test_add_func("/uri/search/default", test_search_default);
    g_test_add_func("/uri/search/configuration", test_search_configuration);
    g_test_add_func("/uri/direct/rejected-schemes", test_rejected_schemes);
    g_test_add_func("/uri/direct/malformed", test_malformed_urls);
    g_test_add_func("/uri/search/invalid-configuration",
                    test_invalid_search_configuration);
    g_test_add_func("/uri/input/invalid", test_invalid_input);
    return g_test_run();
}
