#include "mux-ui-protocol.h"

#include <stdarg.h>
#include <string.h>

#define MUX_UI_MAGIC 0x4d555549U
#define MUX_UI_MAX_ORIGIN 2048U
#define MUX_UI_MAX_HEADING 512U
#define MUX_UI_MAX_CHOICE_LABEL 1024U

typedef struct {
    const guint8 *data;
    gsize length;
    gsize offset;
} MuxUiReader;

GQuark
mux_ui_error_quark(void)
{
    return g_quark_from_static_string("mux-ui-error-quark");
}

static gboolean
fail(GError **error, MuxUiError code, const gchar *format, ...)
{
    if (error) {
        va_list args;
        gchar *message;

        va_start(args, format);
        message = g_strdup_vprintf(format, args);
        va_end(args);
        g_set_error_literal(error, MUX_UI_ERROR, code, message);
        g_free(message);
    }
    return FALSE;
}

static void
append_u16(GByteArray *bytes, guint16 value)
{
    guint16 encoded = GUINT16_TO_BE(value);
    g_byte_array_append(bytes, (const guint8 *)&encoded, sizeof(encoded));
}

static void
append_u32(GByteArray *bytes, guint32 value)
{
    guint32 encoded = GUINT32_TO_BE(value);
    g_byte_array_append(bytes, (const guint8 *)&encoded, sizeof(encoded));
}

static void
append_u64(GByteArray *bytes, guint64 value)
{
    guint64 encoded = GUINT64_TO_BE(value);
    g_byte_array_append(bytes, (const guint8 *)&encoded, sizeof(encoded));
}

static void
append_header(GByteArray *bytes, MuxUiRecordType type)
{
    append_u32(bytes, MUX_UI_MAGIC);
    append_u16(bytes, MUX_UI_WIRE_VERSION);
    append_u16(bytes, type);
}

static void
append_string(GByteArray *bytes, const gchar *value)
{
    gsize length = value ? strlen(value) : 0;

    append_u32(bytes, (guint32)length);
    if (length)
        g_byte_array_append(bytes, (const guint8 *)value, length);
}

static gboolean
reader_take(MuxUiReader *reader,
            gsize amount,
            const guint8 **value,
            GError **error)
{
    if (amount > reader->length - reader->offset)
        return fail(error, MUX_UI_ERROR_TRUNCATED, "truncated UI payload");
    *value = reader->data + reader->offset;
    reader->offset += amount;
    return TRUE;
}

static gboolean
reader_u16(MuxUiReader *reader, guint16 *value, GError **error)
{
    const guint8 *raw;
    guint16 encoded;

    if (!reader_take(reader, sizeof(encoded), &raw, error))
        return FALSE;
    memcpy(&encoded, raw, sizeof(encoded));
    *value = GUINT16_FROM_BE(encoded);
    return TRUE;
}

static gboolean
reader_u32(MuxUiReader *reader, guint32 *value, GError **error)
{
    const guint8 *raw;
    guint32 encoded;

    if (!reader_take(reader, sizeof(encoded), &raw, error))
        return FALSE;
    memcpy(&encoded, raw, sizeof(encoded));
    *value = GUINT32_FROM_BE(encoded);
    return TRUE;
}

static gboolean
reader_u64(MuxUiReader *reader, guint64 *value, GError **error)
{
    const guint8 *raw;
    guint64 encoded;

    if (!reader_take(reader, sizeof(encoded), &raw, error))
        return FALSE;
    memcpy(&encoded, raw, sizeof(encoded));
    *value = GUINT64_FROM_BE(encoded);
    return TRUE;
}

static gboolean
reader_string(MuxUiReader *reader,
              gsize maximum,
              const gchar *field,
              gchar **value,
              GError **error)
{
    guint32 length;
    const guint8 *raw;

    if (!reader_u32(reader, &length, error))
        return FALSE;
    if (length > maximum)
        return fail(error,
                    MUX_UI_ERROR_TOO_LARGE,
                    "%s exceeds its %zu-byte limit",
                    field,
                    maximum);
    if (!reader_take(reader, length, &raw, error))
        return FALSE;
    if (memchr(raw, '\0', length))
        return fail(error, MUX_UI_ERROR_INVALID, "%s contains NUL", field);
    if (!g_utf8_validate((const gchar *)raw, length, NULL))
        return fail(error, MUX_UI_ERROR_INVALID, "%s is not valid UTF-8", field);
    *value = g_strndup((const gchar *)raw, length);
    return TRUE;
}

