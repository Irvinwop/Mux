#include "mux-notification-pane.h"

#include <string.h>

typedef struct {
    MuxNotificationPane *pane;
    GPtrArray *writes;
    GArray *actions;
    guint destroyed;
    gboolean dispose_on_write;
    gboolean dispose_on_send;
    gboolean cancel_on_write;
    gboolean replace_on_write;
    gboolean replace_on_close_write;
    gboolean replace_on_send;
    gboolean fail_write;
} Fixture;

static gboolean post_notification(Fixture *fixture, const gchar *title,
                                  const gchar *body, guint32 flags,
                                  GError **error);
static gboolean cancel_notification(Fixture *fixture);

static void
dispose_owner(Fixture *fixture)
{
    mux_notification_pane_free(g_steal_pointer(&fixture->pane));
}

static void
on_destroy(gpointer user_data)
{
    Fixture *fixture = user_data;

    fixture->destroyed++;
}

static gboolean
on_send(GBytes *payload, gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;
    g_autoptr(MuxUiResponse) response = NULL;
    gsize length;
    const guint8 *data = g_bytes_get_data(payload, &length);

    g_assert_true(mux_ui_response_decode(data, length, &response, error));
    g_assert_cmpuint(response->request_id, ==, 1);
    g_array_append_val(fixture->actions, response->action);
    if (fixture->dispose_on_send) {
        fixture->dispose_on_send = FALSE;
        dispose_owner(fixture);
        g_assert_cmpuint(fixture->destroyed, ==, 0);
    } else if (fixture->replace_on_send) {
        fixture->replace_on_send = FALSE;
        g_assert_true(post_notification(fixture, "replacement", NULL, 0, NULL));
    }
    return TRUE;
}

static gboolean
on_write(const guint8 *data, gsize length, gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;
    gchar *sequence = g_strndup((const gchar *)data, length);
    gboolean closing = strstr(sequence, "p=close;") != NULL;

    g_assert_cmpuint(fixture->destroyed, ==, 0);
    g_ptr_array_add(fixture->writes, sequence);
    if (fixture->fail_write) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
                            "terminal disconnected");
        return FALSE;
    }
    if (fixture->dispose_on_write && !closing) {
        fixture->dispose_on_write = FALSE;
        dispose_owner(fixture);
        g_assert_cmpuint(fixture->destroyed, ==, 0);
    } else if (fixture->cancel_on_write && !closing) {
        fixture->cancel_on_write = FALSE;
        g_assert_true(cancel_notification(fixture));
    } else if (fixture->replace_on_write && !closing) {
        fixture->replace_on_write = FALSE;
        g_assert_true(post_notification(fixture, "replacement", NULL, 0, NULL));
    } else if (fixture->replace_on_close_write && closing) {
        fixture->replace_on_close_write = FALSE;
        g_assert_true(post_notification(fixture, "replacement", NULL, 0, NULL));
    }
    return TRUE;
}

static void
fixture_init(Fixture *fixture)
{
    *fixture = (Fixture) {
        .writes = g_ptr_array_new_with_free_func(g_free),
        .actions = g_array_new(FALSE, FALSE, sizeof(MuxUiAction)),
    };
    fixture->pane = mux_notification_pane_new(on_send, on_write,
                                              fixture, on_destroy);
}

static void
fixture_clear(Fixture *fixture)
{
    dispose_owner(fixture);
    g_assert_cmpuint(fixture->destroyed, ==, 1);
    g_ptr_array_unref(fixture->writes);
    g_array_unref(fixture->actions);
}

static gboolean
post_notification(Fixture *fixture, const gchar *title, const gchar *body,
                  guint32 flags, GError **error)
{
    g_autoptr(MuxUiRequest) request =
        mux_ui_request_new(MUX_UI_REQUEST_NOTIFICATION);
    g_autoptr(GBytes) payload = NULL;
    gboolean consumed = FALSE;
    gboolean result;
    gsize length;
    const guint8 *data;

    request->request_id = 1;
    request->flags = flags;
    request->origin = g_strdup("https://example.com");
    request->heading = g_strdup(title);
    request->message = g_strdup(body);
    payload = mux_ui_request_encode(request, NULL);
    g_assert_nonnull(payload);
    data = g_bytes_get_data(payload, &length);
    result = mux_notification_pane_handle_payload(fixture->pane, data, length,
                                                  &consumed, error);
    g_assert_true(consumed);
    return result;
}

static gboolean
cancel_notification(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) payload = mux_ui_cancel_encode(
        1, MUX_UI_CANCEL_UNDERLYING_GONE, &error);
    gboolean consumed = FALSE;
    gboolean result;
    gsize length;
    const guint8 *data;

    g_assert_no_error(error);
    data = g_bytes_get_data(payload, &length);
    result = mux_notification_pane_handle_payload(fixture->pane, data, length,
                                                  &consumed, &error);
    g_assert_no_error(error);
    g_assert_true(consumed);
    return result;
}

static gboolean
terminal_response(Fixture *fixture, gboolean close)
{
    g_autoptr(GError) error = NULL;
    const gchar *response = close
        ? "\033]99;i=muxn-0000000000000001:p=close;\033\\"
        : "\033]99;i=muxn-0000000000000001;\033\\";
    gboolean result = mux_notification_pane_handle_osc(
        fixture->pane, (const guint8 *)response, strlen(response), &error);

    g_assert_no_error(error);
    return result;
}

