#include "mux-uri.h"

#include <string.h>

#define MUX_URI_MAX_INPUT_BYTES (16U * 1024U)
#define MUX_URI_MAX_SEARCH_URL_BYTES 4096U
#define MUX_URI_QUERY_MARKER "{query}"

typedef enum {
    MUX_HOST_INPUT_NONE,
    MUX_HOST_INPUT_RESOLVED,
    MUX_HOST_INPUT_INVALID,
} MuxHostInputResult;

typedef enum {
    MUX_HOST_NAME_NONE,
    MUX_HOST_NAME_VALID,
    MUX_HOST_NAME_INVALID,
} MuxHostNameResult;

GQuark
mux_uri_error_quark(void)
{
    return g_quark_from_static_string("mux-uri-error-quark");
}

static gchar *
trim_input(const gchar *input, GError **error)
{
    const gchar *start;
    const gchar *end;
    gsize length;

    if (!input) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_INPUT,
                            "Navigation input is missing");
        return NULL;
    }

    length = strlen(input);
    if (length > MUX_URI_MAX_INPUT_BYTES) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_INPUT,
                    "Navigation input exceeds the %u-byte limit",
                    MUX_URI_MAX_INPUT_BYTES);
        return NULL;
    }
    if (!g_utf8_validate(input, length, NULL)) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_INPUT,
                            "Navigation input is not valid UTF-8");
        return NULL;
    }

    start = input;
    end = input + length;
    while (start < end) {
        gunichar character = g_utf8_get_char(start);

        if (!g_unichar_isspace(character))
            break;
        start = g_utf8_next_char(start);
    }
    while (end > start) {
        const gchar *previous = g_utf8_find_prev_char(start, end);

        if (!previous || !g_unichar_isspace(g_utf8_get_char(previous)))
            break;
        end = previous;
    }

    return g_strndup(start, end - start);
}

static gboolean
has_url_forbidden_character(const gchar *text)
{
    const gchar *cursor;

    for (cursor = text; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);

        if (g_unichar_isspace(character) ||
            g_unichar_iscntrl(character))
            return TRUE;
    }
    return FALSE;
}

static gboolean
parse_port(const gchar *port_text, GError **error)
{
    const gchar *cursor;
    guint port = 0;

    if (!port_text || !*port_text) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "A URL port was introduced with ':' but is empty");
        return FALSE;
    }

    for (cursor = port_text; *cursor; cursor++) {
        if (!g_ascii_isdigit(*cursor)) {
            g_set_error(error,
                        MUX_URI_ERROR,
                        MUX_URI_ERROR_INVALID_URL,
                        "URL port '%s' is not a decimal number",
                        port_text);
            return FALSE;
        }
        port = (port * 10U) + (guint)(*cursor - '0');
        if (port > 65535U) {
            g_set_error(error,
                        MUX_URI_ERROR,
                        MUX_URI_ERROR_INVALID_URL,
                        "URL port '%s' is outside the range 1-65535",
                        port_text);
            return FALSE;
        }
    }
    if (!port) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "URL port 0 is not allowed");
        return FALSE;
    }
    return TRUE;
}

static gboolean
looks_like_ipv4(const gchar *host)
{
    const gchar *cursor;
    guint dots = 0;

    for (cursor = host; *cursor; cursor++) {
        if (*cursor == '.') {
            dots++;
        } else if (!g_ascii_isdigit(*cursor)) {
            return FALSE;
        }
    }
    return dots == 3;
}

static gboolean
ascii_domain_is_valid(const gchar *host)
{
    gsize length;
    gsize label_start = 0;
    guint dots = 0;
    gsize i;

    length = strlen(host);
    if (!length || length > 253U)
        return FALSE;
    if (host[length - 1] == '.')
        length--;
    if (!length)
        return FALSE;

    for (i = 0; i <= length; i++) {
        if (i == length || host[i] == '.') {
            gsize label_length = i - label_start;

            if (!label_length || label_length > 63U ||
                host[label_start] == '-' || host[i - 1] == '-')
                return FALSE;
            if (i < length)
                dots++;
            label_start = i + 1;
            continue;
        }
        if (!g_ascii_isalnum(host[i]) && host[i] != '-')
            return FALSE;
    }
    return dots > 0;
}