static gboolean
reader_header(MuxUiReader *reader,
              MuxUiRecordType expected,
              GError **error)
{
    guint32 magic;
    guint16 version;
    guint16 type;

    if (reader->length > MUX_UI_MAX_PAYLOAD)
        return fail(error, MUX_UI_ERROR_TOO_LARGE, "UI payload exceeds 256 KiB");
    if (!reader_u32(reader, &magic, error) ||
        !reader_u16(reader, &version, error) ||
        !reader_u16(reader, &type, error))
        return FALSE;
    if (magic != MUX_UI_MAGIC)
        return fail(error, MUX_UI_ERROR_INVALID, "invalid UI payload magic");
    if (version != MUX_UI_WIRE_VERSION)
        return fail(error,
                    MUX_UI_ERROR_UNSUPPORTED,
                    "unsupported UI payload version %u",
                    version);
    if (type != expected)
        return fail(error,
                    MUX_UI_ERROR_INVALID,
                    "unexpected UI payload record type %u",
                    type);
    return TRUE;
}

static gboolean
reader_finished(const MuxUiReader *reader, GError **error)
{
    if (reader->offset != reader->length)
        return fail(error,
                    MUX_UI_ERROR_INVALID,
                    "UI payload has %zu trailing bytes",
                    reader->length - reader->offset);
    return TRUE;
}

static gboolean
validate_string(const gchar *value,
                gsize maximum,
                const gchar *field,
                GError **error)
{
    gsize length = value ? strlen(value) : 0;

    if (length > maximum)
        return fail(error,
                    MUX_UI_ERROR_TOO_LARGE,
                    "%s exceeds its %zu-byte limit",
                    field,
                    maximum);
    if (value && !g_utf8_validate(value, length, NULL))
        return fail(error, MUX_UI_ERROR_INVALID, "%s is not valid UTF-8", field);
    return TRUE;
}

static gboolean
request_kind_valid(MuxUiRequestKind kind)
{
    return kind >= MUX_UI_REQUEST_DIALOG_ALERT &&
           kind <= MUX_UI_REQUEST_NOTIFICATION;
}

static gboolean
action_valid(MuxUiAction action)
{
    return action >= MUX_UI_ACTION_ACKNOWLEDGE &&
           action <= MUX_UI_ACTION_UNSUPPORTED;
}

static gboolean
cancel_reason_valid(MuxUiCancelReason reason)
{
    return reason >= MUX_UI_CANCEL_UNDERLYING_GONE &&
           reason <= MUX_UI_CANCEL_SUPERSEDED;
}

static const guint32 request_flags_all =
    MUX_UI_REQUEST_FLAG_MULTIPLE |
    MUX_UI_REQUEST_FLAG_USER_GESTURE |
    MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE |
    MUX_UI_REQUEST_FLAG_PASSWORD |
    MUX_UI_REQUEST_FLAG_DANGER |
    MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE;

static const guint32 choice_flags_all =
    MUX_UI_CHOICE_FLAG_DISABLED |
    MUX_UI_CHOICE_FLAG_SEPARATOR |
    MUX_UI_CHOICE_FLAG_SELECTED |
    MUX_UI_CHOICE_FLAG_DANGER;

MuxUiChoice *
mux_ui_choice_new(guint32 id, guint32 flags, const gchar *label)
{
    MuxUiChoice *choice = g_new0(MuxUiChoice, 1);

    choice->id = id;
    choice->flags = flags;
    choice->label = g_strdup(label ? label : "");
    return choice;
}

MuxUiChoice *
mux_ui_choice_copy(const MuxUiChoice *choice)
{
    if (!choice)
        return NULL;
    return mux_ui_choice_new(choice->id, choice->flags, choice->label);
}

void
mux_ui_choice_free(MuxUiChoice *choice)
{
    if (!choice)
        return;
    g_free(choice->label);
    g_free(choice);
}

MuxUiRequest *
mux_ui_request_new(MuxUiRequestKind kind)
{
    MuxUiRequest *request = g_new0(MuxUiRequest, 1);

    request->kind = kind;
    request->choices = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mux_ui_choice_free);
    return request;
}

