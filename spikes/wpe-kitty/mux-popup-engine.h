#ifndef MUX_POPUP_ENGINE_H
#define MUX_POPUP_ENGINE_H

#include <wpe/webkit.h>

G_BEGIN_DECLS

#define MUX_POPUP_TOKEN_BYTES 16U
#define MUX_POPUP_TOKEN_LENGTH (MUX_POPUP_TOKEN_BYTES * 2U)
#define MUX_POPUP_ATTACH_TIMEOUT_MS 30000U
#define MUX_POPUP_MAX_PENDING 32U

typedef struct _MuxPopupManager MuxPopupManager;

/*
 * create_func must synchronously create a related WebKitWebView and transfers
 * one reference to the manager. Returning NULL denies the popup.
 */
typedef WebKitWebView *(*MuxPopupCreateFunc)(
    WebKitWebView *parent,
    WebKitNavigationAction *navigation_action,
    gpointer user_data,
    GError **error);

/*
 * offer_func asks muxd to launch a pane that will claim token. All arguments
 * are borrowed for the duration of the callback.
 */
typedef gboolean (*MuxPopupOfferFunc)(WebKitWebView *parent,
                                      WebKitWebView *child,
                                      const gchar *token,
                                      gpointer user_data,
                                      GError **error);

/*
 * Called for unclaimed child views. It receives the reference originally
 * returned by create_func and must tear down the engine-side view.
 */
typedef void (*MuxPopupDestroyFunc)(WebKitWebView *child,
                                    gpointer user_data);

MuxPopupManager *mux_popup_manager_new(
    WebKitWebView *parent,
    MuxPopupCreateFunc create_func,
    MuxPopupOfferFunc offer_func,
    MuxPopupDestroyFunc destroy_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_popup_manager_free(MuxPopupManager *manager);

/*
 * Consumes a ready, unexpired token and transfers the child reference to the
 * caller. Tokens are single use.
 */
WebKitWebView *mux_popup_manager_claim(MuxPopupManager *manager,
                                      const gchar *token,
                                      GError **error);

/*
 * Destroys expired, unclaimed children. Returns the number removed.
 */
guint mux_popup_manager_tick(MuxPopupManager *manager,
                             gint64 monotonic_us);

guint mux_popup_manager_pending_count(const MuxPopupManager *manager);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxPopupManager,
                              mux_popup_manager_free)

G_END_DECLS

#endif
