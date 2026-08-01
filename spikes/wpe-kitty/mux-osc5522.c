#include "mux-osc5522.h"

#include <string.h>

#define OSC_PREFIX "\033]5522;"
#define OSC_SUFFIX "\033\\"
#define SUPPORT_PREFIX "\033[?5522;"
#define SUPPORT_SUFFIX "$y"

G_DEFINE_QUARK(mux-osc5522-error-quark, mux_osc5522_error)

static gboolean
set_invalid(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_OSC5522_ERROR,
                        MUX_OSC5522_CODEC_ERROR_INVALID,
                        message);
    return FALSE;
}

static gboolean
set_limit(GError **error, const gchar *message)
{
    g_set_error_literal(error,
                        MUX_OSC5522_ERROR,
                        MUX_OSC5522_CODEC_ERROR_LIMIT,
                        message);
    return FALSE;
}

static gboolean
validate_location(MuxOsc5522Location location, GError **error)
{
    if (location == MUX_OSC5522_LOCATION_CLIPBOARD ||
        location == MUX_OSC5522_LOCATION_PRIMARY)
        return TRUE;

    return set_invalid(error, "invalid clipboard location");
}

static gboolean
validate_id(const gchar *id, gboolean required, GError **error)
{
    gsize i;
    gsize length;

    if (id == NULL)
        return required ? set_invalid(error, "clipboard request id is required") : TRUE;

    length = strlen(id);
    if (length == 0 || length > MUX_OSC5522_MAX_ID)
        return set_limit(error, "clipboard request id has an invalid length");

    for (i = 0; i < length; i++) {
        if (!g_ascii_isalnum((guchar)id[i]) && id[i] != '-' && id[i] != '_' &&
            id[i] != '+' && id[i] != '.')
            return set_invalid(error, "clipboard request id contains an unsafe byte");
    }

    return TRUE;
}

static gboolean
validate_text(const gchar *text, const gchar *label, GError **error)
{
    gsize length;

    if (text == NULL)
        return TRUE;

    length = strlen(text);
    if (length > MUX_OSC5522_MAX_TEXT) {
        g_set_error(error,
                    MUX_OSC5522_ERROR,
                    MUX_OSC5522_CODEC_ERROR_LIMIT,
                    "%s is too long",
                    label);
        return FALSE;
    }

    if (!g_utf8_validate(text, length, NULL)) {
        g_set_error(error,
                    MUX_OSC5522_ERROR,
                    MUX_OSC5522_CODEC_ERROR_INVALID,
                    "%s is not valid UTF-8",
                    label);
        return FALSE;
    }

    return TRUE;
}

static gboolean
validate_mime(const gchar *mime, GError **error)
{
    gsize i;
    gsize length;

    if (mime == NULL || mime[0] == '\0')
        return set_invalid(error, "MIME type is required");

    length = strlen(mime);
    if (length > MUX_OSC5522_MAX_MIME)
        return set_limit(error, "MIME type is too long");

    for (i = 0; i < length; i++) {
        if ((guchar)mime[i] < 0x21 || (guchar)mime[i] > 0x7e)
            return set_invalid(error, "MIME type contains an unsafe byte");
    }

    return TRUE;
}

static void
append_location(GString *metadata, MuxOsc5522Location location)
{
    if (location == MUX_OSC5522_LOCATION_PRIMARY)
        g_string_append(metadata, ":loc=primary");
}

static void
append_base64_field(GString *metadata, const gchar *key, const gchar *value)
{
    gchar *encoded;

    if (value == NULL)
        return;

    encoded = g_base64_encode((const guchar *)value, strlen(value));
    g_string_append_printf(metadata, ":%s=%s", key, encoded);
    g_free(encoded);
}

static gboolean
append_common(GString *metadata,
              const gchar *id,
              MuxOsc5522Location location,
              const gchar *password,
              const gchar *human_name,
              GError **error)
{
    if (!validate_id(id, TRUE, error) ||
        !validate_location(location, error) ||
        !validate_text(password, "clipboard password", error) ||
        !validate_text(human_name, "clipboard request name", error))
        return FALSE;

    append_location(metadata, location);
    g_string_append_printf(metadata, ":id=%s", id);
    append_base64_field(metadata, "pw", password);
    append_base64_field(metadata, "name", human_name);
    return TRUE;
}