MuxUiRequest *
mux_ui_request_copy(const MuxUiRequest *request)
{
    MuxUiRequest *copy;
    guint i;

    if (!request)
        return NULL;
    copy = mux_ui_request_new(request->kind);
    copy->request_id = request->request_id;
    copy->flags = request->flags;
    copy->deadline_ms = request->deadline_ms;
    copy->origin = g_strdup(request->origin);
    copy->heading = g_strdup(request->heading);
    copy->message = g_strdup(request->message);
    copy->default_value = g_strdup(request->default_value);
    if (request->choices) {
        for (i = 0; i < request->choices->len; i++)
            g_ptr_array_add(copy->choices,
                            mux_ui_choice_copy(g_ptr_array_index(
                                request->choices, i)));
    }
    return copy;
}

void
mux_ui_request_free(MuxUiRequest *request)
{
    if (!request)
        return;
    g_free(request->origin);
    g_free(request->heading);
    g_free(request->message);
    g_free(request->default_value);
    g_clear_pointer(&request->choices, g_ptr_array_unref);
    g_free(request);
}

MuxUiResponse *
mux_ui_response_new(guint64 request_id, MuxUiAction action)
{
    MuxUiResponse *response = g_new0(MuxUiResponse, 1);

    response->request_id = request_id;
    response->action = action;
    response->paths = g_ptr_array_new_with_free_func(g_free);
    return response;
}

MuxUiResponse *
mux_ui_response_copy(const MuxUiResponse *response)
{
    MuxUiResponse *copy;
    guint i;

    if (!response)
        return NULL;
    copy = mux_ui_response_new(response->request_id, response->action);
    copy->value = g_strdup(response->value);
    if (response->paths) {
        for (i = 0; i < response->paths->len; i++)
            g_ptr_array_add(copy->paths,
                            g_strdup(g_ptr_array_index(response->paths, i)));
    }
    return copy;
}

void
mux_ui_response_free(MuxUiResponse *response)
{
    if (!response)
        return;
    g_free(response->value);
    g_clear_pointer(&response->paths, g_ptr_array_unref);
    g_free(response);
}

gboolean
mux_ui_action_is_valid(MuxUiRequestKind kind, MuxUiAction action)
{
    if (action == MUX_UI_ACTION_UNSUPPORTED)
        return request_kind_valid(kind);

    switch (kind) {
    case MUX_UI_REQUEST_DIALOG_ALERT:
        return action == MUX_UI_ACTION_ACKNOWLEDGE;
    case MUX_UI_REQUEST_DIALOG_CONFIRM:
        return action == MUX_UI_ACTION_ACCEPT ||
               action == MUX_UI_ACTION_CANCEL;
    case MUX_UI_REQUEST_DIALOG_PROMPT:
        return action == MUX_UI_ACTION_SUBMIT ||
               action == MUX_UI_ACTION_CANCEL;
    case MUX_UI_REQUEST_DIALOG_BEFORE_UNLOAD:
        return action == MUX_UI_ACTION_LEAVE ||
               action == MUX_UI_ACTION_STAY;
    case MUX_UI_REQUEST_PERMISSION:
        return action == MUX_UI_ACTION_ALLOW_ONCE ||
               action == MUX_UI_ACTION_DENY_ONCE ||
               action == MUX_UI_ACTION_ALLOW_ALWAYS ||
               action == MUX_UI_ACTION_DENY_ALWAYS;
    case MUX_UI_REQUEST_AUTHENTICATION:
    case MUX_UI_REQUEST_FILE_CHOOSER:
    case MUX_UI_REQUEST_DOWNLOAD_DESTINATION:
        return action == MUX_UI_ACTION_SUBMIT ||
               action == MUX_UI_ACTION_CANCEL;
    case MUX_UI_REQUEST_CONTEXT_MENU:
    case MUX_UI_REQUEST_OPTION_MENU:
        return action == MUX_UI_ACTION_SELECT ||
               action == MUX_UI_ACTION_CANCEL;
    case MUX_UI_REQUEST_CRASH:
        return action == MUX_UI_ACTION_RELOAD ||
               action == MUX_UI_ACTION_CLOSE;
    case MUX_UI_REQUEST_NOTIFICATION:
        return action == MUX_UI_ACTION_ACKNOWLEDGE ||
               action == MUX_UI_ACTION_CANCEL;
    default:
        return FALSE;
    }
}

