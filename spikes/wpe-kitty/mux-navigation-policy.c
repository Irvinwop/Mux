#include "mux-navigation-policy.h"

#include "mux-ui-protocol.h"

enum {
    MUX_NAVIGATION_MAX_PENDING = 16,
    MUX_NAVIGATION_PROMPT_DEADLINE_MS = 120000,
    MUX_NAVIGATION_ORIGIN_LIMIT = 2048,
    MUX_NAVIGATION_MESSAGE_LIMIT = 8192,
};

typedef enum {
    PENDING_TLS,
    PENDING_EXTERNAL_URI,
} PendingKind;

typedef struct {
    guint64 request_id;
    guint64 navigation_epoch;
    guint64 tls_epoch;
    PendingKind kind;
    gchar *uri;
    gchar *host;
    GTlsCertificate *certificate;
    WebKitPolicyDecision *decision;
    gboolean decision_resolved;
} PendingDecision;

struct _MuxNavigationPolicy {
    WebKitWebView *web_view;
    gboolean private_profile;
    MuxNavigationPolicyOutputFunc output;
    gpointer output_data;
    GDestroyNotify output_destroy;
    GHashTable *pending;
    guint64 navigation_epoch;
    guint64 tls_epoch;
    gchar *navigation_uri;
};

static void
advance_epoch(guint64 *epoch)
{
    (*epoch)++;
    if (*epoch == 0)
        (*epoch)++;
}

static gchar *
bounded_utf8(const gchar *text, gsize limit)
{
    gchar *valid = g_utf8_make_valid(text ? text : "", -1);
    gsize length = strlen(valid);
    gchar *end;

    if (length <= limit)
        return valid;
    end = valid + limit;
    while (end > valid && (((guchar)*end & 0xc0U) == 0x80U))
        end--;
    *end = '\0';
    return valid;
}

static void
pending_decision_free(gpointer data)
{
    PendingDecision *pending = data;

    if (!pending)
        return;
    if (pending->decision && !pending->decision_resolved)
        webkit_policy_decision_ignore(pending->decision);
    g_clear_object(&pending->decision);
    g_clear_object(&pending->certificate);
    g_free(pending->uri);
    g_free(pending->host);
    g_free(pending);
}

static void
invalidate_pending_tls(MuxNavigationPolicy *policy)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, policy->pending);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        PendingDecision *pending = value;

        if (pending->kind == PENDING_TLS)
            g_hash_table_iter_remove(&iter);
    }
}

static void
navigation_changed(MuxNavigationPolicy *policy, const gchar *uri)
{
    advance_epoch(&policy->navigation_epoch);
    g_free(policy->navigation_uri);
    policy->navigation_uri = g_strdup(uri);
    invalidate_pending_tls(policy);
}

static void
load_changed(WebKitWebView *web_view,
             WebKitLoadEvent load_event,
             gpointer data)
{
    MuxNavigationPolicy *policy = data;

    if (load_event == WEBKIT_LOAD_STARTED ||
        load_event == WEBKIT_LOAD_REDIRECTED)
        navigation_changed(policy, webkit_web_view_get_uri(web_view));
}

static guint64
next_request_id(MuxNavigationPolicy *policy)
{
    guint64 request_id;

    do {
        request_id = UINT64_C(0x4e00000000000000) |
                     ((((guint64)g_random_int() << 32) | g_random_int()) &
                      UINT64_C(0x00ffffffffffffff));
    } while (g_hash_table_contains(policy->pending, &request_id));
    return request_id;
}