static void
test_click_and_close(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "title", "body", 0, NULL));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 1);
    g_assert_true(terminal_response(&fixture, FALSE));
    g_assert_true(terminal_response(&fixture, FALSE));
    g_assert_cmpuint(fixture.actions->len, ==, 1);
    g_assert_cmpint(g_array_index(fixture.actions, MuxUiAction, 0), ==,
                    MUX_UI_ACTION_ACKNOWLEDGE);
    g_assert_true(terminal_response(&fixture, TRUE));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 0);
    g_assert_cmpuint(fixture.actions->len, ==, 2);
    g_assert_cmpint(g_array_index(fixture.actions, MuxUiAction, 1), ==,
                    MUX_UI_ACTION_CANCEL);
    g_assert_true(terminal_response(&fixture, TRUE));
    g_assert_cmpuint(fixture.actions->len, ==, 2);
    fixture_clear(&fixture);
}

static void
test_private_notification(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "private", "secret",
                                    MUX_UI_REQUEST_FLAG_PRIVATE_PROFILE, NULL));
    g_assert_cmpuint(fixture.writes->len, ==, 0);
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 0);
    g_assert_cmpuint(fixture.actions->len, ==, 1);
    g_assert_cmpint(g_array_index(fixture.actions, MuxUiAction, 0), ==,
                    MUX_UI_ACTION_UNSUPPORTED);
    fixture_clear(&fixture);
}

static void
test_dispose_during_write(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    fixture.dispose_on_write = TRUE;
    g_assert_false(post_notification(&fixture, "title", "body", 0, NULL));
    g_assert_null(fixture.pane);
    g_assert_cmpuint(fixture.destroyed, ==, 1);
    g_assert_cmpuint(fixture.actions->len, ==, 0);
    g_assert_cmpuint(fixture.writes->len, ==, 2);
    g_assert_nonnull(strstr(g_ptr_array_index(fixture.writes, 1), "p=close;"));
    fixture_clear(&fixture);
}

static void
test_dispose_during_send(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "title", NULL, 0, NULL));
    fixture.dispose_on_send = TRUE;
    g_assert_false(terminal_response(&fixture, FALSE));
    g_assert_null(fixture.pane);
    g_assert_cmpuint(fixture.destroyed, ==, 1);
    g_assert_cmpuint(fixture.actions->len, ==, 1);
    fixture_clear(&fixture);
}

static void
test_replace_during_close_response(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "title", NULL, 0, NULL));
    fixture.replace_on_send = TRUE;
    g_assert_true(terminal_response(&fixture, TRUE));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 1);
    g_assert_true(terminal_response(&fixture, FALSE));
    g_assert_cmpuint(fixture.actions->len, ==, 2);
    g_assert_cmpint(g_array_index(fixture.actions, MuxUiAction, 1), ==,
                    MUX_UI_ACTION_ACKNOWLEDGE);
    fixture_clear(&fixture);
}

static void
test_cancel_during_publication(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    fixture.cancel_on_write = TRUE;
    g_assert_true(post_notification(&fixture, "title", "stale body", 0, NULL));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 0);
    g_assert_cmpuint(fixture.writes->len, ==, 2);
    g_assert_cmpuint(fixture.actions->len, ==, 0);
    fixture_clear(&fixture);
}

static void
test_replace_during_publication(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    fixture.replace_on_write = TRUE;
    g_assert_true(post_notification(&fixture, "title", "stale body", 0, NULL));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 1);
    g_assert_cmpuint(fixture.writes->len, ==, 2);
    g_assert_cmpuint(fixture.actions->len, ==, 0);
    fixture_clear(&fixture);
}

static void
test_replace_during_cancellation(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "title", NULL, 0, NULL));
    fixture.replace_on_close_write = TRUE;
    g_assert_true(cancel_notification(&fixture));
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 1);
    fixture_clear(&fixture);
}

static void
test_write_error(void)
{
    Fixture fixture;
    g_autoptr(GError) error = NULL;

    fixture_init(&fixture);
    fixture.fail_write = TRUE;
    g_assert_false(post_notification(&fixture, "title", NULL, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
    g_assert_cmpuint(mux_notification_pane_pending_count(fixture.pane), ==, 0);
    g_assert_cmpuint(fixture.actions->len, ==, 1);
    g_assert_cmpint(g_array_index(fixture.actions, MuxUiAction, 0), ==,
                    MUX_UI_ACTION_UNSUPPORTED);
    fixture_clear(&fixture);
}

static void
test_public_autoptr_disposes(void)
{
    Fixture fixture;

    fixture_init(&fixture);
    g_assert_true(post_notification(&fixture, "title", NULL, 0, NULL));
    {
        g_autoptr(MuxNotificationPane) owner = g_steal_pointer(&fixture.pane);

        g_assert_nonnull(owner);
        g_assert_cmpuint(fixture.destroyed, ==, 0);
    }
    g_assert_cmpuint(fixture.destroyed, ==, 1);
    g_assert_cmpuint(fixture.writes->len, ==, 2);
    fixture_clear(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/notification/click-and-close", test_click_and_close);
    g_test_add_func("/notification/private", test_private_notification);
    g_test_add_func("/notification/dispose-during-write", test_dispose_during_write);
    g_test_add_func("/notification/dispose-during-send", test_dispose_during_send);
    g_test_add_func("/notification/replace-during-close-response", test_replace_during_close_response);
    g_test_add_func("/notification/cancel-during-publication", test_cancel_during_publication);
    g_test_add_func("/notification/replace-during-publication", test_replace_during_publication);
    g_test_add_func("/notification/replace-during-cancellation", test_replace_during_cancellation);
    g_test_add_func("/notification/write-error", test_write_error);
    g_test_add_func("/notification/public-autoptr", test_public_autoptr_disposes);
    return g_test_run();
}