static gboolean
validate_request(const MuxUiRequest *request, GError **error)
{
    guint i;

    if (!request)
        return fail(error, MUX_UI_ERROR_INVALID, "request is NULL");
    if (!request->request_id)
        return fail(error, MUX_UI_ERROR_INVALID, "request ID must be nonzero");
    if (!request_kind_valid(request->kind))
        return fail(error, MUX_UI_ERROR_INVALID, "invalid request kind");
    if (request->flags & ~request_flags_all)
        return fail(error, MUX_UI_ERROR_INVALID, "invalid request flags");
    if (!validate_string(request->origin,
                         MUX_UI_MAX_ORIGIN,
                         "origin",
                         error) ||
        !validate_string(request->heading,
                         MUX_UI_MAX_HEADING,
                         "heading",
                         error) ||
        !validate_string(request->message,
                         MUX_UI_MAX_MESSAGE,
                         "message",
                         error) ||
        !validate_string(request->default_value,
                         MUX_UI_MAX_VALUE,
                         "default value",
                         error))
        return FALSE;
    if (request->choices && request->choices->len > MUX_UI_MAX_CHOICES)
        return fail(error, MUX_UI_ERROR_TOO_LARGE, "too many UI choices");
    if (!request->choices)
        return TRUE;
    for (i = 0; i < request->choices->len; i++) {
        const MuxUiChoice *choice = g_ptr_array_index(request->choices, i);

        if (!choice)
            return fail(error, MUX_UI_ERROR_INVALID, "choice is NULL");
        if (choice->flags & ~choice_flags_all)
            return fail(error, MUX_UI_ERROR_INVALID, "invalid choice flags");
        if (!validate_string(choice->label,
                             MUX_UI_MAX_CHOICE_LABEL,
                             "choice label",
                             error))
            return FALSE;
    }
    return TRUE;
}

static gboolean
validate_response(const MuxUiResponse *response, GError **error)
{
    guint i;

    if (!response)
        return fail(error, MUX_UI_ERROR_INVALID, "response is NULL");
    if (!response->request_id)
        return fail(error, MUX_UI_ERROR_INVALID, "response ID must be nonzero");
    if (!action_valid(response->action))
        return fail(error, MUX_UI_ERROR_INVALID, "invalid response action");
    if (!validate_string(response->value,
                         MUX_UI_MAX_VALUE,
                         "response value",
                         error))
        return FALSE;
    if (response->paths && response->paths->len > MUX_UI_MAX_PATHS)
        return fail(error, MUX_UI_ERROR_TOO_LARGE, "too many response paths");
    if (!response->paths)
        return TRUE;
    for (i = 0; i < response->paths->len; i++) {
        if (!validate_string(g_ptr_array_index(response->paths, i),
                             MUX_UI_MAX_PATH,
                             "response path",
                             error))
            return FALSE;
    }
    return TRUE;
}

static GBytes *
finish_bytes(GByteArray *bytes, GError **error)
{
    if (bytes->len > MUX_UI_MAX_PAYLOAD) {
        g_byte_array_unref(bytes);
        fail(error, MUX_UI_ERROR_TOO_LARGE, "UI payload exceeds 256 KiB");
        return NULL;
    }
    return g_byte_array_free_to_bytes(bytes);
}

GBytes *
mux_ui_request_encode(const MuxUiRequest *request, GError **error)
{
    GByteArray *bytes;
    guint i;

    if (!validate_request(request, error))
        return NULL;
    bytes = g_byte_array_new();
    append_header(bytes, MUX_UI_RECORD_REQUEST);
    append_u64(bytes, request->request_id);
    append_u16(bytes, request->kind);
    append_u16(bytes, 0);
    append_u32(bytes, request->flags);
    append_u32(bytes, request->deadline_ms);
    append_u16(bytes, request->choices ? request->choices->len : 0);
    append_u16(bytes, 0);
    append_string(bytes, request->origin);
    append_string(bytes, request->heading);
    append_string(bytes, request->message);
    append_string(bytes, request->default_value);

    if (request->choices) {
        for (i = 0; i < request->choices->len; i++) {
            const MuxUiChoice *choice =
                g_ptr_array_index(request->choices, i);

            append_u32(bytes, choice->id);
            append_u32(bytes, choice->flags);
            append_string(bytes, choice->label);
        }
    }
    return finish_bytes(bytes, error);
}