static gboolean
send_request(MuxNavigationPolicy *policy,
             PendingDecision *pending,
             MuxUiRequestKind kind,
             guint32 flags,
             const gchar *heading,
             const gchar *message,
             GError **error)
{
    g_autoptr(MuxUiRequest) request = mux_ui_request_new(kind);
    g_autoptr(GBytes) payload = NULL;
    const gchar *current_uri = webkit_web_view_get_uri(policy->web_view);
    const gchar *origin = pending->kind == PENDING_TLS
                              ? pending->uri
                              : current_uri;

    request->request_id = pending->request_id;
    request->flags = flags;
    if (policy->private_profile)
        request->flags |= MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE;
    request->deadline_ms = MUX_NAVIGATION_PROMPT_DEADLINE_MS;
    request->origin = bounded_utf8(origin ? origin : "about:blank",
                                   MUX_NAVIGATION_ORIGIN_LIMIT);
    request->heading = g_strdup(heading);
    request->message = bounded_utf8(message, MUX_NAVIGATION_MESSAGE_LIMIT);
    payload = mux_ui_request_encode(request, error);
    if (!payload)
        return FALSE;
    return policy->output(payload, policy->output_data, error);
}

static gchar *
tls_error_summary(GTlsCertificateFlags errors)
{
    GString *summary = g_string_new(NULL);
    GTlsCertificateFlags known = 0;

#define APPEND_TLS_ERROR(flag, text)                                            \
    G_STMT_START                                                               \
    {                                                                          \
        known |= (flag);                                                       \
        if (errors & (flag)) {                                                 \
            if (summary->len)                                                  \
                g_string_append(summary, ", ");                               \
            g_string_append(summary, (text));                                  \
        }                                                                      \
    }                                                                          \
    G_STMT_END

    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_UNKNOWN_CA, "unknown certificate authority");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_BAD_IDENTITY, "host name mismatch");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_NOT_ACTIVATED, "certificate not yet valid");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_EXPIRED, "certificate expired");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_REVOKED, "certificate revoked");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_INSECURE, "insecure certificate algorithm");
    APPEND_TLS_ERROR(G_TLS_CERTIFICATE_GENERIC_ERROR, "certificate validation error");
#undef APPEND_TLS_ERROR

    if (errors & ~known) {
        if (summary->len)
            g_string_append(summary, ", ");
        g_string_append(summary, "unrecognized certificate error");
    }
    if (!summary->len)
        g_string_append(summary, "unspecified certificate error");
    return g_string_free(summary, FALSE);
}

static gboolean
load_failed_with_tls_errors(WebKitWebView *web_view,
                            gchar *failing_uri,
                            GTlsCertificate *certificate,
                            GTlsCertificateFlags errors,
                            gpointer data)
{
    MuxNavigationPolicy *policy = data;
    g_autoptr(GUri) parsed = NULL;
    g_autofree gchar *error_summary = NULL;
    g_autofree gchar *message = NULL;
    g_autoptr(GError) send_error = NULL;
    PendingDecision *pending;
    const gchar *scheme;
    const gchar *host;
    guint64 *key;

    (void)web_view;
    parsed = g_uri_parse(failing_uri, G_URI_FLAGS_NONE, NULL);
    if (!parsed)
        return FALSE;
    scheme = g_uri_get_scheme(parsed);
    host = g_uri_get_host(parsed);
    if (!scheme || g_ascii_strcasecmp(scheme, "https") != 0 || !host || !*host)
        return FALSE;

    invalidate_pending_tls(policy);
    advance_epoch(&policy->tls_epoch);
    if (g_hash_table_size(policy->pending) >= MUX_NAVIGATION_MAX_PENDING)
        return FALSE;

    g_free(policy->navigation_uri);
    policy->navigation_uri = g_strdup(failing_uri);

    pending = g_new0(PendingDecision, 1);
    pending->request_id = next_request_id(policy);
    pending->navigation_epoch = policy->navigation_epoch;
    pending->tls_epoch = policy->tls_epoch;
    pending->kind = PENDING_TLS;
    pending->uri = g_strdup(failing_uri);
    pending->host = g_strdup(host);
    pending->certificate = g_object_ref(certificate);
    key = g_new(guint64, 1);
    *key = pending->request_id;
    g_hash_table_insert(policy->pending, key, pending);

    error_summary = tls_error_summary(errors);
    message = g_strdup_printf(
        "The certificate for %s failed validation: %s. "
        "Proceeding trusts this certificate for this engine session.",
        host,
        error_summary);
    if (!send_request(policy,
                      pending,
                      MUX_UI_REQUEST_PERMISSION,
                      MUX_UI_REQUEST_FLAG_DANGER,
                      "TLS certificate error",
                      message,
                      &send_error)) {
        g_hash_table_remove(policy->pending, &pending->request_id);
        return FALSE;
    }
    return TRUE;
}

