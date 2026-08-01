#include "mux-ui-protocol.h"

#include <string.h>

#define TEST_REQUEST_ID G_GUINT64_CONSTANT(0x0102030405060708)

enum {
    UI_MAGIC_OFFSET = 0,
    UI_VERSION_OFFSET = 4,
    UI_RECORD_TYPE_OFFSET = 6,
    UI_REQUEST_ID_OFFSET = 8,
    UI_REQUEST_KIND_OFFSET = 16,
    UI_REQUEST_RESERVED_A_OFFSET = 18,
    UI_REQUEST_FLAGS_OFFSET = 20,
    UI_REQUEST_CHOICE_COUNT_OFFSET = 28,
    UI_REQUEST_RESERVED_B_OFFSET = 30,
    UI_REQUEST_ORIGIN_LENGTH_OFFSET = 32,
    UI_REQUEST_ORIGIN_DATA_OFFSET = 36,
    UI_SPARSE_REQUEST_CHOICE_FLAGS_OFFSET = 52,
    UI_SPARSE_REQUEST_CHOICE_LABEL_LENGTH_OFFSET = 56,
    UI_RESPONSE_ACTION_OFFSET = 16,
    UI_RESPONSE_RESERVED_A_OFFSET = 18,
    UI_RESPONSE_PATH_COUNT_OFFSET = 20,
    UI_RESPONSE_RESERVED_B_OFFSET = 22,
    UI_RESPONSE_VALUE_LENGTH_OFFSET = 24,
    UI_SPARSE_RESPONSE_PATH_LENGTH_OFFSET = 28,
    UI_CANCEL_REASON_OFFSET = 16,
    UI_CANCEL_RESERVED_OFFSET = 18,
};

static GBytes *
bytes_with_u8(GBytes *source, gsize offset, guint8 value)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(source, &length);
    guint8 *copy;

    g_assert_cmpuint(length, >, offset);
    copy = g_malloc(length);
    memcpy(copy, data, length);
    copy[offset] = value;
    return g_bytes_new_take(copy, length);
}

static GBytes *
bytes_with_u16(GBytes *source, gsize offset, guint16 value)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(source, &length);
    guint8 *copy;
    guint16 encoded = GUINT16_TO_BE(value);

    g_assert_cmpuint(length, >=, offset + sizeof(encoded));
    copy = g_malloc(length);
    memcpy(copy, data, length);
    memcpy(copy + offset, &encoded, sizeof(encoded));
    return g_bytes_new_take(copy, length);
}

static GBytes *
bytes_with_u32(GBytes *source, gsize offset, guint32 value)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(source, &length);
    guint8 *copy;
    guint32 encoded = GUINT32_TO_BE(value);

    g_assert_cmpuint(length, >=, offset + sizeof(encoded));
    copy = g_malloc(length);
    memcpy(copy, data, length);
    memcpy(copy + offset, &encoded, sizeof(encoded));
    return g_bytes_new_take(copy, length);
}

static GBytes *
bytes_with_u64(GBytes *source, gsize offset, guint64 value)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(source, &length);
    guint8 *copy;
    guint64 encoded = GUINT64_TO_BE(value);

    g_assert_cmpuint(length, >=, offset + sizeof(encoded));
    copy = g_malloc(length);
    memcpy(copy, data, length);
    memcpy(copy + offset, &encoded, sizeof(encoded));
    return g_bytes_new_take(copy, length);
}

static GBytes *
bytes_with_trailing_byte(GBytes *source)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(source, &length);
    guint8 *copy = g_malloc(length + 1);

    memcpy(copy, data, length);
    copy[length] = 0xa5;
    return g_bytes_new_take(copy, length + 1);
}

static GBytes *
encode_representative_request(void)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_OPTION_MENU);
    g_autoptr(GError) error = NULL;
    GBytes *encoded;

    request->request_id = TEST_REQUEST_ID;
    request->flags = MUX_UI_REQUEST_FLAG_USER_GESTURE |
                     MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE |
                     MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE;
    request->deadline_ms = 12500;
    request->origin = g_strdup("https://example.test");
    request->heading = g_strdup("Choose an option");
    request->message = g_strdup("Representative UI request");
    request->default_value = g_strdup("second");
    g_ptr_array_add(request->choices,
                    mux_ui_choice_new(10,
                                      MUX_UI_CHOICE_FLAG_SELECTED,
                                      "First"));
    g_ptr_array_add(request->choices,
                    mux_ui_choice_new(20,
                                      MUX_UI_CHOICE_FLAG_DISABLED |
                                          MUX_UI_CHOICE_FLAG_DANGER,
                                      "Second"));

    encoded = mux_ui_request_encode(request, &error);
    g_assert_no_error(error);
    g_assert_nonnull(encoded);
    return encoded;
}

