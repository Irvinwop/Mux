#ifndef MUX_DOWNLOAD_ENGINE_H
#define MUX_DOWNLOAD_ENGINE_H

#include "mux-ui-protocol.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxDownloadManager MuxDownloadManager;

#define MUX_DOWNLOAD_MAX_PENDING_PER_VIEW 2U
#define MUX_DOWNLOAD_MAX_PENDING_GLOBAL 8U
#define MUX_DOWNLOAD_MAX_ACTIVE_PER_VIEW 4U
#define MUX_DOWNLOAD_MAX_ACTIVE_GLOBAL 16U
#define MUX_DOWNLOAD_DESTINATION_TIMEOUT_MS 120000U

typedef enum {
    MUX_DOWNLOAD_EVENT_STARTED,
    MUX_DOWNLOAD_EVENT_DESTINATION,
    MUX_DOWNLOAD_EVENT_PROGRESS,
    MUX_DOWNLOAD_EVENT_FINISHED,
    MUX_DOWNLOAD_EVENT_FAILED,
    MUX_DOWNLOAD_EVENT_CANCELLED,
} MuxDownloadEventType;

typedef struct {
    MuxDownloadEventType type;
    guint64 download_id;
    WebKitWebView *source_view;
    const gchar *path;
    const gchar *message;
    guint64 received_bytes;
    gdouble estimated_progress;
} MuxDownloadEvent;

/*
 * The source view identifies the pane that should receive the destination
 * request. It may be NULL for a download started directly by NetworkSession.
 */
typedef gboolean (*MuxDownloadSendFunc)(WebKitWebView *source_view,
                                        GBytes *payload,
                                        gpointer user_data,
                                        GError **error);

typedef void (*MuxDownloadEventFunc)(const MuxDownloadEvent *event,
                                     gpointer user_data);

typedef gboolean (*MuxDownloadClipboardFunc)(WebKitWebView *source_view,
                                              const gchar *path,
                                              const gchar *mime_type,
                                              gpointer user_data,
                                              GError **error);

MuxDownloadManager *mux_download_manager_new(
    WebKitNetworkSession *network_session,
    MuxDownloadSendFunc send_func,
    MuxDownloadEventFunc event_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_download_manager_free(MuxDownloadManager *manager);

void mux_download_manager_set_clipboard_func(
    MuxDownloadManager *manager,
    MuxDownloadClipboardFunc clipboard_func);

gboolean mux_download_manager_download_uri_to_clipboard(
    MuxDownloadManager *manager,
    WebKitWebView *source_view,
    const gchar *uri,
    GError **error);

/*
 * Handles a mux-ui response or cancel record routed from any pane.
 * Unknown and late IDs are ignored.
 */
gboolean mux_download_manager_handle_payload(MuxDownloadManager *manager,
                                             const guint8 *data,
                                             gsize length,
                                             GError **error);

void mux_download_manager_cancel(MuxDownloadManager *manager,
                                 guint64 download_id);
void mux_download_manager_cancel_view(MuxDownloadManager *manager,
                                      WebKitWebView *source_view);
void mux_download_manager_cancel_all(MuxDownloadManager *manager);
guint mux_download_manager_count(const MuxDownloadManager *manager);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxDownloadManager,
                              mux_download_manager_free)

G_END_DECLS

#endif
