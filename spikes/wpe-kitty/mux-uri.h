#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MUX_URI_DEFAULT_SEARCH_URL "https://duckduckgo.com/?q={query}"
#define MUX_URI_SEARCH_ENV "MUX_SEARCH_URL"

#define MUX_URI_ERROR (mux_uri_error_quark())

typedef enum {
    MUX_URI_ERROR_INVALID_INPUT,
    MUX_URI_ERROR_DISALLOWED_SCHEME,
    MUX_URI_ERROR_INVALID_URL,
    MUX_URI_ERROR_INVALID_SEARCH_URL,
} MuxUriError;

GQuark mux_uri_error_quark(void);

/*
 * Resolves address-bar or control-plane input only. Page-initiated navigation,
 * including blob: URLs owned by WebKit, must not pass through this function.
 *
 * search_url may contain one {query} marker. If it has no marker, the escaped
 * query is appended. NULL and the empty string select DuckDuckGo.
 */
gchar *mux_uri_resolve_user_input(const gchar *input,
                                  const gchar *search_url,
                                  GError **error);

G_END_DECLS
