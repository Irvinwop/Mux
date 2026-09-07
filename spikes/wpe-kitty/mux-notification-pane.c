#include "mux-notification-pane.h"

#include <string.h>

#define MUX_NOTIFICATION_OSC_CHUNK 2048U
#define MUX_NOTIFICATION_PANE_MAX_PENDING 64U

typedef struct {
    grefcount references;
    guint64 request_id;
    gboolean clicked;
} PaneNotification;

static PaneNotification *
pane_notification_ref(PaneNotification *notification)
{
    g_ref_count_inc(&notification->references);
    return notification;
}

static void
pane_notification_unref(PaneNotification *notification)
{
    if (g_ref_count_dec(&notification->references))
        g_free(notification);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(PaneNotification, pane_notification_unref)

struct _MuxNotificationPane {
    gatomicrefcount references;
    GHashTable *pending;
    MuxNotificationPaneSendFunc send_func;
    MuxNotificationPaneWriteFunc write_func;
    gpointer user_data;
    GDestroyNotify user_data_destroy;
    gboolean disposing;
};

static MuxNotificationPane *
notification_pane_ref(MuxNotificationPane *pane)
{
    g_atomic_ref_count_inc(&pane->references);
    return pane;
}

static void
notification_pane_unref(MuxNotificationPane *pane)
{
    if (!g_atomic_ref_count_dec(&pane->references))
        return;
    g_clear_pointer(&pane->pending, g_hash_table_unref);
    if (pane->user_data_destroy)
        pane->user_data_destroy(pane->user_data);
    g_free(pane);
}

typedef MuxNotificationPane MuxNotificationPaneGuard;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNotificationPaneGuard,
                              notification_pane_unref)

static guint64 *
request_key_new(guint64 request_id)
{
    guint64 *key = g_new(guint64, 1);

    *key = request_id;
    return key;
}

static gchar *
notification_identifier(guint64 request_id)
{
    return g_strdup_printf("muxn-%016" G_GINT64_MODIFIER "x",
                           request_id);
}

static gboolean
write_sequence(MuxNotificationPane *pane,
               const gchar *sequence,
               GError **error)
{
    gboolean written = pane->write_func &&
        pane->write_func((const guint8 *)sequence,
                         strlen(sequence),
                         pane->user_data,
                         error);

    return written && !pane->disposing;
}

static gboolean
write_chunk(MuxNotificationPane *pane,
            const gchar *identifier,
            const gchar *part,
            const guint8 *data,
            gsize length,
            gboolean first,
            gboolean done,
            GError **error)
{
    g_autofree gchar *encoded = g_base64_encode(data, length);
    g_autofree gchar *sequence = g_strdup_printf(
        "\033]99;i=%s:d=%u:e=1:p=%s%s;%s\033\\",
        identifier,
        done ? 1U : 0U,
        part,
        first ? ":a=report,focus:c=1:f=TXV4:t=d2Vi:o=always" : "",
        encoded);

    return write_sequence(pane, sequence, error);
}

static gboolean
write_part(MuxNotificationPane *pane,
           const gchar *identifier,
           PaneNotification *pending,
           const gchar *part,
           const gchar *text,
           gboolean *first,
           gboolean final_part,
           GError **error)
{
    const guint8 *bytes = (const guint8 *)(text ? text : "");
    gsize length = strlen((const gchar *)bytes);
    gsize offset = 0;

    if (!length)
        return TRUE;
    while (offset < length) {
        gsize chunk = MIN((gsize)MUX_NOTIFICATION_OSC_CHUNK,
                          length - offset);
        gboolean done = final_part && offset + chunk == length;

        if (g_hash_table_lookup(pane->pending,
                                &pending->request_id) != pending)
            return FALSE;
        if (!write_chunk(pane,
                         identifier,
                         part,
                         bytes + offset,
                         chunk,
                         *first,
                         done,
                         error))
            return FALSE;
        *first = FALSE;
        offset += chunk;
    }
    return TRUE;
}

