#ifndef MUX_NOTIFICATION_PANE_H
#define MUX_NOTIFICATION_PANE_H

#include "mux-ui-protocol.h"

G_BEGIN_DECLS

typedef struct _MuxNotificationPane MuxNotificationPane;

typedef gboolean (*MuxNotificationPaneSendFunc)(GBytes *payload,
                                                 gpointer user_data,
                                                 GError **error);
typedef gboolean (*MuxNotificationPaneWriteFunc)(const guint8 *data,
                                                  gsize length,
                                                  gpointer user_data,
                                                  GError **error);

MuxNotificationPane *mux_notification_pane_new(
    MuxNotificationPaneSendFunc send_func,
    MuxNotificationPaneWriteFunc write_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

void mux_notification_pane_free(MuxNotificationPane *pane);

gboolean mux_notification_pane_handle_payload(MuxNotificationPane *pane,
                                              const guint8 *data,
                                              gsize length,
                                              gboolean *consumed,
                                              GError **error);

gboolean mux_notification_pane_handle_osc(MuxNotificationPane *pane,
                                          const guint8 *data,
                                          gsize length,
                                          GError **error);

guint mux_notification_pane_pending_count(const MuxNotificationPane *pane);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MuxNotificationPane,
                              mux_notification_pane_free)

G_END_DECLS

#endif