static GBytes *
encode_sparse_request(guint32 request_flags, guint32 choice_flags)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_CONTEXT_MENU);
    g_autoptr(GError) error = NULL;
    GBytes *encoded;

    request->request_id = 7;
    request->flags = request_flags;
    g_ptr_array_add(request->choices,
                    mux_ui_choice_new(9, choice_flags, "choice"));
    encoded = mux_ui_request_encode(request, &error);
    g_assert_no_error(error);
    g_assert_nonnull(encoded);
    return encoded;
}

static GBytes *
encode_representative_response(void)
{
    g_autoptr(MuxUiResponse) response =
        mux_ui_response_new(TEST_REQUEST_ID, MUX_UI_ACTION_SUBMIT);
    g_autoptr(GError) error = NULL;
    GBytes *encoded;

    response->value = g_strdup("chosen-value");
    g_ptr_array_add(response->paths, g_strdup("/tmp/report.pdf"));
    g_ptr_array_add(response->paths, g_strdup("/home/user/image.png"));
    encoded = mux_ui_response_encode(response, &error);
    g_assert_no_error(error);
    g_assert_nonnull(encoded);
    return encoded;
}

static GBytes *
encode_sparse_response(void)
{
    g_autoptr(MuxUiResponse) response =
        mux_ui_response_new(7, MUX_UI_ACTION_SUBMIT);
    g_autoptr(GError) error = NULL;
    GBytes *encoded;

    g_ptr_array_add(response->paths, g_strdup(""));
    encoded = mux_ui_response_encode(response, &error);
    g_assert_no_error(error);
    g_assert_nonnull(encoded);
    return encoded;
}

static GBytes *
encode_cancel(void)
{
    g_autoptr(GError) error = NULL;
    GBytes *encoded = mux_ui_cancel_encode(
        TEST_REQUEST_ID, MUX_UI_CANCEL_NAVIGATION, &error);

    g_assert_no_error(error);
    g_assert_nonnull(encoded);
    return encoded;
}

static void
assert_request_decode_error(const guint8 *data,
                            gsize length,
                            MuxUiError expected)
{
    g_autoptr(MuxUiRequest) decoded = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_ui_request_decode(data, length, &decoded, &error));
    g_assert_null(decoded);
    g_assert_error(error, MUX_UI_ERROR, (gint)expected);
}

static void
assert_request_bytes_error(GBytes *bytes, MuxUiError expected)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(bytes, &length);

    assert_request_decode_error(data, length, expected);
}

static void
assert_response_decode_error(const guint8 *data,
                             gsize length,
                             MuxUiError expected)
{
    g_autoptr(MuxUiResponse) decoded = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_false(mux_ui_response_decode(data, length, &decoded, &error));
    g_assert_null(decoded);
    g_assert_error(error, MUX_UI_ERROR, (gint)expected);
}

static void
assert_response_bytes_error(GBytes *bytes, MuxUiError expected)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(bytes, &length);

    assert_response_decode_error(data, length, expected);
}

static void
assert_cancel_decode_error(const guint8 *data,
                           gsize length,
                           MuxUiError expected)
{
    g_autoptr(GError) error = NULL;
    guint64 request_id = 0;
    MuxUiCancelReason reason = 0;

    g_assert_false(mux_ui_cancel_decode(data,
                                        length,
                                        &request_id,
                                        &reason,
                                        &error));
    g_assert_error(error, MUX_UI_ERROR, (gint)expected);
}

static void
assert_cancel_bytes_error(GBytes *bytes, MuxUiError expected)
{
    gsize length;
    const guint8 *data = g_bytes_get_data(bytes, &length);

    assert_cancel_decode_error(data, length, expected);
}