gboolean
mux_ui_request_decode(const guint8 *data,
                      gsize length,
                      MuxUiRequest **request_out,
                      GError **error)
{
    MuxUiReader reader = {data, length, 0};
    MuxUiRequest *request = NULL;
    guint16 kind;
    guint16 reserved_a;
    guint16 reserved_b;
    guint16 choice_count;
    guint i;

    g_return_val_if_fail(request_out, FALSE);
    *request_out = NULL;
    if (!data && length)
        return fail(error, MUX_UI_ERROR_INVALID, "payload data is NULL");
    if (!reader_header(&reader, MUX_UI_RECORD_REQUEST, error) ||
        !reader_u64(&reader, &(guint64){0}, error))
        return FALSE;
    reader.offset -= sizeof(guint64);

    request = mux_ui_request_new(MUX_UI_REQUEST_DIALOG_ALERT);
    if (!reader_u64(&reader, &request->request_id, error) ||
        !reader_u16(&reader, &kind, error) ||
        !reader_u16(&reader, &reserved_a, error) ||
        !reader_u32(&reader, &request->flags, error) ||
        !reader_u32(&reader, &request->deadline_ms, error) ||
        !reader_u16(&reader, &choice_count, error) ||
        !reader_u16(&reader, &reserved_b, error))
        goto invalid;
    request->kind = kind;
    if (!request->request_id || !request_kind_valid(request->kind) ||
        reserved_a || reserved_b ||
        (request->flags & ~request_flags_all)) {
        fail(error,
             MUX_UI_ERROR_INVALID,
             "invalid request ID, kind, flags, or reserved field");
        goto invalid;
    }
    if (choice_count > MUX_UI_MAX_CHOICES) {
        fail(error, MUX_UI_ERROR_TOO_LARGE, "too many UI choices");
        goto invalid;
    }
    if (!reader_string(&reader,
                       MUX_UI_MAX_ORIGIN,
                       "origin",
                       &request->origin,
                       error) ||
        !reader_string(&reader,
                       MUX_UI_MAX_HEADING,
                       "heading",
                       &request->heading,
                       error) ||
        !reader_string(&reader,
                       MUX_UI_MAX_MESSAGE,
                       "message",
                       &request->message,
                       error) ||
        !reader_string(&reader,
                       MUX_UI_MAX_VALUE,
                       "default value",
                       &request->default_value,
                       error))
        goto invalid;

    for (i = 0; i < choice_count; i++) {
        MuxUiChoice *choice = mux_ui_choice_new(0, 0, NULL);

        g_clear_pointer(&choice->label, g_free);
        if (!reader_u32(&reader, &choice->id, error) ||
            !reader_u32(&reader, &choice->flags, error) ||
            !reader_string(&reader,
                           MUX_UI_MAX_CHOICE_LABEL,
                           "choice label",
                           &choice->label,
                           error)) {
            mux_ui_choice_free(choice);
            goto invalid;
        }
        if (choice->flags & ~choice_flags_all) {
            fail(error, MUX_UI_ERROR_INVALID, "invalid choice flags");
            mux_ui_choice_free(choice);
            goto invalid;
        }
        g_ptr_array_add(request->choices, choice);
    }
    if (!reader_finished(&reader, error))
        goto invalid;
    *request_out = request;
    return TRUE;

invalid:
    mux_ui_request_free(request);
    return FALSE;
}

GBytes *
mux_ui_response_encode(const MuxUiResponse *response, GError **error)
{
    GByteArray *bytes;
    guint i;

    if (!validate_response(response, error))
        return NULL;
    bytes = g_byte_array_new();
    append_header(bytes, MUX_UI_RECORD_RESPONSE);
    append_u64(bytes, response->request_id);
    append_u16(bytes, response->action);
    append_u16(bytes, 0);
    append_u16(bytes, response->paths ? response->paths->len : 0);
    append_u16(bytes, 0);
    append_string(bytes, response->value);
    if (response->paths) {
        for (i = 0; i < response->paths->len; i++)
            append_string(bytes, g_ptr_array_index(response->paths, i));
    }
    return finish_bytes(bytes, error);
}