static GBytes *
build_osc(const gchar *metadata, const gchar *payload, GError **error)
{
    GByteArray *wire;
    gsize metadata_length = strlen(metadata);
    gsize payload_length = strlen(payload);
    gsize total = sizeof(OSC_PREFIX) - 1 + metadata_length + 1 +
                  payload_length + sizeof(OSC_SUFFIX) - 1;

    if (total > MUX_OSC5522_MAX_SEQUENCE) {
        set_limit(error, "OSC 5522 sequence is too long");
        return NULL;
    }

    wire = g_byte_array_sized_new(total);
    g_byte_array_append(wire, (const guint8 *)OSC_PREFIX, sizeof(OSC_PREFIX) - 1);
    g_byte_array_append(wire, (const guint8 *)metadata, metadata_length);
    g_byte_array_append(wire, (const guint8 *)";", 1);
    g_byte_array_append(wire, (const guint8 *)payload, payload_length);
    g_byte_array_append(wire, (const guint8 *)OSC_SUFFIX, sizeof(OSC_SUFFIX) - 1);
    return g_byte_array_free_to_bytes(wire);
}

static GBytes *
literal_bytes(const gchar *text)
{
    return g_bytes_new(text, strlen(text));
}

GBytes *
mux_osc5522_set_paste_events(gboolean enabled)
{
    return literal_bytes(enabled ? "\033[?5522h" : "\033[?5522l");
}

GBytes *
mux_osc5522_query_support(void)
{
    return literal_bytes("\033[?5522$p");
}

gboolean
mux_osc5522_parse_support(const guint8 *sequence,
                          gsize length,
                          MuxOsc5522Support *support,
                          GError **error)
{
    const gsize prefix_length = sizeof(SUPPORT_PREFIX) - 1;
    const gsize suffix_length = sizeof(SUPPORT_SUFFIX) - 1;
    gchar *number;
    gchar *end = NULL;
    guint64 value;

    g_return_val_if_fail(support != NULL, FALSE);
    *support = MUX_OSC5522_SUPPORT_UNKNOWN;

    if (sequence == NULL || length <= prefix_length + suffix_length ||
        memcmp(sequence, SUPPORT_PREFIX, prefix_length) != 0 ||
        memcmp(sequence + length - suffix_length, SUPPORT_SUFFIX, suffix_length) != 0)
        return set_invalid(error, "invalid OSC 5522 support response");

    number = g_strndup((const gchar *)sequence + prefix_length,
                       length - prefix_length - suffix_length);
    value = g_ascii_strtoull(number, &end, 10);
    if (number[0] == '\0' || end == NULL || *end != '\0' || value > 4) {
        g_free(number);
        return set_invalid(error, "invalid OSC 5522 support state");
    }
    g_free(number);

    switch (value) {
    case 1:
        *support = MUX_OSC5522_SUPPORT_ENABLED;
        break;
    case 2:
        *support = MUX_OSC5522_SUPPORT_DISABLED;
        break;
    case 3:
        *support = MUX_OSC5522_SUPPORT_PERMANENTLY_ENABLED;
        break;
    case 4:
        *support = MUX_OSC5522_SUPPORT_PERMANENTLY_DISABLED;
        break;
    default:
        *support = MUX_OSC5522_SUPPORT_UNKNOWN;
        break;
    }

    return TRUE;
}

GBytes *
mux_osc5522_read_request(const gchar *id,
                         MuxOsc5522Location location,
                         const gchar *const *mime_types,
                         const gchar *password,
                         const gchar *human_name,
                         GError **error)
{
    GString *metadata = g_string_new("type=read");
    GString *mimes = g_string_new(NULL);
    GBytes *result = NULL;
    gchar *payload = NULL;
    guint i;

    if (!append_common(metadata, id, location, password, human_name, error))
        goto out;

    if (mime_types != NULL) {
        for (i = 0; mime_types[i] != NULL; i++) {
            if (i >= MUX_OSC5522_MAX_MIME_TYPES) {
                set_limit(error, "too many requested MIME types");
                goto out;
            }
            if (!validate_mime(mime_types[i], error))
                goto out;
            if (i > 0)
                g_string_append_c(mimes, ' ');
            g_string_append(mimes, mime_types[i]);
        }
    }

    if (mimes->len == 0)
        payload = g_strdup(".");
    else
        payload = g_base64_encode((const guchar *)mimes->str, mimes->len);

    result = build_osc(metadata->str, payload, error);

out:
    g_free(payload);
    g_string_free(mimes, TRUE);
    g_string_free(metadata, TRUE);
    return result;
}