static void
assert_record_type_error(GBytes *bytes, MuxUiError expected)
{
    g_autoptr(GError) error = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(bytes, &length);
    MuxUiRecordType type = 0;

    g_assert_false(mux_ui_record_type(data, length, &type, &error));
    g_assert_error(error, MUX_UI_ERROR, (gint)expected);
}

static void
test_request_round_trip(void)
{
    g_autoptr(GBytes) encoded = encode_representative_request();
    g_autoptr(MuxUiRequest) decoded = NULL;
    g_autoptr(GError) error = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(encoded, &length);
    const MuxUiChoice *choice;
    MuxUiRecordType type = 0;

    g_assert_true(mux_ui_record_type(data, length, &type, &error));
    g_assert_no_error(error);
    g_assert_cmpint(type, ==, MUX_UI_RECORD_REQUEST);
    g_assert_true(mux_ui_request_decode(data, length, &decoded, &error));
    g_assert_no_error(error);
    g_assert_nonnull(decoded);
    g_assert_cmpuint(decoded->request_id, ==, TEST_REQUEST_ID);
    g_assert_cmpint(decoded->kind, ==, MUX_UI_REQUEST_OPTION_MENU);
    g_assert_cmpuint(decoded->flags,
                     ==,
                     MUX_UI_REQUEST_FLAG_USER_GESTURE |
                         MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE |
                         MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE);
    g_assert_cmpuint(decoded->deadline_ms, ==, 12500);
    g_assert_cmpstr(decoded->origin, ==, "https://example.test");
    g_assert_cmpstr(decoded->heading, ==, "Choose an option");
    g_assert_cmpstr(decoded->message, ==, "Representative UI request");
    g_assert_cmpstr(decoded->default_value, ==, "second");
    g_assert_cmpuint(decoded->choices->len, ==, 2);

    choice = g_ptr_array_index(decoded->choices, 0);
    g_assert_cmpuint(choice->id, ==, 10);
    g_assert_cmpuint(choice->flags, ==, MUX_UI_CHOICE_FLAG_SELECTED);
    g_assert_cmpstr(choice->label, ==, "First");
    choice = g_ptr_array_index(decoded->choices, 1);
    g_assert_cmpuint(choice->id, ==, 20);
    g_assert_cmpuint(choice->flags,
                     ==,
                     MUX_UI_CHOICE_FLAG_DISABLED |
                         MUX_UI_CHOICE_FLAG_DANGER);
    g_assert_cmpstr(choice->label, ==, "Second");
}

static void
test_response_round_trip(void)
{
    g_autoptr(GBytes) encoded = encode_representative_response();
    g_autoptr(MuxUiResponse) decoded = NULL;
    g_autoptr(GError) error = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(encoded, &length);
    MuxUiRecordType type = 0;

    g_assert_true(mux_ui_record_type(data, length, &type, &error));
    g_assert_no_error(error);
    g_assert_cmpint(type, ==, MUX_UI_RECORD_RESPONSE);
    g_assert_true(mux_ui_response_decode(data, length, &decoded, &error));
    g_assert_no_error(error);
    g_assert_nonnull(decoded);
    g_assert_cmpuint(decoded->request_id, ==, TEST_REQUEST_ID);
    g_assert_cmpint(decoded->action, ==, MUX_UI_ACTION_SUBMIT);
    g_assert_cmpstr(decoded->value, ==, "chosen-value");
    g_assert_cmpuint(decoded->paths->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(decoded->paths, 0),
                    ==,
                    "/tmp/report.pdf");
    g_assert_cmpstr(g_ptr_array_index(decoded->paths, 1),
                    ==,
                    "/home/user/image.png");
}

static void
test_cancel_round_trip(void)
{
    g_autoptr(GBytes) encoded = encode_cancel();
    g_autoptr(GError) error = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(encoded, &length);
    guint64 request_id = 0;
    MuxUiCancelReason reason = 0;
    MuxUiRecordType type = 0;

    g_assert_true(mux_ui_record_type(data, length, &type, &error));
    g_assert_no_error(error);
    g_assert_cmpint(type, ==, MUX_UI_RECORD_CANCEL);
    g_assert_true(mux_ui_cancel_decode(data,
                                       length,
                                       &request_id,
                                       &reason,
                                       &error));
    g_assert_no_error(error);
    g_assert_cmpuint(request_id, ==, TEST_REQUEST_ID);
    g_assert_cmpint(reason, ==, MUX_UI_CANCEL_NAVIGATION);
}