static MuxHostNameResult
classify_host_name(const gchar *host,
                   gchar **canonical_host,
                   gboolean *prefer_http)
{
    g_autofree gchar *ascii = NULL;
    gchar *lower;
    gsize length;

    *canonical_host = NULL;
    *prefer_http = FALSE;
    if (!host || !*host)
        return MUX_HOST_NAME_NONE;

    if (g_hostname_is_ip_address(host)) {
        if (strchr(host, ':'))
            return MUX_HOST_NAME_NONE;
        *canonical_host = g_ascii_strdown(host, -1);
        *prefer_http = TRUE;
        return MUX_HOST_NAME_VALID;
    }
    if (looks_like_ipv4(host))
        return MUX_HOST_NAME_INVALID;

    ascii = g_hostname_to_ascii(host);
    if (!ascii) {
        return strchr(host, '.') ? MUX_HOST_NAME_INVALID
                                 : MUX_HOST_NAME_NONE;
    }
    lower = g_ascii_strdown(ascii, -1);
    length = strlen(lower);
    if (length > 1 && lower[length - 1] == '.')
        lower[length - 1] = '\0';

    if (g_str_equal(lower, "localhost") ||
        g_str_has_suffix(lower, ".localhost")) {
        if (!g_str_equal(lower, "localhost") &&
            !ascii_domain_is_valid(lower)) {
            g_free(lower);
            return MUX_HOST_NAME_INVALID;
        }
        *canonical_host = lower;
        *prefer_http = TRUE;
        return MUX_HOST_NAME_VALID;
    }
    if (!strchr(lower, '.')) {
        g_free(lower);
        return MUX_HOST_NAME_NONE;
    }
    if (!ascii_domain_is_valid(lower)) {
        g_free(lower);
        return MUX_HOST_NAME_INVALID;
    }

    *canonical_host = lower;
    return MUX_HOST_NAME_VALID;
}

static gchar *
validate_http_url(const gchar *uri, GError **error)
{
    g_autoptr(GError) parse_error = NULL;
    g_autoptr(GUri) parsed = NULL;
    const gchar *scheme;
    const gchar *host;

    scheme = g_uri_peek_scheme(uri);
    if (!scheme ||
        (g_ascii_strcasecmp(scheme, "http") &&
         g_ascii_strcasecmp(scheme, "https"))) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "A web URL must use http:// or https://");
        return NULL;
    }
    if (has_url_forbidden_character(uri)) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "A URL cannot contain unescaped whitespace or control characters");
        return NULL;
    }

    parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &parse_error);
    if (!parsed) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_URL,
                    "The web URL is malformed: %s",
                    parse_error->message);
        return NULL;
    }
    host = g_uri_get_host(parsed);
    if (!host || !*host) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "The web URL is missing a host name");
        return NULL;
    }
    if (g_uri_get_port(parsed) == 0) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "URL port 0 is not allowed");
        return NULL;
    }
    return g_strdup(uri);
}