GBytes *
mux_osc5522_read_mime_request(const gchar *id,
                              MuxOsc5522Location location,
                              const gchar *mime,
                              const gchar *password,
                              const gchar *human_name,
                              GError **error)
{
    GString *metadata = g_string_new("type=read");
    GBytes *result = NULL;

    if (!validate_mime(mime, error) ||
        !append_common(metadata, id, location, password, human_name, error))
        goto out;

    append_base64_field(metadata, "mime", mime);
    result = build_osc(metadata->str, "", error);

out:
    g_string_free(metadata, TRUE);
    return result;
}

GBytes *
mux_osc5522_write_begin(const gchar *id,
                        MuxOsc5522Location location,
                        const gchar *password,
                        const gchar *human_name,
                        GError **error)
{
    GString *metadata = g_string_new("type=write");
    GBytes *result = NULL;

    if (append_common(metadata, id, location, password, human_name, error))
        result = build_osc(metadata->str, "", error);

    g_string_free(metadata, TRUE);
    return result;
}

GBytes *
mux_osc5522_write_data(const gchar *mime,
                       const guint8 *data,
                       gsize length,
                       GError **error)
{
    GString *metadata = g_string_new("type=wdata");
    GBytes *result = NULL;
    gchar *payload;

    if (!validate_mime(mime, error))
        goto out;
    if (length > MUX_OSC5522_MAX_CHUNK) {
        set_limit(error, "clipboard data chunk exceeds 4096 bytes");
        goto out;
    }
    if (length > 0 && data == NULL) {
        set_invalid(error, "clipboard data is missing");
        goto out;
    }

    append_base64_field(metadata, "mime", mime);
    payload = g_base64_encode(data, length);
    result = build_osc(metadata->str, payload, error);
    g_free(payload);

out:
    g_string_free(metadata, TRUE);
    return result;
}

GBytes *
mux_osc5522_write_end(GError **error)
{
    return build_osc("type=wdata", "", error);
}

static gboolean
valid_base64(const gchar *encoded)
{
    gsize i;
    gsize length = strlen(encoded);
    gboolean saw_padding = FALSE;
    guint padding = 0;

    if (length == 0)
        return TRUE;
    if (length % 4 != 0)
        return FALSE;

    for (i = 0; i < length; i++) {
        if (encoded[i] == '=') {
            saw_padding = TRUE;
            padding++;
            if (padding > 2 || i < length - 2)
                return FALSE;
        } else if (saw_padding ||
                   (!g_ascii_isalnum((guchar)encoded[i]) &&
                    encoded[i] != '+' && encoded[i] != '/')) {
            return FALSE;
        }
    }

    return TRUE;
}

static GBytes *
decode_bytes(const gchar *encoded, gsize limit, GError **error)
{
    guchar *decoded;
    gsize decoded_length = 0;
    gchar *canonical;

    if (!valid_base64(encoded)) {
        set_invalid(error, "invalid base64 in OSC 5522 sequence");
        return NULL;
    }

    decoded = g_base64_decode(encoded, &decoded_length);
    if (decoded_length > limit) {
        g_free(decoded);
        set_limit(error, "decoded OSC 5522 field exceeds its limit");
        return NULL;
    }

    if (decoded_length == 0) {
        g_free(decoded);
        return g_bytes_new_static("", 0);
    }

    canonical = g_base64_encode(decoded, decoded_length);
    if (strcmp(canonical, encoded) != 0) {
        g_free(canonical);
        g_free(decoded);
        set_invalid(error, "non-canonical base64 in OSC 5522 sequence");
        return NULL;
    }
    g_free(canonical);

    return g_bytes_new_take(decoded, decoded_length);
}

static gchar *
decode_text(const gchar *encoded, gsize limit, const gchar *label, GError **error)
{
    GBytes *bytes;
    const guint8 *data;
    gsize length;
    gchar *text;

    bytes = decode_bytes(encoded, limit, error);
    if (bytes == NULL)
        return NULL;

    data = g_bytes_get_data(bytes, &length);
    if (length > 0 &&
        (memchr(data, '\0', length) != NULL ||
         !g_utf8_validate((const gchar *)data, length, NULL))) {
        g_set_error(error,
                    MUX_OSC5522_ERROR,
                    MUX_OSC5522_CODEC_ERROR_INVALID,
                    "%s is not valid UTF-8 text",
                    label);
        g_bytes_unref(bytes);
        return NULL;
    }

    text = length > 0 ? g_strndup((const gchar *)data, length) : g_strdup("");
    g_bytes_unref(bytes);
    return text;
}