static void
test_reject_header_errors(void)
{
    g_autoptr(GBytes) request = encode_representative_request();
    g_autoptr(GBytes) bad_magic =
        bytes_with_u32(request, UI_MAGIC_OFFSET, 0);
    g_autoptr(GBytes) bad_version =
        bytes_with_u16(request,
                       UI_VERSION_OFFSET,
                       MUX_UI_WIRE_VERSION + 1);
    g_autoptr(GBytes) bad_record_low =
        bytes_with_u16(request, UI_RECORD_TYPE_OFFSET, 0);
    g_autoptr(GBytes) bad_record_high =
        bytes_with_u16(request,
                       UI_RECORD_TYPE_OFFSET,
                       MUX_UI_RECORD_CANCEL + 1);
    g_autoptr(GBytes) wrong_record =
        bytes_with_u16(request,
                       UI_RECORD_TYPE_OFFSET,
                       MUX_UI_RECORD_RESPONSE);

    assert_request_bytes_error(bad_magic, MUX_UI_ERROR_INVALID);
    assert_record_type_error(bad_magic, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(bad_version, MUX_UI_ERROR_UNSUPPORTED);
    assert_record_type_error(bad_version, MUX_UI_ERROR_UNSUPPORTED);
    assert_record_type_error(bad_record_low, MUX_UI_ERROR_INVALID);
    assert_record_type_error(bad_record_high, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(wrong_record, MUX_UI_ERROR_INVALID);
}

static void
test_reject_ids_and_enums(void)
{
    g_autoptr(GBytes) request = encode_representative_request();
    g_autoptr(GBytes) response = encode_representative_response();
    g_autoptr(GBytes) cancel = encode_cancel();
    g_autoptr(GBytes) request_id_zero =
        bytes_with_u64(request, UI_REQUEST_ID_OFFSET, 0);
    g_autoptr(GBytes) request_kind_low =
        bytes_with_u16(request, UI_REQUEST_KIND_OFFSET, 0);
    g_autoptr(GBytes) request_kind_high =
        bytes_with_u16(request,
                       UI_REQUEST_KIND_OFFSET,
                       MUX_UI_REQUEST_NOTIFICATION + 1);
    g_autoptr(GBytes) response_id_zero =
        bytes_with_u64(response, UI_REQUEST_ID_OFFSET, 0);
    g_autoptr(GBytes) response_action_low =
        bytes_with_u16(response, UI_RESPONSE_ACTION_OFFSET, 0);
    g_autoptr(GBytes) response_action_high =
        bytes_with_u16(response,
                       UI_RESPONSE_ACTION_OFFSET,
                       MUX_UI_ACTION_UNSUPPORTED + 1);
    g_autoptr(GBytes) cancel_id_zero =
        bytes_with_u64(cancel, UI_REQUEST_ID_OFFSET, 0);
    g_autoptr(GBytes) cancel_reason_low =
        bytes_with_u16(cancel, UI_CANCEL_REASON_OFFSET, 0);
    g_autoptr(GBytes) cancel_reason_high =
        bytes_with_u16(cancel,
                       UI_CANCEL_REASON_OFFSET,
                       MUX_UI_CANCEL_SUPERSEDED + 1);

    assert_request_bytes_error(request_id_zero, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(request_kind_low, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(request_kind_high, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_id_zero, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_action_low, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_action_high, MUX_UI_ERROR_INVALID);
    assert_cancel_bytes_error(cancel_id_zero, MUX_UI_ERROR_INVALID);
    assert_cancel_bytes_error(cancel_reason_low, MUX_UI_ERROR_INVALID);
    assert_cancel_bytes_error(cancel_reason_high, MUX_UI_ERROR_INVALID);
}

static void
test_reject_unknown_flags(void)
{
    const guint32 request_flags =
        MUX_UI_REQUEST_FLAG_MULTIPLE |
        MUX_UI_REQUEST_FLAG_USER_GESTURE |
        MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE |
        MUX_UI_REQUEST_FLAG_PASSWORD |
        MUX_UI_REQUEST_FLAG_DANGER |
        MUX_UI_REQUEST_FLAG_PERSISTENCE_AVAILABLE;
    const guint32 choice_flags =
        MUX_UI_CHOICE_FLAG_DISABLED |
        MUX_UI_CHOICE_FLAG_SEPARATOR |
        MUX_UI_CHOICE_FLAG_SELECTED |
        MUX_UI_CHOICE_FLAG_DANGER;
    const guint32 unknown_flag = 1U << 31;
    g_autoptr(GBytes) valid =
        encode_sparse_request(request_flags, choice_flags);
    g_autoptr(GBytes) bad_request_flags =
        bytes_with_u32(valid,
                       UI_REQUEST_FLAGS_OFFSET,
                       request_flags | unknown_flag);
    g_autoptr(GBytes) bad_choice_flags =
        bytes_with_u32(valid,
                       UI_SPARSE_REQUEST_CHOICE_FLAGS_OFFSET,
                       choice_flags | unknown_flag);

    assert_request_bytes_error(bad_request_flags, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(bad_choice_flags, MUX_UI_ERROR_INVALID);

    {
        g_autoptr(MuxUiRequest) request =
            mux_ui_request_new(MUX_UI_REQUEST_PERMISSION);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;

        request->request_id = 1;
        request->flags = request_flags | unknown_flag;
        encoded = mux_ui_request_encode(request, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_INVALID);
    }

    {
        g_autoptr(MuxUiRequest) request =
            mux_ui_request_new(MUX_UI_REQUEST_CONTEXT_MENU);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;

        request->request_id = 1;
        g_ptr_array_add(request->choices,
                        mux_ui_choice_new(1,
                                          choice_flags | unknown_flag,
                                          "choice"));
        encoded = mux_ui_request_encode(request, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_INVALID);
    }
}

static void
test_reject_nonzero_reserved_fields(void)
{
    g_autoptr(GBytes) request = encode_representative_request();
    g_autoptr(GBytes) response = encode_representative_response();
    g_autoptr(GBytes) cancel = encode_cancel();
    g_autoptr(GBytes) request_reserved_a =
        bytes_with_u16(request, UI_REQUEST_RESERVED_A_OFFSET, 1);
    g_autoptr(GBytes) request_reserved_b =
        bytes_with_u16(request, UI_REQUEST_RESERVED_B_OFFSET, 1);
    g_autoptr(GBytes) response_reserved_a =
        bytes_with_u16(response, UI_RESPONSE_RESERVED_A_OFFSET, 1);
    g_autoptr(GBytes) response_reserved_b =
        bytes_with_u16(response, UI_RESPONSE_RESERVED_B_OFFSET, 1);
    g_autoptr(GBytes) cancel_reserved =
        bytes_with_u16(cancel, UI_CANCEL_RESERVED_OFFSET, 1);

    assert_request_bytes_error(request_reserved_a, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(request_reserved_b, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_reserved_a, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_reserved_b, MUX_UI_ERROR_INVALID);
    assert_cancel_bytes_error(cancel_reserved, MUX_UI_ERROR_INVALID);
}

static void
test_reject_wire_lengths(void)
{
    g_autoptr(GBytes) request = encode_sparse_request(0, 0);
    g_autoptr(GBytes) response = encode_sparse_response();
    g_autoptr(GBytes) too_many_choices =
        bytes_with_u16(request,
                       UI_REQUEST_CHOICE_COUNT_OFFSET,
                       MUX_UI_MAX_CHOICES + 1);
    g_autoptr(GBytes) oversized_origin =
        bytes_with_u32(request,
                       UI_REQUEST_ORIGIN_LENGTH_OFFSET,
                       G_MAXUINT32);
    g_autoptr(GBytes) oversized_choice_label =
        bytes_with_u32(request,
                       UI_SPARSE_REQUEST_CHOICE_LABEL_LENGTH_OFFSET,
                       G_MAXUINT32);
    g_autoptr(GBytes) too_many_paths =
        bytes_with_u16(response,
                       UI_RESPONSE_PATH_COUNT_OFFSET,
                       MUX_UI_MAX_PATHS + 1);
    g_autoptr(GBytes) oversized_value =
        bytes_with_u32(response,
                       UI_RESPONSE_VALUE_LENGTH_OFFSET,
                       G_MAXUINT32);
    g_autoptr(GBytes) oversized_path =
        bytes_with_u32(response,
                       UI_SPARSE_RESPONSE_PATH_LENGTH_OFFSET,
                       G_MAXUINT32);
    g_autofree guint8 *oversized_payload =
        g_malloc0((gsize)MUX_UI_MAX_PAYLOAD + 1);
    MuxUiRecordType type = 0;
    g_autoptr(GError) error = NULL;

    assert_request_bytes_error(too_many_choices, MUX_UI_ERROR_TOO_LARGE);
    assert_request_bytes_error(oversized_origin, MUX_UI_ERROR_TOO_LARGE);
    assert_request_bytes_error(oversized_choice_label,
                               MUX_UI_ERROR_TOO_LARGE);
    assert_response_bytes_error(too_many_paths, MUX_UI_ERROR_TOO_LARGE);
    assert_response_bytes_error(oversized_value, MUX_UI_ERROR_TOO_LARGE);
    assert_response_bytes_error(oversized_path, MUX_UI_ERROR_TOO_LARGE);
    g_assert_false(mux_ui_record_type(oversized_payload,
                                      (gsize)MUX_UI_MAX_PAYLOAD + 1,
                                      &type,
                                      &error));
    g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
}

static void
test_reject_encode_limits(void)
{
    {
        g_autoptr(MuxUiRequest) request =
            mux_ui_request_new(MUX_UI_REQUEST_DIALOG_ALERT);
        g_autofree gchar *too_long =
            g_strnfill((gsize)MUX_UI_MAX_MESSAGE + 1, 'x');
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;

        request->request_id = 1;
        request->message = g_steal_pointer(&too_long);
        encoded = mux_ui_request_encode(request, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
    }

    {
        g_autoptr(MuxUiRequest) request =
            mux_ui_request_new(MUX_UI_REQUEST_CONTEXT_MENU);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;
        guint i;

        request->request_id = 1;
        for (i = 0; i <= MUX_UI_MAX_CHOICES; i++)
            g_ptr_array_add(request->choices,
                            mux_ui_choice_new(i, 0, "choice"));
        encoded = mux_ui_request_encode(request, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
    }

    {
        g_autoptr(MuxUiResponse) response =
            mux_ui_response_new(1, MUX_UI_ACTION_SUBMIT);
        g_autofree gchar *too_long =
            g_strnfill((gsize)MUX_UI_MAX_VALUE + 1, 'x');
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;

        response->value = g_steal_pointer(&too_long);
        encoded = mux_ui_response_encode(response, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
    }

    {
        g_autoptr(MuxUiResponse) response =
            mux_ui_response_new(1, MUX_UI_ACTION_SUBMIT);
        g_autofree gchar *too_long =
            g_strnfill((gsize)MUX_UI_MAX_PATH + 1, 'x');
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;

        g_ptr_array_add(response->paths, g_steal_pointer(&too_long));
        encoded = mux_ui_response_encode(response, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
    }

    {
        g_autoptr(MuxUiResponse) response =
            mux_ui_response_new(1, MUX_UI_ACTION_SUBMIT);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) encoded = NULL;
        guint i;

        for (i = 0; i <= MUX_UI_MAX_PATHS; i++)
            g_ptr_array_add(response->paths, g_strdup("/tmp/item"));
        encoded = mux_ui_response_encode(response, &error);
        g_assert_null(encoded);
        g_assert_error(error, MUX_UI_ERROR, MUX_UI_ERROR_TOO_LARGE);
    }
}

static void
test_reject_invalid_strings(void)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_DIALOG_ALERT);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) encoded = NULL;
    g_autoptr(GBytes) embedded_nul = NULL;
    g_autoptr(GBytes) invalid_utf8 = NULL;

    request->request_id = 1;
    request->origin = g_strdup("x");
    encoded = mux_ui_request_encode(request, &error);
    g_assert_no_error(error);
    g_assert_nonnull(encoded);

    embedded_nul = bytes_with_u8(encoded, UI_REQUEST_ORIGIN_DATA_OFFSET, 0);
    invalid_utf8 = bytes_with_u8(encoded, UI_REQUEST_ORIGIN_DATA_OFFSET, 0xff);
    assert_request_bytes_error(embedded_nul, MUX_UI_ERROR_INVALID);
    assert_request_bytes_error(invalid_utf8, MUX_UI_ERROR_INVALID);
}

static void
test_reject_truncated_and_trailing_data(void)
{
    g_autoptr(GBytes) request = encode_representative_request();
    g_autoptr(GBytes) response = encode_representative_response();
    g_autoptr(GBytes) cancel = encode_cancel();
    g_autoptr(GBytes) request_trailing =
        bytes_with_trailing_byte(request);
    g_autoptr(GBytes) response_trailing =
        bytes_with_trailing_byte(response);
    g_autoptr(GBytes) cancel_trailing =
        bytes_with_trailing_byte(cancel);
    gsize request_length;
    gsize response_length;
    gsize cancel_length;
    const guint8 *request_data =
        g_bytes_get_data(request, &request_length);
    const guint8 *response_data =
        g_bytes_get_data(response, &response_length);
    const guint8 *cancel_data = g_bytes_get_data(cancel, &cancel_length);
    gsize length;

    assert_request_bytes_error(request_trailing, MUX_UI_ERROR_INVALID);
    assert_response_bytes_error(response_trailing, MUX_UI_ERROR_INVALID);
    assert_cancel_bytes_error(cancel_trailing, MUX_UI_ERROR_INVALID);

    for (length = 0; length < request_length; length++)
        assert_request_decode_error(request_data,
                                    length,
                                    MUX_UI_ERROR_TRUNCATED);
    for (length = 0; length < response_length; length++)
        assert_response_decode_error(response_data,
                                     length,
                                     MUX_UI_ERROR_TRUNCATED);
    for (length = 0; length < cancel_length; length++)
        assert_cancel_decode_error(cancel_data,
                                   length,
                                   MUX_UI_ERROR_TRUNCATED);
}

static void
test_action_compatibility(void)
{
    g_assert_true(mux_ui_action_is_valid(MUX_UI_REQUEST_DIALOG_PROMPT,
                                         MUX_UI_ACTION_SUBMIT));
    g_assert_true(mux_ui_action_is_valid(MUX_UI_REQUEST_DIALOG_PROMPT,
                                         MUX_UI_ACTION_CANCEL));
    g_assert_false(mux_ui_action_is_valid(MUX_UI_REQUEST_DIALOG_PROMPT,
                                          MUX_UI_ACTION_ALLOW_ONCE));
    g_assert_true(mux_ui_action_is_valid(MUX_UI_REQUEST_PERMISSION,
                                         MUX_UI_ACTION_ALLOW_ALWAYS));
    g_assert_false(mux_ui_action_is_valid(MUX_UI_REQUEST_PERMISSION,
                                          MUX_UI_ACTION_ACCEPT));
    g_assert_true(mux_ui_action_is_valid(MUX_UI_REQUEST_CRASH,
                                         MUX_UI_ACTION_RELOAD));
    g_assert_true(mux_ui_action_is_valid(MUX_UI_REQUEST_NOTIFICATION,
                                         MUX_UI_ACTION_UNSUPPORTED));
    g_assert_false(mux_ui_action_is_valid(0, MUX_UI_ACTION_UNSUPPORTED));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ui-protocol/round-trip/request",
                    test_request_round_trip);
    g_test_add_func("/ui-protocol/round-trip/response",
                    test_response_round_trip);
    g_test_add_func("/ui-protocol/round-trip/cancel",
                    test_cancel_round_trip);
    g_test_add_func("/ui-protocol/reject/header", test_reject_header_errors);
    g_test_add_func("/ui-protocol/reject/ids-and-enums",
                    test_reject_ids_and_enums);
    g_test_add_func("/ui-protocol/reject/unknown-flags",
                    test_reject_unknown_flags);
    g_test_add_func("/ui-protocol/reject/reserved-fields",
                    test_reject_nonzero_reserved_fields);
    g_test_add_func("/ui-protocol/reject/wire-lengths",
                    test_reject_wire_lengths);
    g_test_add_func("/ui-protocol/reject/encode-limits",
                    test_reject_encode_limits);
    g_test_add_func("/ui-protocol/reject/invalid-strings",
                    test_reject_invalid_strings);
    g_test_add_func("/ui-protocol/reject/truncated-and-trailing",
                    test_reject_truncated_and_trailing_data);
    g_test_add_func("/ui-protocol/action-compatibility",
                    test_action_compatibility);
    return g_test_run();
}
