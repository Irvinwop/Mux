#ifndef MUX_NOTIFICATION_ENGINE_H
#define MUX_NOTIFICATION_ENGINE_H

#include "mux-ui-protocol.h"

#include <wpe/webkit.h>

G_BEGIN_DECLS

typedef struct _MuxNotificationEngine MuxNotificationEngine;

typedef gboolean (*MuxNotificationEngineSendFunc)(GBytes *payload,
                                                   gpointer user_data,
                                                   GError **error);

MuxNotificationEngine *mux_notification_engine_new(
    WebKitWebView *web_view,
    gboolean private_profile,
    MuxNotificationEngineSendFunc send_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_notification_engine_free(MuxNotificationEngine *engine);

gboolean mux_notification_engine_handle_payload(
    MuxNotificationEngine *engine,
    const guint8 *data,
    gsize length,
    GError **error);

guint mux_notification_engine_pending_count(
    const MuxNotificationEngine *engine);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNotificationEngine,
                              mux_notification_engine_free)

G_END_DECLS

#endif
