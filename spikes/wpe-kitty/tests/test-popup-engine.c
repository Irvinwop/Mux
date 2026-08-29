#define MUX_POPUP_ENGINE_TEST

#include "../mux-popup-engine.c"

typedef struct {
    guint offers;
    guint destroys;
    gboolean offer_succeeds;
    WebKitWebView *expected_parent;
    WebKitWebView *expected_child;
    gchar *offered_token;
} PopupCapture;

static gboolean
capture_offer(WebKitWebView *parent,
              WebKitWebView *child,
              const gchar *token,
              gpointer user_data,
              GError **error)
{
    PopupCapture *capture = user_data;

    capture->offers++;
    g_assert_true(parent == capture->expected_parent);
    g_assert_true(child == capture->expected_child);
    g_free(capture->offered_token);
    capture->offered_token = g_strdup(token);
    if (!capture->offer_succeeds) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "trusted UI rejected popup");
        return FALSE;
    }
    return TRUE;
}

static void
capture_destroy(WebKitWebView *child, gpointer user_data)
{
    PopupCapture *capture = user_data;

    g_assert_true(child == capture->expected_child);
    capture->destroys++;
}

static void
test_manager_init(MuxPopupManager *manager, PopupCapture *capture)
{
    memset(manager, 0, sizeof(*manager));
    manager->parent = capture->expected_parent;
    manager->by_token = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        NULL,
        (GDestroyNotify)popup_record_free);
    manager->by_child =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    manager->offer_func = capture_offer;
    manager->destroy_func = capture_destroy;
    manager->user_data = capture;
}

static void
test_manager_clear(MuxPopupManager *manager)
{
    g_hash_table_remove_all(manager->by_child);
    g_clear_pointer(&manager->by_token, g_hash_table_unref);
    g_clear_pointer(&manager->by_child, g_hash_table_unref);
}

static PopupRecord *
add_record(MuxPopupManager *manager, WebKitWebView *child)
{
    PopupRecord *record = g_new0(PopupRecord, 1);

    record->manager = manager;
    record->token =
        g_strdup("0123456789abcdef0123456789abcdef");
    record->child = child;
    record->expires_at_us =
        g_get_monotonic_time() + G_TIME_SPAN_MINUTE;
    g_hash_table_insert(manager->by_child, child, record);
    g_hash_table_insert(manager->by_token, record->token, record);
    return record;
}

static void
test_admission_requires_current_gesture(void)
{
    g_assert_false(popup_request_is_admissible(FALSE, 0, 0));
    g_assert_false(popup_request_is_admissible(FALSE,
                                               MUX_POPUP_MAX_PENDING - 1,
                                               0));
    g_assert_true(popup_request_is_admissible(TRUE, 0, 0));
}

static void
test_admission_bounds_pending_and_reentrant_creation(void)
{
    g_assert_false(popup_request_is_admissible(
        TRUE, MUX_POPUP_MAX_PENDING, 0));
    g_assert_false(popup_request_is_admissible(
        TRUE, MUX_POPUP_MAX_PENDING - 1, 1));
    g_assert_false(popup_request_is_admissible(
        TRUE, 0, MUX_POPUP_MAX_PENDING));
    g_assert_true(popup_request_is_admissible(
        TRUE, MUX_POPUP_MAX_PENDING - 1, 0));
}

static void
test_ready_popup_is_offered_and_claimed_once(void)
{
    PopupCapture capture = {0};
    MuxPopupManager manager;
    PopupRecord *record;
    WebKitWebView *claimed;
    g_autoptr(GError) error = NULL;
    guint parent_marker;
    guint child_marker;

    capture.offer_succeeds = TRUE;
    capture.expected_parent = (WebKitWebView *)&parent_marker;
    capture.expected_child = (WebKitWebView *)&child_marker;
    test_manager_init(&manager, &capture);
    record = add_record(&manager, capture.expected_child);

    on_child_ready(capture.expected_child, record);
    on_child_ready(capture.expected_child, record);
    g_assert_cmpuint(capture.offers, ==, 1);
    g_assert_cmpuint(mux_popup_manager_pending_count(&manager), ==, 1);
    claimed = mux_popup_manager_claim(&manager,
                                      capture.offered_token,
                                      &error);
    g_assert_no_error(error);
    g_assert_true(claimed == capture.expected_child);
    g_assert_cmpuint(mux_popup_manager_pending_count(&manager), ==, 0);
    g_assert_cmpuint(capture.destroys, ==, 0);

    test_manager_clear(&manager);
    g_free(capture.offered_token);
}

static void
test_rejected_offer_destroys_untrusted_child(void)
{
    PopupCapture capture = {0};
    MuxPopupManager manager;
    PopupRecord *record;
    guint parent_marker;
    guint child_marker;

    capture.offer_succeeds = FALSE;
    capture.expected_parent = (WebKitWebView *)&parent_marker;
    capture.expected_child = (WebKitWebView *)&child_marker;
    test_manager_init(&manager, &capture);
    record = add_record(&manager, capture.expected_child);

    g_test_expect_message(NULL,
                          G_LOG_LEVEL_WARNING,
                          "*could not place popup pane:*");
    on_child_ready(capture.expected_child, record);
    g_test_assert_expected_messages();
    g_assert_cmpuint(capture.offers, ==, 1);
    g_assert_cmpuint(capture.destroys, ==, 1);
    g_assert_cmpuint(mux_popup_manager_pending_count(&manager), ==, 0);

    test_manager_clear(&manager);
    g_free(capture.offered_token);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/popup/admission-requires-gesture",
                    test_admission_requires_current_gesture);
    g_test_add_func("/popup/admission-bounds-reentrancy",
                    test_admission_bounds_pending_and_reentrant_creation);
    g_test_add_func("/popup/trusted-offer-single-claim",
                    test_ready_popup_is_offered_and_claimed_once);
    g_test_add_func("/popup/rejected-offer-destroyed",
                    test_rejected_offer_destroys_untrusted_child);
    return g_test_run();
}