static gboolean
show_notification(MuxNotificationPane *pane,
                  const MuxUiRequest *request,
                  PaneNotification *pending,
                  GError **error)
{
    g_autofree gchar *identifier =
        notification_identifier(request->request_id);
    const gchar *title = request->heading && *request->heading
                             ? request->heading
                             : request->origin;
    const gchar *body = request->message;
    gboolean first = TRUE;
    gboolean has_body = body && *body;

    if (!title || !*title)
        title = "Web notification";
    if (!write_part(pane,
                    identifier,
                    pending,
                    "title",
                    title,
                    &first,
                    !has_body,
                    error))
        return FALSE;
    if (has_body &&
        !write_part(pane,
                    identifier,
                    pending,
                    "body",
                    body,
                    &first,
                    TRUE,
                    error))
        return FALSE;
    return TRUE;
}

static gboolean
close_notification(MuxNotificationPane *pane,
                   guint64 request_id,
                   GError **error)
{
    g_autofree gchar *identifier =
        notification_identifier(request_id);
    g_autofree gchar *sequence = g_strdup_printf(
        "\033]99;i=%s:p=close;\033\\", identifier);

    return write_sequence(pane, sequence, error);
}

static gboolean
send_action(MuxNotificationPane *pane,
            guint64 request_id,
            MuxUiAction action,
            GError **error)
{
    if (pane->disposing)
        return FALSE;

    g_autoptr(MuxUiResponse) response =
        mux_ui_response_new(request_id, action);
    g_autoptr(GBytes) payload = mux_ui_response_encode(response, error);

    gboolean sent = payload && pane->send_func &&
        pane->send_func(payload, pane->user_data, error);

    return sent && !pane->disposing;
}

MuxNotificationPane *
mux_notification_pane_new(MuxNotificationPaneSendFunc send_func,
                          MuxNotificationPaneWriteFunc write_func,
                          gpointer user_data,
                          GDestroyNotify user_data_destroy)
{
    MuxNotificationPane *pane;

    g_return_val_if_fail(send_func, NULL);
    g_return_val_if_fail(write_func, NULL);
    pane = g_new0(MuxNotificationPane, 1);
    g_atomic_ref_count_init(&pane->references);
    pane->pending = g_hash_table_new_full(g_int64_hash,
                                         g_int64_equal,
                                         g_free,
                                         (GDestroyNotify)pane_notification_unref);
    pane->send_func = send_func;
    pane->write_func = write_func;
    pane->user_data = user_data;
    pane->user_data_destroy = user_data_destroy;
    return pane;
}

void
mux_notification_pane_free(MuxNotificationPane *pane)
{
    GHashTableIter iterator;
    gpointer value;

    if (!pane || pane->disposing)
        return;
    pane->disposing = TRUE;
    g_hash_table_iter_init(&iterator, pane->pending);
    while (g_hash_table_iter_next(&iterator, NULL, &value)) {
        PaneNotification *pending = value;

        (void)close_notification(pane, pending->request_id, NULL);
    }
    notification_pane_unref(pane);
}

gboolean
mux_notification_pane_handle_payload(MuxNotificationPane *pane,
                                     const guint8 *data,
                                     gsize length,
                                     gboolean *consumed,
                                     GError **error)
{
    g_autoptr(MuxNotificationPaneGuard) guard = NULL;
    MuxUiRecordType type;

    g_return_val_if_fail(pane, FALSE);
    guard = notification_pane_ref(pane);
    if (pane->disposing)
        return FALSE;
    g_return_val_if_fail(consumed, FALSE);
    *consumed = FALSE;
    if (!mux_ui_record_type(data, length, &type, error))
        return FALSE;

    if (type == MUX_UI_RECORD_REQUEST) {
        g_autoptr(MuxUiRequest) request = NULL;
        g_autoptr(PaneNotification) pending = NULL;

        if (!mux_ui_request_decode(data, length, &request, error))
            return FALSE;
        if (request->kind != MUX_UI_REQUEST_NOTIFICATION)
            return TRUE;
        *consumed = TRUE;
        if (request->flags & MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE)
            return send_action(pane,
                               request->request_id,
                               MUX_UI_ACTION_UNSUPPORTED,
                               error);
        if (!g_hash_table_contains(pane->pending, &request->request_id) &&
            g_hash_table_size(pane->pending) >=
                MUX_NOTIFICATION_PANE_MAX_PENDING)
            return send_action(pane,
                               request->request_id,
                               MUX_UI_ACTION_UNSUPPORTED,
                               error);
        pending = g_new0(PaneNotification, 1);
        g_ref_count_init(&pending->references);
        pending->request_id = request->request_id;
        /* Publication is visible to reentrant cancellation and disposal.
         * Retain this exact entry so a replacement cannot reuse its address. */
        g_hash_table_replace(pane->pending,
                             request_key_new(pending->request_id),
                             pane_notification_ref(pending));
        if (!show_notification(pane, request, pending, error)) {
            if (pane->disposing)
                return FALSE;
            if (g_hash_table_lookup(pane->pending,
                                    &pending->request_id) != pending)
                return TRUE;
            g_hash_table_remove(pane->pending, &pending->request_id);
            /* Preserve the terminal error. A best-effort engine response
             * must not pass an already populated GError to the encoder. */
            (void)send_action(pane,
                              request->request_id,
                              MUX_UI_ACTION_UNSUPPORTED,
                              NULL);
            return FALSE;
        }
        return TRUE;
    }

    if (type == MUX_UI_RECORD_CANCEL) {
        guint64 request_id = 0;
        MuxUiCancelReason reason;

        if (!mux_ui_cancel_decode(data,
                                  length,
                                  &request_id,
                                  &reason,
                                  error))
            return FALSE;
        (void)reason;
        if (!g_hash_table_remove(pane->pending, &request_id))
            return TRUE;
        *consumed = TRUE;
        return close_notification(pane, request_id, error);
    }
    return TRUE;
}