static MuxHostInputResult
resolve_host_input(const gchar *input, gchar **resolved, GError **error)
{
    g_autofree gchar *authority = NULL;
    g_autofree gchar *host = NULL;
    g_autofree gchar *canonical_host = NULL;
    g_autofree gchar *candidate = NULL;
    g_autofree gchar *address = NULL;
    const gchar *suffix;
    const gchar *colon;
    const gchar *first_colon;
    const gchar *port_text = NULL;
    MuxHostNameResult host_result;
    gboolean prefer_http = FALSE;
    gsize authority_length;

    *resolved = NULL;
    authority_length = strcspn(input, "/?#");
    authority = g_strndup(input, authority_length);
    suffix = input + authority_length;
    if (!*authority)
        return MUX_HOST_INPUT_NONE;

    if (authority[0] == '[') {
        gchar *closing = strchr(authority, ']');

        if (!closing) {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_URL,
                                "Bracketed IPv6 address is missing ']'");
            return MUX_HOST_INPUT_INVALID;
        }
        address = g_strndup(authority + 1, closing - authority - 1);
        if (!strchr(address, ':') || !g_hostname_is_ip_address(address)) {
            g_set_error(error,
                        MUX_URI_ERROR,
                        MUX_URI_ERROR_INVALID_URL,
                        "'%s' is not a valid bracketed IPv6 address",
                        address);
            return MUX_HOST_INPUT_INVALID;
        }
        if (closing[1] == ':') {
            port_text = closing + 2;
            if (!parse_port(port_text, error))
                return MUX_HOST_INPUT_INVALID;
        } else if (closing[1] != '\0') {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_URL,
                                "Unexpected text follows the bracketed IPv6 address");
            return MUX_HOST_INPUT_INVALID;
        }
        if (has_url_forbidden_character(input)) {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_URL,
                                "A URL cannot contain unescaped whitespace or control characters");
            return MUX_HOST_INPUT_INVALID;
        }
        if (port_text) {
            candidate = g_strdup_printf("http://[%s]:%s%s",
                                        address,
                                        port_text,
                                        suffix);
        } else {
            candidate = g_strdup_printf("http://[%s]%s", address, suffix);
        }
        *resolved = validate_http_url(candidate, error);
        return *resolved ? MUX_HOST_INPUT_RESOLVED : MUX_HOST_INPUT_INVALID;
    }

    if (strchr(authority, ':') &&
        g_hostname_is_ip_address(authority)) {
        if (has_url_forbidden_character(input)) {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_URL,
                                "A URL cannot contain unescaped whitespace or control characters");
            return MUX_HOST_INPUT_INVALID;
        }
        candidate = g_strdup_printf("http://[%s]%s", authority, suffix);
        *resolved = validate_http_url(candidate, error);
        return *resolved ? MUX_HOST_INPUT_RESOLVED : MUX_HOST_INPUT_INVALID;
    }

    first_colon = strchr(authority, ':');
    colon = strrchr(authority, ':');
    if (colon && colon == first_colon) {
        host = g_strndup(authority, colon - authority);
        port_text = colon + 1;
    } else {
        host = g_strdup(authority);
    }

    host_result = classify_host_name(host, &canonical_host, &prefer_http);
    if (host_result == MUX_HOST_NAME_NONE)
        return MUX_HOST_INPUT_NONE;
    if (host_result == MUX_HOST_NAME_INVALID) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_URL,
                    "Host '%s' is not a valid domain name or IP address",
                    host);
        return MUX_HOST_INPUT_INVALID;
    }
    if (port_text && !parse_port(port_text, error))
        return MUX_HOST_INPUT_INVALID;
    if (has_url_forbidden_character(input)) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_URL,
                            "A URL cannot contain unescaped whitespace or control characters");
        return MUX_HOST_INPUT_INVALID;
    }

    if (port_text) {
        candidate = g_strdup_printf("%s://%s:%s%s",
                                    prefer_http ? "http" : "https",
                                    canonical_host,
                                    port_text,
                                    suffix);
    } else {
        candidate = g_strdup_printf("%s://%s%s",
                                    prefer_http ? "http" : "https",
                                    canonical_host,
                                    suffix);
    }
    *resolved = validate_http_url(candidate, error);
    return *resolved ? MUX_HOST_INPUT_RESOLVED : MUX_HOST_INPUT_INVALID;
}

static gchar *
replace_query_marker(const gchar *search_url,
                     const gchar *replacement,
                     gboolean *had_marker,
                     GError **error)
{
    const gchar *marker;
    const gchar *second;
    gsize prefix_length;

    marker = strstr(search_url, MUX_URI_QUERY_MARKER);
    *had_marker = marker != NULL;
    if (!marker)
        return g_strconcat(search_url, replacement, NULL);

    second = strstr(marker + strlen(MUX_URI_QUERY_MARKER),
                    MUX_URI_QUERY_MARKER);
    if (second) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_SEARCH_URL,
                            "Search URL may contain at most one {query} marker");
        return NULL;
    }
    prefix_length = marker - search_url;
    return g_strdup_printf("%.*s%s%s",
                           (gint)prefix_length,
                           search_url,
                           replacement,
                           marker + strlen(MUX_URI_QUERY_MARKER));
}