static gboolean
scheme_is_internal(const gchar *scheme)
{
    static const gchar *const internal_schemes[] = {
        "about", "blob", "data", "file", "http", "https",
        "javascript", "ws", "wss", NULL,
    };
    guint i;

    for (i = 0; internal_schemes[i]; i++) {
        if (g_ascii_strcasecmp(scheme, internal_schemes[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
decide_policy(WebKitWebView *web_view,
              WebKitPolicyDecision *decision,
              WebKitPolicyDecisionType decision_type,
              gpointer data)
{
    MuxNavigationPolicy *policy = data;
    WebKitNavigationAction *navigation;
    WebKitURIRequest *request;
    const gchar *uri;
    const gchar *scheme;
    PendingDecision *pending;
    guint64 *key;
    guint32 flags = 0;
    g_autofree gchar *message = NULL;
    g_autoptr(GError) send_error = NULL;

    (void)web_view;
    if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        decision_type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;
    if (!WEBKIT_IS_NAVIGATION_POLICY_DECISION(decision))
        return FALSE;
    navigation = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    request = webkit_navigation_action_get_request(navigation);
    uri = request ? webkit_uri_request_get_uri(request) : NULL;
    if (decision_type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        navigation_changed(policy, uri);
    scheme = uri ? g_uri_peek_scheme(uri) : NULL;
    if (!scheme || scheme_is_internal(scheme))
        return FALSE;

    if (g_hash_table_size(policy->pending) >= MUX_NAVIGATION_MAX_PENDING) {
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    pending = g_new0(PendingDecision, 1);
    pending->request_id = next_request_id(policy);
    pending->kind = PENDING_EXTERNAL_URI;
    pending->uri = g_strdup(uri);
    pending->decision = g_object_ref(decision);
    if (webkit_navigation_action_is_user_gesture(navigation))
        flags |= MUX_UI_REQUEST_FLAG_USER_GESTURE;
    key = g_new(guint64, 1);
    *key = pending->request_id;
    g_hash_table_insert(policy->pending, key, pending);

    message = g_strdup_printf(
        "Open this %s link with the system's registered application?\n%s",
        scheme,
        uri);
    if (!send_request(policy,
                      pending,
                      MUX_UI_REQUEST_DIALOG_CONFIRM,
                      flags,
                      "Open external application?",
                      message,
                      &send_error)) {
        pending->decision_resolved = TRUE;
        webkit_policy_decision_ignore(pending->decision);
        g_hash_table_remove(policy->pending, &pending->request_id);
    }
    return TRUE;
}

static void
external_launch_finished(GObject *source_object,
                         GAsyncResult *result,
                         gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    (void)source_object;
    (void)user_data;
    if (!g_app_info_launch_default_for_uri_finish(result, &error))
        g_warning("external URI launch failed");
}

MuxNavigationPolicy *
mux_navigation_policy_new(WebKitWebView *web_view,
                          gboolean private_profile,
                          MuxNavigationPolicyOutputFunc output,
                          gpointer output_data,
                          GDestroyNotify output_destroy)
{
    MuxNavigationPolicy *policy;

    g_return_val_if_fail(WEBKIT_IS_WEB_VIEW(web_view), NULL);
    g_return_val_if_fail(output, NULL);
    policy = g_new0(MuxNavigationPolicy, 1);
    policy->web_view = g_object_ref(web_view);
    policy->private_profile = private_profile;
    policy->output = output;
    policy->output_data = output_data;
    policy->output_destroy = output_destroy;
    policy->pending = g_hash_table_new_full(g_int64_hash,
                                            g_int64_equal,
                                            g_free,
                                            pending_decision_free);
    policy->navigation_epoch = 1;
    policy->tls_epoch = 1;
    policy->navigation_uri = g_strdup(webkit_web_view_get_uri(web_view));
    g_signal_connect(web_view,
                     "load-changed",
                     G_CALLBACK(load_changed),
                     policy);
    g_signal_connect(web_view,
                     "load-failed-with-tls-errors",
                     G_CALLBACK(load_failed_with_tls_errors),
                     policy);
    g_signal_connect(web_view,
                     "decide-policy",
                     G_CALLBACK(decide_policy),
                     policy);
    return policy;
}

gboolean
mux_navigation_policy_handle_payload(MuxNavigationPolicy *policy,
                                     const guint8 *data,
                                     gsize length,
                                     GError **error)
{
    MuxUiRecordType record_type;
    g_autoptr(MuxUiResponse) response = NULL;
    PendingDecision *pending;

    g_return_val_if_fail(policy, FALSE);
    if (!mux_ui_record_type(data, length, &record_type, error))
        return FALSE;
    if (record_type != MUX_UI_RECORD_RESPONSE)
        return TRUE;
    if (!mux_ui_response_decode(data, length, &response, error))
        return FALSE;
    pending = g_hash_table_lookup(policy->pending, &response->request_id);
    if (!pending)
        return TRUE;

    if (pending->kind == PENDING_TLS) {
        g_autoptr(GTlsCertificate) certificate = NULL;
        g_autofree gchar *host = NULL;
        g_autofree gchar *uri = NULL;
        g_autoptr(GUri) parsed = NULL;
        const gchar *parsed_host;

        parsed = g_uri_parse(pending->uri, G_URI_FLAGS_NONE, NULL);
        parsed_host = parsed ? g_uri_get_host(parsed) : NULL;
        if (pending->navigation_epoch != policy->navigation_epoch ||
            pending->tls_epoch != policy->tls_epoch ||
            g_strcmp0(pending->uri, policy->navigation_uri) != 0 ||
            !parsed_host ||
            g_ascii_strcasecmp(parsed_host, pending->host) != 0) {
            g_hash_table_remove(policy->pending, &response->request_id);
            return TRUE;
        }
        if (response->action == MUX_UI_ACTION_ALLOW_ONCE ||
            response->action == MUX_UI_ACTION_ALLOW_ALWAYS) {
            WebKitNetworkSession *session =
                webkit_web_view_get_network_session(policy->web_view);

            certificate = g_object_ref(pending->certificate);
            host = g_strdup(pending->host);
            uri = g_strdup(pending->uri);
            g_hash_table_remove(policy->pending, &response->request_id);
            webkit_network_session_allow_tls_certificate_for_host(
                session,
                certificate,
                host);
            webkit_web_view_load_uri(policy->web_view, uri);
            return TRUE;
        }
    } else {
        pending->decision_resolved = TRUE;
        webkit_policy_decision_ignore(pending->decision);
        if (response->action == MUX_UI_ACTION_ACCEPT)
            g_app_info_launch_default_for_uri_async(pending->uri,
                                                    NULL,
                                                    NULL,
                                                    external_launch_finished,
                                                    NULL);
    }
    g_hash_table_remove(policy->pending, &response->request_id);
    return TRUE;
}

void
mux_navigation_policy_free(MuxNavigationPolicy *policy)
{
    if (!policy)
        return;
    g_signal_handlers_disconnect_by_data(policy->web_view, policy);
    g_clear_pointer(&policy->pending, g_hash_table_unref);
    if (policy->output_destroy)
        policy->output_destroy(policy->output_data);
    g_free(policy->navigation_uri);
    g_clear_object(&policy->web_view);
    g_free(policy);
}