static gboolean
parse_identifier(const gchar *value, guint64 *request_id)
{
    const gchar *digits;
    gchar *end = NULL;
    guint64 parsed;
    guint i;

    if (!g_str_has_prefix(value, "muxn-") || strlen(value) != 21)
        return FALSE;
    digits = value + 5;
    for (i = 0; i < 16; i++) {
        if (!g_ascii_isxdigit(digits[i]))
            return FALSE;
    }
    parsed = g_ascii_strtoull(digits, &end, 16);
    if (!parsed || !end || *end)
        return FALSE;
    *request_id = parsed;
    return TRUE;
}

gboolean
mux_notification_pane_handle_osc(MuxNotificationPane *pane,
                                 const guint8 *data,
                                 gsize length,
                                 GError **error)
{
    g_autoptr(MuxNotificationPaneGuard) guard = NULL;
    const guint8 *metadata_end;
    g_autofree gchar *metadata = NULL;
    g_auto(GStrv) fields = NULL;
    const gchar *identifier = NULL;
    const gchar *part = NULL;
    guint64 request_id;
    PaneNotification *pending;
    guint i;

    g_return_val_if_fail(pane, FALSE);
    guard = notification_pane_ref(pane);
    if (pane->disposing)
        return FALSE;
    if (!data || length < 8 || memcmp(data, "\033]99;", 5) != 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "invalid Kitty notification response");
        return FALSE;
    }
    metadata_end = memchr(data + 5, ';', length - 5);
    if (!metadata_end) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Kitty notification response has no payload separator");
        return FALSE;
    }
    metadata = g_strndup((const gchar *)data + 5,
                         metadata_end - (data + 5));
    fields = g_strsplit(metadata, ":", -1);
    for (i = 0; fields[i]; i++) {
        if (g_str_has_prefix(fields[i], "i="))
            identifier = fields[i] + 2;
        else if (g_str_has_prefix(fields[i], "p="))
            part = fields[i] + 2;
    }
    if (!identifier || !parse_identifier(identifier, &request_id))
        return TRUE;
    pending = g_hash_table_lookup(pane->pending, &request_id);
    if (!pending)
        return TRUE;

    if (g_strcmp0(part, "close") == 0) {
        g_hash_table_remove(pane->pending, &request_id);
        return send_action(pane,
                            request_id,
                            MUX_UI_ACTION_CANCEL,
                            error);
    } else if (!pending->clicked) {
        pending->clicked = TRUE;
        if (!send_action(pane,
                         request_id,
                         MUX_UI_ACTION_ACKNOWLEDGE,
                         error))
            return FALSE;
    }
    return TRUE;
}

guint
mux_notification_pane_pending_count(const MuxNotificationPane *pane)
{
    g_return_val_if_fail(pane, 0);
    return g_hash_table_size(pane->pending);
}