static gchar *
build_search_url(const gchar *query,
                 const gchar *configured_search_url,
                 GError **error)
{
    const gchar *search_url = configured_search_url;
    const gchar *scheme;
    const gchar *authority_end;
    const gchar *marker;
    g_autofree gchar *probe = NULL;
    g_autofree gchar *escaped_query = NULL;
    g_autofree gchar *result = NULL;
    g_autofree gchar *validated = NULL;
    g_autoptr(GError) local_error = NULL;
    gboolean had_marker = FALSE;

    if (!search_url || !*search_url)
        search_url = MUX_URI_DEFAULT_SEARCH_URL;
    if (strlen(search_url) > MUX_URI_MAX_SEARCH_URL_BYTES) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "Search URL exceeds the %u-byte limit",
                    MUX_URI_MAX_SEARCH_URL_BYTES);
        return NULL;
    }

    marker = strstr(search_url, MUX_URI_QUERY_MARKER);
    probe = replace_query_marker(search_url,
                                 "mux-query",
                                 &had_marker,
                                 error);
    if (!probe)
        return NULL;
    scheme = g_uri_peek_scheme(probe);
    if (!scheme ||
        (g_ascii_strcasecmp(scheme, "http") &&
         g_ascii_strcasecmp(scheme, "https"))) {
        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_INVALID_SEARCH_URL,
                            "Search URL must use http:// or https://");
        return NULL;
    }
    if (marker) {
        const gchar *authority = strstr(search_url, "://");

        authority_end = authority ? strpbrk(authority + 3, "/?#") : NULL;
        if (!authority_end || marker < authority_end) {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_SEARCH_URL,
                                "The {query} marker must appear after the search host");
            return NULL;
        }
    }

    validated = validate_http_url(probe, &local_error);
    if (!validated) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "Search URL is invalid: %s",
                    local_error->message);
        return NULL;
    }

    escaped_query = g_uri_escape_string(query, NULL, FALSE);
    result = replace_query_marker(search_url,
                                  escaped_query,
                                  &had_marker,
                                  error);
    if (!result)
        return NULL;

    g_clear_pointer(&validated, g_free);
    g_clear_error(&local_error);
    validated = validate_http_url(result, &local_error);
    if (!validated) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_INVALID_SEARCH_URL,
                    "Resolved search URL is invalid: %s",
                    local_error->message);
        return NULL;
    }
    return g_steal_pointer(&validated);
}

gchar *
mux_uri_resolve_user_input(const gchar *input,
                           const gchar *search_url,
                           GError **error)
{
    g_autofree gchar *trimmed = NULL;
    gchar *resolved = NULL;
    const gchar *scheme;
    MuxHostInputResult host_result;

    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    trimmed = trim_input(input, error);
    if (!trimmed)
        return NULL;
    if (!*trimmed)
        return g_strdup("about:blank");

    scheme = g_uri_peek_scheme(trimmed);
    if (scheme &&
        (!g_ascii_strcasecmp(scheme, "http") ||
         !g_ascii_strcasecmp(scheme, "https")))
        return validate_http_url(trimmed, error);
    if (scheme && !g_ascii_strcasecmp(scheme, "about")) {
        if (!g_ascii_strcasecmp(trimmed, "about:blank"))
            return g_strdup("about:blank");

        g_set_error_literal(error,
                            MUX_URI_ERROR,
                            MUX_URI_ERROR_DISALLOWED_SCHEME,
                            "Only about:blank is allowed for direct about: navigation");
        return NULL;
    }

    for (const gchar *cursor = trimmed; *cursor;
         cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);

        if (g_unichar_iscntrl(character)) {
            g_set_error_literal(error,
                                MUX_URI_ERROR,
                                MUX_URI_ERROR_INVALID_INPUT,
                                "Navigation input contains a control character");
            return NULL;
        }
        if (g_unichar_isspace(character))
            return build_search_url(trimmed, search_url, error);
    }

    if (strchr(trimmed, '@')) {
        if (scheme) {
            g_set_error(error,
                        MUX_URI_ERROR,
                        MUX_URI_ERROR_DISALLOWED_SCHEME,
                        "Navigation scheme '%s' is not allowed; use http://, https://, or about:blank",
                        scheme);
            return NULL;
        }
        return build_search_url(trimmed, search_url, error);
    }

    host_result = resolve_host_input(trimmed, &resolved, error);
    if (host_result == MUX_HOST_INPUT_RESOLVED)
        return resolved;
    if (host_result == MUX_HOST_INPUT_INVALID)
        return NULL;

    if (scheme) {
        g_set_error(error,
                    MUX_URI_ERROR,
                    MUX_URI_ERROR_DISALLOWED_SCHEME,
                    "Navigation scheme '%s' is not allowed; use http://, https://, or about:blank",
                    scheme);
        return NULL;
    }

    return build_search_url(trimmed, search_url, error);
}