gboolean
mux_ui_response_decode(const guint8 *data,
                       gsize length,
                       MuxUiResponse **response_out,
                       GError **error)
{
    MuxUiReader reader = {data, length, 0};
    MuxUiResponse *response = NULL;
    guint64 request_id;
    guint16 action;
    guint16 reserved_a;
    guint16 reserved_b;
    guint16 path_count;
    guint i;

    g_return_val_if_fail(response_out, FALSE);
    *response_out = NULL;
    if (!data && length)
        return fail(error, MUX_UI_ERROR_INVALID, "payload data is NULL");
    if (!reader_header(&reader, MUX_UI_RECORD_RESPONSE, error) ||
        !reader_u64(&reader, &request_id, error) ||
        !reader_u16(&reader, &action, error) ||
        !reader_u16(&reader, &reserved_a, error) ||
        !reader_u16(&reader, &path_count, error) ||
        !reader_u16(&reader, &reserved_b, error))
        return FALSE;
    if (!request_id || !action_valid(action) || reserved_a || reserved_b)
        return fail(error,
                    MUX_UI_ERROR_INVALID,
                    "invalid response ID, action, or reserved field");
    if (path_count > MUX_UI_MAX_PATHS)
        return fail(error, MUX_UI_ERROR_TOO_LARGE, "too many response paths");

    response = mux_ui_response_new(request_id, action);
    if (!reader_string(&reader,
                       MUX_UI_MAX_VALUE,
                       "response value",
                       &response->value,
                       error))
        goto invalid;
    for (i = 0; i < path_count; i++) {
        gchar *path = NULL;

        if (!reader_string(&reader,
                           MUX_UI_MAX_PATH,
                           "response path",
                           &path,
                           error))
            goto invalid;
        g_ptr_array_add(response->paths, path);
    }
    if (!reader_finished(&reader, error))
        goto invalid;
    *response_out = response;
    return TRUE;

invalid:
    mux_ui_response_free(response);
    return FALSE;
}

GBytes *
mux_ui_cancel_encode(guint64 request_id,
                     MuxUiCancelReason reason,
                     GError **error)
{
    GByteArray *bytes;

    if (!request_id)
        return fail(error,
                    MUX_UI_ERROR_INVALID,
                    "cancel request ID must be nonzero"),
               NULL;
    if (!cancel_reason_valid(reason))
        return fail(error, MUX_UI_ERROR_INVALID, "invalid cancel reason"), NULL;

    bytes = g_byte_array_new();
    append_header(bytes, MUX_UI_RECORD_CANCEL);
    append_u64(bytes, request_id);
    append_u16(bytes, reason);
    append_u16(bytes, 0);
    return finish_bytes(bytes, error);
}

gboolean
mux_ui_cancel_decode(const guint8 *data,
                     gsize length,
                     guint64 *request_id,
                     MuxUiCancelReason *reason,
                     GError **error)
{
    MuxUiReader reader = {data, length, 0};
    guint16 raw_reason;
    guint16 reserved;

    g_return_val_if_fail(request_id, FALSE);
    g_return_val_if_fail(reason, FALSE);
    if (!data && length)
        return fail(error, MUX_UI_ERROR_INVALID, "payload data is NULL");
    if (!reader_header(&reader, MUX_UI_RECORD_CANCEL, error) ||
        !reader_u64(&reader, request_id, error) ||
        !reader_u16(&reader, &raw_reason, error) ||
        !reader_u16(&reader, &reserved, error))
        return FALSE;
    if (!*request_id || !cancel_reason_valid(raw_reason) || reserved)
        return fail(error, MUX_UI_ERROR_INVALID, "invalid cancel record");
    *reason = raw_reason;
    return reader_finished(&reader, error);
}

gboolean
mux_ui_record_type(const guint8 *data,
                   gsize length,
                   MuxUiRecordType *type,
                   GError **error)
{
    MuxUiReader reader = {data, length, 0};
    guint32 magic;
    guint16 version;
    guint16 raw_type;

    g_return_val_if_fail(type, FALSE);
    if (!data && length)
        return fail(error, MUX_UI_ERROR_INVALID, "payload data is NULL");
    if (length > MUX_UI_MAX_PAYLOAD)
        return fail(error, MUX_UI_ERROR_TOO_LARGE, "UI payload exceeds 256 KiB");
    if (!reader_u32(&reader, &magic, error) ||
        !reader_u16(&reader, &version, error) ||
        !reader_u16(&reader, &raw_type, error))
        return FALSE;
    if (magic != MUX_UI_MAGIC)
        return fail(error, MUX_UI_ERROR_INVALID, "invalid UI payload magic");
    if (version != MUX_UI_WIRE_VERSION)
        return fail(error, MUX_UI_ERROR_UNSUPPORTED, "unsupported UI version");
    if (raw_type < MUX_UI_RECORD_REQUEST || raw_type > MUX_UI_RECORD_CANCEL)
        return fail(error, MUX_UI_ERROR_INVALID, "invalid UI record type");
    *type = raw_type;
    return TRUE;
}