static gboolean
take_field(const gchar **slot, const gchar *value, GError **error)
{
    if (*slot != NULL)
        return set_invalid(error, "duplicate metadata field in OSC 5522 sequence");
    *slot = value;
    return TRUE;
}

static gboolean
parse_remote_error(const gchar *status, MuxOsc5522RemoteError *remote_error)
{
    if (g_str_equal(status, "EIO"))
        *remote_error = MUX_OSC5522_REMOTE_ERROR_IO;
    else if (g_str_equal(status, "EINVAL"))
        *remote_error = MUX_OSC5522_REMOTE_ERROR_INVALID;
    else if (g_str_equal(status, "ENOSYS"))
        *remote_error = MUX_OSC5522_REMOTE_ERROR_UNSUPPORTED;
    else if (g_str_equal(status, "EPERM"))
        *remote_error = MUX_OSC5522_REMOTE_ERROR_PERMISSION;
    else if (g_str_equal(status, "EBUSY"))
        *remote_error = MUX_OSC5522_REMOTE_ERROR_BUSY;
    else
        return FALSE;

    return TRUE;
}

gboolean
mux_osc5522_parse(const guint8 *sequence,
                  gsize length,
                  MuxOsc5522Event **event_out,
                  GError **error)
{
    const gsize prefix_length = sizeof(OSC_PREFIX) - 1;
    const guint8 *body;
    const guint8 *separator;
    gsize body_length;
    gsize metadata_length;
    gsize payload_length;
    gchar *metadata = NULL;
    gchar *payload = NULL;
    gchar **fields = NULL;
    const gchar *type = NULL;
    const gchar *status = NULL;
    const gchar *id = NULL;
    const gchar *location = NULL;
    const gchar *encoded_mime = NULL;
    const gchar *encoded_password = NULL;
    const gchar *encoded_name = NULL;
    gchar *mime = NULL;
    gchar *password = NULL;
    gchar *human_name = NULL;
    MuxOsc5522Event *event = NULL;
    guint i;
    gboolean ok = FALSE;

    g_return_val_if_fail(event_out != NULL, FALSE);
    *event_out = NULL;

    if (sequence == NULL || length > MUX_OSC5522_MAX_SEQUENCE ||
        length < prefix_length + 2 ||
        memcmp(sequence, OSC_PREFIX, prefix_length) != 0)
        return set_invalid(error, "invalid OSC 5522 sequence");

    if (sequence[length - 1] == '\a') {
        body = sequence + prefix_length;
        body_length = length - prefix_length - 1;
    } else if (length >= prefix_length + 3 &&
               sequence[length - 2] == '\033' && sequence[length - 1] == '\\') {
        body = sequence + prefix_length;
        body_length = length - prefix_length - 2;
    } else {
        return set_invalid(error, "OSC 5522 sequence has no terminator");
    }

    if (memchr(body, '\0', body_length) != NULL)
        return set_invalid(error, "OSC 5522 sequence contains a NUL byte");

    separator = memchr(body, ';', body_length);
    if (separator == NULL)
        return set_invalid(error, "OSC 5522 sequence has no payload separator");

    metadata_length = separator - body;
    payload_length = body_length - metadata_length - 1;
    if (metadata_length == 0)
        return set_invalid(error, "OSC 5522 metadata is empty");

    metadata = g_strndup((const gchar *)body, metadata_length);
    payload = g_strndup((const gchar *)separator + 1, payload_length);
    fields = g_strsplit(metadata, ":", -1);

    for (i = 0; fields[i] != NULL; i++) {
        gchar *equals = strchr(fields[i], '=');
        const gchar *value;

        if (equals == NULL || equals == fields[i]) {
            set_invalid(error, "invalid OSC 5522 metadata field");
            goto out;
        }
        *equals = '\0';
        value = equals + 1;

        if (g_str_equal(fields[i], "type")) {
            if (!take_field(&type, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "status")) {
            if (!take_field(&status, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "id")) {
            if (!take_field(&id, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "loc")) {
            if (!take_field(&location, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "mime")) {
            if (!take_field(&encoded_mime, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "pw")) {
            if (!take_field(&encoded_password, value, error))
                goto out;
        } else if (g_str_equal(fields[i], "name")) {
            if (!take_field(&encoded_name, value, error))
                goto out;
        }
    }

    if (type == NULL || status == NULL || type[0] == '\0' || status[0] == '\0') {
        set_invalid(error, "OSC 5522 response lacks type or status");
        goto out;
    }
    if (!validate_id(id, FALSE, error))
        goto out;

    event = g_new0(MuxOsc5522Event, 1);
    event->location = MUX_OSC5522_LOCATION_CLIPBOARD;
    if (location != NULL) {
        if (g_str_equal(location, "primary"))
            event->location = MUX_OSC5522_LOCATION_PRIMARY;
        else if (!g_str_equal(location, "clipboard")) {
            set_invalid(error, "invalid OSC 5522 clipboard location");
            goto out;
        }
    }

    if (encoded_mime != NULL) {
        mime = decode_text(encoded_mime, MUX_OSC5522_MAX_MIME, "MIME type", error);
        if (mime == NULL || !validate_mime(mime, error))
            goto out;
    }
    if (encoded_password != NULL) {
        password = decode_text(encoded_password,
                               MUX_OSC5522_MAX_TEXT,
                               "clipboard password",
                               error);
        if (password == NULL)
            goto out;
    }
    if (encoded_name != NULL) {
        human_name = decode_text(encoded_name,
                                 MUX_OSC5522_MAX_TEXT,
                                 "clipboard request name",
                                 error);
        if (human_name == NULL)
            goto out;
    }

    if (parse_remote_error(status, &event->remote_error)) {
        if (!g_str_equal(type, "read") && !g_str_equal(type, "write")) {
            set_invalid(error, "invalid OSC 5522 error response type");
            goto out;
        }
        if (payload_length != 0) {
            set_invalid(error, "OSC 5522 error response has unexpected payload");
            goto out;
        }
        event->type = MUX_OSC5522_EVENT_ERROR;
    } else if (g_str_equal(type, "read") && g_str_equal(status, "OK")) {
        if (payload_length != 0) {
            set_invalid(error, "OSC 5522 read acknowledgement has unexpected payload");
            goto out;
        }
        event->type = MUX_OSC5522_EVENT_READ_OK;
    } else if (g_str_equal(type, "read") && g_str_equal(status, "DATA")) {
        if (mime == NULL) {
            set_invalid(error, "OSC 5522 data response lacks a MIME type");
            goto out;
        }
        event->data = decode_bytes(payload, MUX_OSC5522_MAX_CHUNK, error);
        if (event->data == NULL)
            goto out;
        event->type = MUX_OSC5522_EVENT_READ_DATA;
    } else if (g_str_equal(type, "read") && g_str_equal(status, "DONE")) {
        if (payload_length != 0) {
            set_invalid(error, "OSC 5522 read completion has unexpected payload");
            goto out;
        }
        event->type = MUX_OSC5522_EVENT_READ_DONE;
    } else if (g_str_equal(type, "write") && g_str_equal(status, "DONE")) {
        if (payload_length != 0) {
            set_invalid(error, "OSC 5522 write completion has unexpected payload");
            goto out;
        }
        event->type = MUX_OSC5522_EVENT_WRITE_DONE;
    } else {
        set_invalid(error, "unsupported OSC 5522 response type or status");
        goto out;
    }

    event->id = g_strdup(id);
    event->mime = mime;
    event->password = password;
    event->human_name = human_name;
    mime = NULL;
    password = NULL;
    human_name = NULL;
    *event_out = event;
    event = NULL;
    ok = TRUE;

out:
    mux_osc5522_event_free(event);
    g_free(mime);
    g_free(password);
    g_free(human_name);
    g_strfreev(fields);
    g_free(metadata);
    g_free(payload);
    return ok;
}

void
mux_osc5522_event_free(MuxOsc5522Event *event)
{
    if (event == NULL)
        return;

    g_free(event->id);
    g_free(event->mime);
    g_free(event->password);
    g_free(event->human_name);
    g_clear_pointer(&event->data, g_bytes_unref);
    g_free(event);
}
